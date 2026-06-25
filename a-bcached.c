/*
 * a-bcached - HDD spin management daemon for bcache
 *
 * Monitors bcache backing devices and manages HDD spin-down to maximize
 * HDD lifespan. Coalesces I/O operations, defers writeback flushes,
 * and controls HDD spin state transitions.
 *
 * Copyright 2026, a-bcache contributors
 * GPLv2
 */

#define _FILE_OFFSET_BITS	64
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/*
 * --------------------------------------------------------------------------
 *  Configuration defaults
 * --------------------------------------------------------------------------
 */

/* Seconds of HDD inactivity before issuing spin-down */
#define DEFAULT_SPINDOWN_DELAY_SECS	300

/* Minimum dirty data (MB) to accumulate before allowing writeback to HDD */
#define DEFAULT_MIN_WRITEBACK_MB	64

/* How often (seconds) the daemon polls bcache sysfs stats */
#define DEFAULT_POLL_INTERVAL_SECS	5

/* Maximum spin-up/spin-down transitions per hour before throttling */
#define DEFAULT_MAX_SPIN_CYCLES_HOUR	4

/* Writeback rate limit (sectors/sec) when we do flush -- 0 = kernel default */
#define DEFAULT_WRITEBACK_RATE		0

/*
 * --------------------------------------------------------------------------
 *  Runtime state
 * --------------------------------------------------------------------------
 */

struct spin_state {
	bool		is_spinning;		/* true if HDD platters are spinning */
	time_t		last_io_time;		/* last time I/O was sent to backing dev */
	time_t		last_spindown_time;	/* last time we issued spin-down */
	time_t		last_spinup_time;	/* last time HDD spun up */
	unsigned	spin_cycles_this_hour;	/* spin transitions in current hour */
	time_t		hour_start;		/* start of current hour window */
};

struct abcache_config {
	unsigned	spindown_delay_secs;
	unsigned	min_writeback_mb;
	unsigned	poll_interval_secs;
	unsigned	max_spin_cycles_hour;
	unsigned	writeback_rate;
	bool		dry_run;		/* don't actually issue commands */
	bool		foreground;		/* don't daemonize */
	int		log_level;		/* 0=quiet, 1=normal, 2=debug */
};

struct bcache_device {
	char		sysfs_path[PATH_MAX];	/* /sys/block/bcache0/bcache */
	char		backing_dev[PATH_MAX];	/* /dev/sdb */
	char		cache_set_uuid[40];
	struct spin_state spin;
};

static volatile sig_atomic_t running = 1;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

/*
 * --------------------------------------------------------------------------
 *  Sysfs helpers -- read bcache state from /sys
 * --------------------------------------------------------------------------
 */

static int sysfs_read_str(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;

	n = read(fd, buf, len - 1);
	close(fd);

	if (n < 0)
		return -errno;

	buf[n] = '\0';
	/* strip trailing newline */
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	return 0;
}

static int sysfs_read_uint(const char *path, unsigned *val)
{
	char buf[64];
	int ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;
	*val = (unsigned)strtoul(buf, NULL, 10);
	return 0;
}

static int sysfs_read_uint64(const char *path, uint64_t *val)
{
	char buf[64];
	int ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;
	*val = strtoull(buf, NULL, 10);
	return 0;
}

static int sysfs_write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;

	n = write(fd, val, strlen(val));
	close(fd);

	return n < 0 ? -errno : 0;
}

static int sysfs_write_uint(const char *path, unsigned val)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%u", val);
	return sysfs_write_str(path, buf);
}

/*
 * --------------------------------------------------------------------------
 *  Bcache state queries
 * --------------------------------------------------------------------------
 */

/* Read dirty data size for a backing device (in bytes, from sysfs) */
static int bcache_get_dirty_data_bytes(const char *sysfs_path, uint64_t *bytes)
{
	char path[PATH_MAX];
	char buf[64];
	int ret;
	double val;
	char unit;

	snprintf(path, sizeof(path), "%s/dirty_data", sysfs_path);
	ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;

	/* bcache reports dirty_data as e.g. "1.2M" or "512k" or "0.0k" */
	if (sscanf(buf, "%lf%c", &val, &unit) < 1) {
		*bytes = 0;
		return 0;
	}

	switch (unit) {
	case 'T': case 't': val *= 1024;  /* fall through */
	case 'G': case 'g': val *= 1024;  /* fall through */
	case 'M': case 'm': val *= 1024;  /* fall through */
	case 'K': case 'k': val *= 1024; break;
	default: break;
	}

	*bytes = (uint64_t)val;
	return 0;
}

/* Read the cache mode (writethrough, writeback, etc.) */
static int bcache_get_cache_mode(const char *sysfs_path, char *mode, size_t len)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/cache_mode", sysfs_path);
	return sysfs_read_str(path, mode, len);
}

/* Check the device state (no cache, clean, dirty, inconsistent) */
static int bcache_get_state(const char *sysfs_path, char *state, size_t len)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/state", sysfs_path);
	return sysfs_read_str(path, state, len);
}

/* Get/set the writeback rate (sectors/sec) */
static int bcache_get_writeback_rate(const char *sysfs_path, unsigned *rate)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/writeback_rate", sysfs_path);
	return sysfs_read_uint(path, rate);
}

static int bcache_set_writeback_rate(const char *sysfs_path, unsigned rate)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/writeback_rate", sysfs_path);
	return sysfs_write_uint(path, rate);
}

/* Get/set writeback_running (0 = pause writeback, 1 = allow) */
static int bcache_set_writeback_running(const char *sysfs_path, unsigned val)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/writeback_running", sysfs_path);
	return sysfs_write_uint(path, val);
}

/*
 * --------------------------------------------------------------------------
 *  HDD spin management via hdparm / ATA
 * --------------------------------------------------------------------------
 */

/*
 * Issue HDD standby (spin-down) command.
 * Uses hdparm -y (standby) or hdparm -Y (sleep).
 * Standby is preferred as the drive will auto-spin-up on next I/O.
 */
static int hdd_spindown(const char *dev, bool dry_run)
{
	char cmd[PATH_MAX + 32];

	if (dry_run) {
		fprintf(stderr, "[DRY-RUN] Would spin down %s\n", dev);
		return 0;
	}

	snprintf(cmd, sizeof(cmd), "hdparm -y %s > /dev/null 2>&1", dev);
	return system(cmd);
}

/*
 * Check if HDD is in standby (spun down) or active (spinning).
 * hdparm -C returns "drive state is: standby" or "drive state is: active/idle"
 * Returns: 1 = spinning, 0 = standby, -1 = error
 */
static int hdd_check_spinning(const char *dev)
{
	char cmd[PATH_MAX + 64];
	char buf[256];
	FILE *fp;

	snprintf(cmd, sizeof(cmd), "hdparm -C %s 2>/dev/null", dev);
	fp = popen(cmd, "r");
	if (!fp)
		return -1;

	while (fgets(buf, sizeof(buf), fp)) {
		if (strstr(buf, "standby")) {
			pclose(fp);
			return 0;
		}
		if (strstr(buf, "active") || strstr(buf, "idle")) {
			pclose(fp);
			return 1;
		}
	}

	pclose(fp);
	return -1;
}

/*
 * --------------------------------------------------------------------------
 *  I/O activity monitoring via /sys/block/<dev>/stat
 * --------------------------------------------------------------------------
 *
 *  /sys/block/sdb/stat has fields:
 *    read_ios read_merges read_sectors read_ticks
 *    write_ios write_merges write_sectors write_ticks
 *    in_flight io_ticks time_in_queue
 *
 *  We track read_ios + write_ios to detect any I/O to the backing device.
 */

struct io_stats {
	uint64_t read_ios;
	uint64_t write_ios;
	uint64_t read_sectors;
	uint64_t write_sectors;
};

static int read_io_stats(const char *block_dev, struct io_stats *stats)
{
	char path[PATH_MAX];
	char buf[512];
	char *devname;
	int ret;

	/* Extract device name from /dev/sdX -> sdX */
	devname = strrchr(block_dev, '/');
	devname = devname ? devname + 1 : (char *)block_dev;

	snprintf(path, sizeof(path), "/sys/block/%s/stat", devname);
	ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;

	memset(stats, 0, sizeof(*stats));
	sscanf(buf, "%lu %*u %lu %*u %lu %*u %lu",
	       &stats->read_ios, &stats->read_sectors,
	       &stats->write_ios, &stats->write_sectors);

	return 0;
}

/*
 * --------------------------------------------------------------------------
 *  Core daemon logic
 * --------------------------------------------------------------------------
 */

static void log_msg(int level, int cfg_level, const char *fmt, ...)
{
	va_list ap;
	if (level > cfg_level)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

#define LOG_NORMAL	1
#define LOG_DEBUG	2

/*
 * Main spin management loop for a single bcache backing device.
 *
 * Strategy:
 *   1. If HDD is spinning and idle for > spindown_delay: spin it down
 *   2. If HDD is spun down: pause writeback (prevent kernel from waking HDD)
 *   3. If dirty data exceeds threshold: allow writeback, let HDD spin up,
 *      wait for flush to complete, then schedule spin-down
 *   4. Track spin cycles per hour; if exceeding limit, extend spin-down delay
 */
static void manage_device(struct bcache_device *dev,
			  struct abcache_config *cfg)
{
	uint64_t dirty_bytes = 0;
	struct io_stats prev_stats, cur_stats;
	time_t now = time(NULL);
	char state[64];
	int spinning;

	/* Read bcache state */
	if (bcache_get_dirty_data_bytes(dev->sysfs_path, &dirty_bytes))
		return;
	if (bcache_get_state(dev->sysfs_path, state, sizeof(state)))
		return;

	/* Read I/O stats to detect activity */
	if (read_io_stats(dev->backing_dev, &cur_stats))
		return;

	/* Check current spin state */
	spinning = hdd_check_spinning(dev->backing_dev);
	if (spinning < 0)
		return;
	dev->spin.is_spinning = (spinning == 1);

	/* Reset hourly spin cycle counter */
	if (now - dev->spin.hour_start >= 3600) {
		dev->spin.spin_cycles_this_hour = 0;
		dev->spin.hour_start = now;
	}

	uint64_t dirty_mb = dirty_bytes / (1024 * 1024);

	if (dev->spin.is_spinning) {
		/*
		 * HDD is spinning.
		 * Check if it's been idle long enough to spin down.
		 */
		time_t idle_secs = now - dev->spin.last_io_time;

		if (idle_secs >= cfg->spindown_delay_secs &&
		    dirty_mb < cfg->min_writeback_mb) {
			/*
			 * Idle and not much dirty data -- spin down.
			 * First pause writeback so kernel doesn't wake it.
			 */
			log_msg(LOG_NORMAL, cfg->log_level,
				"[a-bcached] %s: idle %lds, dirty %luMB "
				"-- spinning down\n",
				dev->backing_dev, idle_secs, dirty_mb);

			bcache_set_writeback_running(dev->sysfs_path, 0);

			if (!cfg->dry_run)
				hdd_spindown(dev->backing_dev, cfg->dry_run);

			dev->spin.is_spinning = false;
			dev->spin.last_spindown_time = now;
			dev->spin.spin_cycles_this_hour++;
		} else if (dirty_mb > 0) {
			/*
			 * HDD is spinning and there's dirty data.
			 * Let writeback proceed (it writes sequentially).
			 */
			bcache_set_writeback_running(dev->sysfs_path, 1);
			dev->spin.last_io_time = now;

			log_msg(LOG_DEBUG, cfg->log_level,
				"[a-bcached] %s: spinning, dirty %luMB "
				"-- writeback active\n",
				dev->backing_dev, dirty_mb);
		}
	} else {
		/*
		 * HDD is spun down.
		 * Keep writeback paused unless dirty data is critical.
		 */
		if (dirty_mb >= cfg->min_writeback_mb) {
			/*
			 * Too much dirty data -- we need to flush.
			 * Check spin cycle budget.
			 */
			if (dev->spin.spin_cycles_this_hour >=
			    cfg->max_spin_cycles_hour) {
				log_msg(LOG_NORMAL, cfg->log_level,
					"[a-bcached] %s: dirty %luMB but "
					"spin cycle budget exhausted (%u/%u) "
					"-- deferring\n",
					dev->backing_dev, dirty_mb,
					dev->spin.spin_cycles_this_hour,
					cfg->max_spin_cycles_hour);
				return;
			}

			log_msg(LOG_NORMAL, cfg->log_level,
				"[a-bcached] %s: dirty %luMB >= %uMB "
				"threshold -- enabling writeback (HDD will "
				"spin up)\n",
				dev->backing_dev, dirty_mb,
				cfg->min_writeback_mb);

			/* Set writeback rate if configured */
			if (cfg->writeback_rate)
				bcache_set_writeback_rate(dev->sysfs_path,
							 cfg->writeback_rate);

			bcache_set_writeback_running(dev->sysfs_path, 1);
			dev->spin.last_io_time = now;
			dev->spin.last_spinup_time = now;
			dev->spin.spin_cycles_this_hour++;
		} else {
			/* Keep writeback paused, HDD stays asleep */
			bcache_set_writeback_running(dev->sysfs_path, 0);

			log_msg(LOG_DEBUG, cfg->log_level,
				"[a-bcached] %s: spun down, dirty %luMB "
				"< %uMB -- holding\n",
				dev->backing_dev, dirty_mb,
				cfg->min_writeback_mb);
		}
	}
}

/*
 * --------------------------------------------------------------------------
 *  Device discovery
 * --------------------------------------------------------------------------
 */

static int discover_bcache_devices(struct bcache_device *devs, int max_devs)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;
	char path[PATH_MAX];
	char label[PATH_MAX];

	dir = opendir("/sys/block");
	if (!dir)
		return 0;

	while ((entry = readdir(dir)) && count < max_devs) {
		if (strncmp(entry->d_name, "bcache", 6) != 0)
			continue;

		snprintf(path, sizeof(path),
			 "/sys/block/%s/bcache", entry->d_name);

		struct stat st;
		if (stat(path, &st) || !S_ISDIR(st.st_mode))
			continue;

		strncpy(devs[count].sysfs_path, path,
			sizeof(devs[count].sysfs_path));

		/* Try to read the backing device path */
		snprintf(path, sizeof(path),
			 "/sys/block/%s/bcache/backing_dev_name",
			 entry->d_name);

		if (sysfs_read_str(path, label, sizeof(label)) == 0)
			snprintf(devs[count].backing_dev,
				 sizeof(devs[count].backing_dev),
				 "/dev/%s", label);

		memset(&devs[count].spin, 0, sizeof(struct spin_state));
		devs[count].spin.hour_start = time(NULL);
		devs[count].spin.last_io_time = time(NULL);

		count++;
	}

	closedir(dir);
	return count;
}

/*
 * --------------------------------------------------------------------------
 *  Main
 * --------------------------------------------------------------------------
 */

static void usage(void)
{
	fprintf(stderr,
		"Usage: a-bcached [options]\n"
		"\n"
		"HDD spin management daemon for bcache.\n"
		"Maximizes HDD lifespan by coalescing I/O and managing\n"
		"spin-down/spin-up transitions.\n"
		"\n"
		"Options:\n"
		"  -s, --spindown-delay SECS   Idle time before spin-down "
			"(default: %d)\n"
		"  -m, --min-writeback MB      Min dirty data before flush "
			"(default: %d)\n"
		"  -p, --poll-interval SECS    Polling interval "
			"(default: %d)\n"
		"  -c, --max-spin-cycles N     Max spin transitions/hour "
			"(default: %d)\n"
		"  -r, --writeback-rate RATE   Writeback rate in sectors/sec "
			"(default: kernel)\n"
		"  -n, --dry-run               Don't issue actual commands\n"
		"  -f, --foreground            Don't daemonize\n"
		"  -v, --verbose               Debug logging\n"
		"  -q, --quiet                 No logging\n"
		"  -h, --help                  Show this help\n",
		DEFAULT_SPINDOWN_DELAY_SECS,
		DEFAULT_MIN_WRITEBACK_MB,
		DEFAULT_POLL_INTERVAL_SECS,
		DEFAULT_MAX_SPIN_CYCLES_HOUR);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	struct abcache_config cfg = {
		.spindown_delay_secs	= DEFAULT_SPINDOWN_DELAY_SECS,
		.min_writeback_mb	= DEFAULT_MIN_WRITEBACK_MB,
		.poll_interval_secs	= DEFAULT_POLL_INTERVAL_SECS,
		.max_spin_cycles_hour	= DEFAULT_MAX_SPIN_CYCLES_HOUR,
		.writeback_rate		= DEFAULT_WRITEBACK_RATE,
		.dry_run		= false,
		.foreground		= false,
		.log_level		= LOG_NORMAL,
	};

	struct bcache_device devices[16];
	int ndevices;
	int c;

	struct option opts[] = {
		{ "spindown-delay",	1, NULL, 's' },
		{ "min-writeback",	1, NULL, 'm' },
		{ "poll-interval",	1, NULL, 'p' },
		{ "max-spin-cycles",	1, NULL, 'c' },
		{ "writeback-rate",	1, NULL, 'r' },
		{ "dry-run",		0, NULL, 'n' },
		{ "foreground",		0, NULL, 'f' },
		{ "verbose",		0, NULL, 'v' },
		{ "quiet",		0, NULL, 'q' },
		{ "help",		0, NULL, 'h' },
		{ NULL,			0, NULL, 0   },
	};

	while ((c = getopt_long(argc, argv, "s:m:p:c:r:nfvqh",
				opts, NULL)) != -1)
		switch (c) {
		case 's':
			cfg.spindown_delay_secs = atoi(optarg);
			break;
		case 'm':
			cfg.min_writeback_mb = atoi(optarg);
			break;
		case 'p':
			cfg.poll_interval_secs = atoi(optarg);
			break;
		case 'c':
			cfg.max_spin_cycles_hour = atoi(optarg);
			break;
		case 'r':
			cfg.writeback_rate = atoi(optarg);
			break;
		case 'n':
			cfg.dry_run = true;
			break;
		case 'f':
			cfg.foreground = true;
			break;
		case 'v':
			cfg.log_level = LOG_DEBUG;
			break;
		case 'q':
			cfg.log_level = 0;
			break;
		case 'h':
		default:
			usage();
		}

	/* Set up signal handlers */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	/* Discover bcache devices */
	ndevices = discover_bcache_devices(devices, 16);
	if (ndevices == 0) {
		fprintf(stderr,
			"[a-bcached] No bcache devices found. "
			"Is bcache loaded and configured?\n");
		exit(EXIT_FAILURE);
	}

	log_msg(LOG_NORMAL, cfg.log_level,
		"[a-bcached] Found %d bcache device(s)\n", ndevices);
	for (int i = 0; i < ndevices; i++)
		log_msg(LOG_NORMAL, cfg.log_level,
			"[a-bcached]   %s -> %s\n",
			devices[i].sysfs_path, devices[i].backing_dev);

	log_msg(LOG_NORMAL, cfg.log_level,
		"[a-bcached] Config: spindown=%ds min_wb=%dMB "
		"poll=%ds max_cycles=%d/hr%s\n",
		cfg.spindown_delay_secs, cfg.min_writeback_mb,
		cfg.poll_interval_secs, cfg.max_spin_cycles_hour,
		cfg.dry_run ? " [DRY-RUN]" : "");

	/* Daemonize unless foreground */
	if (!cfg.foreground) {
		if (daemon(0, 0) < 0) {
			perror("daemon");
			exit(EXIT_FAILURE);
		}
	}

	/* Main loop */
	while (running) {
		for (int i = 0; i < ndevices; i++)
			manage_device(&devices[i], &cfg);

		sleep(cfg.poll_interval_secs);
	}

	/* On exit, re-enable writeback for all devices */
	log_msg(LOG_NORMAL, cfg.log_level,
		"[a-bcached] Shutting down, re-enabling writeback\n");

	for (int i = 0; i < ndevices; i++)
		bcache_set_writeback_running(devices[i].sysfs_path, 1);

	return 0;
}
