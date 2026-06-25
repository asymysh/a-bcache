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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * ===========================================================================
 *  Configuration defaults
 * ===========================================================================
 */

#define DEFAULT_SPINDOWN_DELAY_SECS	300
#define DEFAULT_MIN_WRITEBACK_MB	64
#define DEFAULT_POLL_INTERVAL_SECS	5
#define DEFAULT_MAX_SPIN_CYCLES_HOUR	4
#define DEFAULT_WRITEBACK_RATE		0
#define MAX_DEVICES			16

/*
 * Verbosity levels. Named with VERB_ prefix to avoid clashing with
 * <syslog.h> LOG_* macros.
 */
#define VERB_QUIET	0
#define VERB_INFO	1
#define VERB_DEBUG	2

/*
 * ===========================================================================
 *  Data types
 * ===========================================================================
 */

struct io_stats {
	uint64_t	read_ios;
	uint64_t	read_sectors;
	uint64_t	write_ios;
	uint64_t	write_sectors;
};

struct spin_state {
	bool		is_spinning;
	bool		stats_valid;
	time_t		last_io_time;
	time_t		last_spindown_time;
	time_t		last_spinup_time;
	unsigned	spin_cycles_this_hour;
	time_t		hour_start;
	struct io_stats	prev_stats;
};

struct abcache_config {
	unsigned	spindown_delay_secs;
	unsigned	min_writeback_mb;
	unsigned	poll_interval_secs;
	unsigned	max_spin_cycles_hour;
	unsigned	writeback_rate;
	bool		dry_run;
	bool		foreground;
	int		log_level;
};

struct bcache_device {
	char			sysfs_path[PATH_MAX];
	char			backing_dev[PATH_MAX];
	char			bcache_name[64];
	struct spin_state	spin;
};

/*
 * ===========================================================================
 *  Globals
 * ===========================================================================
 */

static volatile sig_atomic_t	g_running	= 1;
static bool			g_use_syslog	= false;
static int			g_log_level	= VERB_INFO;

/*
 * ===========================================================================
 *  Logging
 * ===========================================================================
 *
 * Routes to syslog when daemonized (g_use_syslog=true), stderr otherwise.
 */

static void __attribute__((format(printf, 2, 3)))
log_msg(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > g_log_level)
		return;

	va_start(ap, fmt);
	if (g_use_syslog) {
		int priority = (level >= VERB_DEBUG) ? LOG_DEBUG : LOG_INFO;
		vsyslog(priority, fmt, ap);
	} else {
		vfprintf(stderr, fmt, ap);
	}
	va_end(ap);
}

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

/*
 * ===========================================================================
 *  Sysfs helpers
 * ===========================================================================
 */

static int sysfs_read_str(const char *path, char *buf, size_t len)
{
	int fd;
	ssize_t n;

	if (!path || !buf || len == 0)
		return -EINVAL;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	n = read(fd, buf, len - 1);
	close(fd);

	if (n < 0)
		return -errno;

	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	return 0;
}

static int sysfs_write_str(const char *path, const char *val)
{
	int fd;
	ssize_t n;
	size_t want;

	if (!path || !val)
		return -EINVAL;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	want = strlen(val);
	n = write(fd, val, want);
	close(fd);

	if (n < 0)
		return -errno;
	if ((size_t)n != want)
		return -EIO;

	return 0;
}

static int sysfs_write_uint(const char *path, unsigned val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", val);
	return sysfs_write_str(path, buf);
}

/*
 * Build a sysfs path safely, returning -ENAMETOOLONG on truncation.
 */
static int build_path(char *out, size_t out_len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out, out_len, fmt, ap);
	va_end(ap);

	if (n < 0)
		return -EIO;
	if ((size_t)n >= out_len)
		return -ENAMETOOLONG;
	return 0;
}

/*
 * ===========================================================================
 *  Bcache state queries
 * ===========================================================================
 */

/*
 * Parse bcache "dirty_data" sysfs output into bytes.
 *
 * Format is "<number><unit>" where unit is one of K/M/G/T/P (case-insensitive),
 * with binary (1024-based) multipliers. e.g. "1.5M" -> ~1572864 bytes.
 * A bare number with no unit is treated as bytes.
 */
static int bcache_get_dirty_data_bytes(const char *sysfs_path, uint64_t *bytes)
{
	char path[PATH_MAX];
	char buf[64];
	int ret, items;
	double val = 0.0;
	char unit = 0;

	ret = build_path(path, sizeof(path), "%s/dirty_data", sysfs_path);
	if (ret)
		return ret;

	ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;

	items = sscanf(buf, "%lf%c", &val, &unit);
	if (items < 1) {
		*bytes = 0;
		return 0;
	}
	if (items < 2)
		unit = 0;

	switch (unit) {
	case 'P': case 'p': val *= 1024.0; /* fall through */
	case 'T': case 't': val *= 1024.0; /* fall through */
	case 'G': case 'g': val *= 1024.0; /* fall through */
	case 'M': case 'm': val *= 1024.0; /* fall through */
	case 'K': case 'k': val *= 1024.0;
		break;
	default:
		/* No unit or unknown unit -- treat as bytes */
		break;
	}

	if (val < 0.0)
		val = 0.0;
	*bytes = (uint64_t)val;
	return 0;
}

static int bcache_get_state(const char *sysfs_path, char *state, size_t len)
{
	char path[PATH_MAX];
	int ret = build_path(path, sizeof(path), "%s/state", sysfs_path);
	if (ret)
		return ret;
	return sysfs_read_str(path, state, len);
}

static int bcache_set_writeback_rate(const char *sysfs_path, unsigned rate)
{
	char path[PATH_MAX];
	int ret = build_path(path, sizeof(path), "%s/writeback_rate",
			     sysfs_path);
	if (ret)
		return ret;
	return sysfs_write_uint(path, rate);
}

static int bcache_set_writeback_running(const char *sysfs_path, unsigned val)
{
	char path[PATH_MAX];
	int ret = build_path(path, sizeof(path), "%s/writeback_running",
			     sysfs_path);
	if (ret)
		return ret;
	return sysfs_write_uint(path, val);
}

/*
 * ===========================================================================
 *  Device path validation
 * ===========================================================================
 *
 * Sysfs paths are well-controlled but we validate to be defensive against
 * shell injection in system()/popen() calls.
 */

static bool valid_dev_path(const char *path)
{
	const char *p;

	if (!path || strncmp(path, "/dev/", 5) != 0)
		return false;

	for (p = path + 5; *p; p++) {
		if (!isalnum((unsigned char)*p) &&
		    *p != '/' && *p != '-' && *p != '_' && *p != '.')
			return false;
	}
	return true;
}

/*
 * ===========================================================================
 *  HDD spin management via hdparm
 * ===========================================================================
 */

static int hdd_spindown(const char *dev, bool dry_run)
{
	char cmd[PATH_MAX + 64];
	int ret;

	if (!valid_dev_path(dev)) {
		log_msg(VERB_INFO,
			"[a-bcached] Refusing spin-down on suspicious path\n");
		return -EINVAL;
	}

	if (dry_run) {
		log_msg(VERB_INFO, "[DRY-RUN] Would spin down %s\n", dev);
		return 0;
	}

	ret = build_path(cmd, sizeof(cmd),
			 "hdparm -y %s > /dev/null 2>&1", dev);
	if (ret)
		return ret;

	ret = system(cmd);
	if (ret == -1)
		return -errno;
	return ret;
}

/*
 * Check HDD spin state via hdparm -C.
 * Returns: 1 = spinning (active/idle), 0 = standby/sleep, -1 = error
 *
 * Note: hdparm -C does NOT wake a spun-down HDD (uses CHECK POWER MODE
 * which goes through the ATA command queue without media access).
 */
static int hdd_check_spinning(const char *dev)
{
	char cmd[PATH_MAX + 64];
	char buf[256];
	FILE *fp;
	int result = -1;
	int ret;

	if (!valid_dev_path(dev))
		return -1;

	ret = build_path(cmd, sizeof(cmd), "hdparm -C %s 2>/dev/null", dev);
	if (ret)
		return -1;

	fp = popen(cmd, "r");
	if (!fp)
		return -1;

	while (fgets(buf, sizeof(buf), fp)) {
		if (strstr(buf, "standby") || strstr(buf, "sleep")) {
			result = 0;
			break;
		}
		if (strstr(buf, "active") || strstr(buf, "idle")) {
			result = 1;
			break;
		}
	}

	pclose(fp);
	return result;
}

/*
 * ===========================================================================
 *  I/O activity monitoring via /sys/block/<dev>/stat
 * ===========================================================================
 *
 * /sys/block/<dev>/stat fields (Documentation/iostats.rst):
 *   1: read_ios   2: read_merges   3: read_sectors    4: read_ticks
 *   5: write_ios  6: write_merges  7: write_sectors   8: write_ticks
 *   9: in_flight  10: io_ticks    11: time_in_queue
 *   (newer kernels add discard and flush fields)
 */

static int read_io_stats(const char *block_dev, struct io_stats *stats)
{
	char path[PATH_MAX];
	char buf[512];
	const char *devname;
	int ret;

	if (!block_dev || !stats)
		return -EINVAL;

	devname = strrchr(block_dev, '/');
	devname = devname ? devname + 1 : block_dev;

	if (*devname == '\0')
		return -EINVAL;

	ret = build_path(path, sizeof(path), "/sys/block/%s/stat", devname);
	if (ret)
		return ret;

	ret = sysfs_read_str(path, buf, sizeof(buf));
	if (ret)
		return ret;

	memset(stats, 0, sizeof(*stats));
	if (sscanf(buf,
		   "%" SCNu64 " %*u %" SCNu64 " %*u "
		   "%" SCNu64 " %*u %" SCNu64,
		   &stats->read_ios, &stats->read_sectors,
		   &stats->write_ios, &stats->write_sectors) < 4)
		return -EINVAL;

	return 0;
}

static bool io_stats_changed(const struct io_stats *a, const struct io_stats *b)
{
	return a->read_ios     != b->read_ios     ||
	       a->write_ios    != b->write_ios    ||
	       a->read_sectors != b->read_sectors ||
	       a->write_sectors != b->write_sectors;
}

/*
 * ===========================================================================
 *  Core daemon logic
 * ===========================================================================
 *
 * Strategy per device:
 *   1. Detect any I/O activity by comparing stats deltas; update last_io_time
 *   2. If HDD is spinning and idle >= spindown_delay AND dirty < threshold:
 *      pause writeback, then issue hdparm -y
 *   3. If HDD is spun down and dirty >= threshold AND budget remains:
 *      enable writeback (HDD spins up naturally on next write)
 *   4. Otherwise: hold current state, keep writeback paused if HDD is down
 */
static void manage_device(struct bcache_device *dev,
			  struct abcache_config *cfg)
{
	uint64_t dirty_bytes = 0;
	uint64_t dirty_mb;
	struct io_stats cur_stats;
	time_t now = time(NULL);
	long idle_secs;
	char state[64];
	int spinning;
	int ret;

	/* Read bcache dirty data and state. Skip device on any error. */
	ret = bcache_get_dirty_data_bytes(dev->sysfs_path, &dirty_bytes);
	if (ret) {
		log_msg(VERB_DEBUG,
			"[a-bcached] %s: dirty_data read failed (%d)\n",
			dev->bcache_name, ret);
		return;
	}

	ret = bcache_get_state(dev->sysfs_path, state, sizeof(state));
	if (ret) {
		log_msg(VERB_DEBUG,
			"[a-bcached] %s: state read failed (%d)\n",
			dev->bcache_name, ret);
		return;
	}

	/*
	 * Only manage devices with an attached cache.
	 * "no cache"     -> no caching, nothing to do
	 * "inconsistent" -> dangerous, leave alone
	 * "clean"        -> ok, may still have dirty=0 due to lag
	 * "dirty"        -> ok, has unflushed data
	 */
	if (strcmp(state, "clean") != 0 && strcmp(state, "dirty") != 0) {
		log_msg(VERB_DEBUG,
			"[a-bcached] %s: skipping (state=\"%s\")\n",
			dev->bcache_name, state);
		return;
	}

	/* Detect I/O activity via /sys/block/<backing>/stat deltas */
	if (read_io_stats(dev->backing_dev, &cur_stats) == 0) {
		if (!dev->spin.stats_valid) {
			dev->spin.prev_stats = cur_stats;
			dev->spin.stats_valid = true;
		} else if (io_stats_changed(&cur_stats, &dev->spin.prev_stats)) {
			dev->spin.last_io_time = now;
			dev->spin.prev_stats = cur_stats;
		}
	}

	/* Query current HDD spin state */
	spinning = hdd_check_spinning(dev->backing_dev);
	if (spinning < 0) {
		/*
		 * hdparm failed (non-ATA device, missing tool, no perms).
		 * Fall back to managing writeback only; assume spinning so
		 * we don't issue spin-down commands that would also fail.
		 */
		log_msg(VERB_DEBUG,
			"[a-bcached] %s: spin state unknown (hdparm failed)\n",
			dev->bcache_name);
		dev->spin.is_spinning = true;
	} else {
		dev->spin.is_spinning = (spinning == 1);
	}

	/* Reset hourly spin-cycle budget */
	if (now - dev->spin.hour_start >= 3600) {
		dev->spin.spin_cycles_this_hour = 0;
		dev->spin.hour_start = now;
	}

	/* Compute idle time, guard against clock jumps */
	idle_secs = (long)(now - dev->spin.last_io_time);
	if (idle_secs < 0) {
		idle_secs = 0;
		dev->spin.last_io_time = now;
	}

	dirty_mb = dirty_bytes / (1024ULL * 1024ULL);

	if (dev->spin.is_spinning) {
		/* HDD spinning: spin down if idle long enough and not much dirty */
		if ((unsigned long)idle_secs >= cfg->spindown_delay_secs &&
		    dirty_mb < cfg->min_writeback_mb) {
			log_msg(VERB_INFO,
				"[a-bcached] %s (%s): idle %lds, dirty %" PRIu64
				"MB -- spinning down\n",
				dev->bcache_name, dev->backing_dev,
				idle_secs, dirty_mb);

			bcache_set_writeback_running(dev->sysfs_path, 0);
			hdd_spindown(dev->backing_dev, cfg->dry_run);

			dev->spin.is_spinning = false;
			dev->spin.last_spindown_time = now;
			dev->spin.spin_cycles_this_hour++;
		} else if (dirty_mb > 0) {
			/* Spinning + dirty: allow sequential writeback */
			bcache_set_writeback_running(dev->sysfs_path, 1);
			log_msg(VERB_DEBUG,
				"[a-bcached] %s: spinning, dirty %" PRIu64
				"MB, idle %lds -- writeback active\n",
				dev->bcache_name, dirty_mb, idle_secs);
		} else {
			log_msg(VERB_DEBUG,
				"[a-bcached] %s: spinning, idle %lds (delay %us)\n",
				dev->bcache_name, idle_secs,
				cfg->spindown_delay_secs);
		}
	} else {
		/* HDD spun down: keep writeback paused unless we must flush */
		if (dirty_mb >= cfg->min_writeback_mb) {
			if (dev->spin.spin_cycles_this_hour >=
			    cfg->max_spin_cycles_hour) {
				log_msg(VERB_INFO,
					"[a-bcached] %s: dirty %" PRIu64
					"MB but spin budget exhausted (%u/%u)"
					" -- deferring\n",
					dev->bcache_name, dirty_mb,
					dev->spin.spin_cycles_this_hour,
					cfg->max_spin_cycles_hour);
				return;
			}

			log_msg(VERB_INFO,
				"[a-bcached] %s (%s): dirty %" PRIu64
				"MB >= %uMB threshold"
				" -- enabling writeback (HDD will spin up)\n",
				dev->bcache_name, dev->backing_dev,
				dirty_mb, cfg->min_writeback_mb);

			if (cfg->writeback_rate)
				bcache_set_writeback_rate(dev->sysfs_path,
							  cfg->writeback_rate);

			bcache_set_writeback_running(dev->sysfs_path, 1);
			dev->spin.last_io_time = now;
			dev->spin.last_spinup_time = now;
			dev->spin.spin_cycles_this_hour++;
		} else {
			bcache_set_writeback_running(dev->sysfs_path, 0);
			log_msg(VERB_DEBUG,
				"[a-bcached] %s: spun down, dirty %" PRIu64
				"MB < %uMB -- holding\n",
				dev->bcache_name, dirty_mb,
				cfg->min_writeback_mb);
		}
	}
}

/*
 * ===========================================================================
 *  Device discovery
 * ===========================================================================
 */

/*
 * Find the backing device for /sys/block/<bcache_name>.
 * For a bcache block device, /sys/block/bcacheN/slaves/ contains symlinks
 * to the underlying block devices; for a singly-attached backing device
 * there's exactly one entry.
 */
static int find_backing_dev(const char *bcache_name, char *out, size_t out_len)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	int found = 0;
	int ret;

	ret = build_path(path, sizeof(path),
			 "/sys/block/%s/slaves", bcache_name);
	if (ret)
		return ret;

	dir = opendir(path);
	if (!dir)
		return -errno;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		ret = build_path(out, out_len, "/dev/%s", entry->d_name);
		if (ret) {
			closedir(dir);
			return ret;
		}
		found = 1;
		break;
	}

	closedir(dir);
	return found ? 0 : -ENOENT;
}

static int discover_bcache_devices(struct bcache_device *devs, int max_devs)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;
	char path[PATH_MAX];
	struct stat st;
	time_t now = time(NULL);

	dir = opendir("/sys/block");
	if (!dir)
		return -errno;

	while ((entry = readdir(dir)) != NULL && count < max_devs) {
		/* Match "bcacheN" where N starts with a digit */
		if (strncmp(entry->d_name, "bcache", 6) != 0 ||
		    !isdigit((unsigned char)entry->d_name[6]))
			continue;

		if (build_path(path, sizeof(path),
			       "/sys/block/%s/bcache", entry->d_name) != 0)
			continue;

		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		/* Zero-initialize the entire device struct */
		memset(&devs[count], 0, sizeof(devs[count]));

		if (build_path(devs[count].sysfs_path,
			       sizeof(devs[count].sysfs_path),
			       "/sys/block/%s/bcache", entry->d_name) != 0)
			continue;

		if (build_path(devs[count].bcache_name,
			       sizeof(devs[count].bcache_name),
			       "%s", entry->d_name) != 0)
			continue;

		if (find_backing_dev(entry->d_name,
				     devs[count].backing_dev,
				     sizeof(devs[count].backing_dev)) != 0) {
			fprintf(stderr,
				"[a-bcached] %s: cannot find backing device,"
				" skipping\n",
				entry->d_name);
			continue;
		}

		devs[count].spin.hour_start = now;
		devs[count].spin.last_io_time = now;
		devs[count].spin.stats_valid = false;
		devs[count].spin.is_spinning = true;

		count++;
	}

	closedir(dir);
	return count;
}

/*
 * ===========================================================================
 *  Argument parsing & main
 * ===========================================================================
 */

static void usage(FILE *out)
{
	fprintf(out,
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
		"  -p, --poll-interval SECS    Polling interval, must be > 0 "
			"(default: %d)\n"
		"  -c, --max-spin-cycles N     Max spin transitions/hour "
			"(default: %d)\n"
		"  -r, --writeback-rate RATE   Writeback rate in sectors/sec "
			"(default: kernel)\n"
		"  -n, --dry-run               Don't issue actual commands\n"
		"  -f, --foreground            Don't daemonize (log to stderr)\n"
		"  -v, --verbose               Debug logging\n"
		"  -q, --quiet                 No logging\n"
		"  -h, --help                  Show this help and exit\n",
		DEFAULT_SPINDOWN_DELAY_SECS,
		DEFAULT_MIN_WRITEBACK_MB,
		DEFAULT_POLL_INTERVAL_SECS,
		DEFAULT_MAX_SPIN_CYCLES_HOUR);
}

/*
 * Parse a non-negative integer from a string. Returns -1 on error.
 */
static long parse_uint_arg(const char *s, const char *name)
{
	char *end;
	unsigned long v;

	if (!s || *s == '\0') {
		fprintf(stderr, "[a-bcached] Missing value for %s\n", name);
		return -1;
	}

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || *end != '\0') {
		fprintf(stderr, "[a-bcached] Invalid value for %s: \"%s\"\n",
			name, s);
		return -1;
	}
	if (v > UINT_MAX) {
		fprintf(stderr, "[a-bcached] Value for %s too large: %s\n",
			name, s);
		return -1;
	}
	return (long)v;
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
		.log_level		= VERB_INFO,
	};

	struct bcache_device devices[MAX_DEVICES];
	int ndevices;
	int c;
	long val;
	int i;
	struct sigaction sa, sa_ignore;

	static const struct option opts[] = {
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
		{ NULL,			0, NULL,  0  },
	};

	while ((c = getopt_long(argc, argv, "s:m:p:c:r:nfvqh",
				opts, NULL)) != -1) {
		switch (c) {
		case 's':
			val = parse_uint_arg(optarg, "spindown-delay");
			if (val < 0)
				return EXIT_FAILURE;
			cfg.spindown_delay_secs = (unsigned)val;
			break;
		case 'm':
			val = parse_uint_arg(optarg, "min-writeback");
			if (val < 0)
				return EXIT_FAILURE;
			cfg.min_writeback_mb = (unsigned)val;
			break;
		case 'p':
			val = parse_uint_arg(optarg, "poll-interval");
			if (val < 0)
				return EXIT_FAILURE;
			if (val == 0) {
				fprintf(stderr,
					"[a-bcached] poll-interval must be > 0\n");
				return EXIT_FAILURE;
			}
			cfg.poll_interval_secs = (unsigned)val;
			break;
		case 'c':
			val = parse_uint_arg(optarg, "max-spin-cycles");
			if (val < 0)
				return EXIT_FAILURE;
			cfg.max_spin_cycles_hour = (unsigned)val;
			break;
		case 'r':
			val = parse_uint_arg(optarg, "writeback-rate");
			if (val < 0)
				return EXIT_FAILURE;
			cfg.writeback_rate = (unsigned)val;
			break;
		case 'n':
			cfg.dry_run = true;
			break;
		case 'f':
			cfg.foreground = true;
			break;
		case 'v':
			cfg.log_level = VERB_DEBUG;
			break;
		case 'q':
			cfg.log_level = VERB_QUIET;
			break;
		case 'h':
			usage(stdout);
			return EXIT_SUCCESS;
		case '?':
		default:
			usage(stderr);
			return EXIT_FAILURE;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "[a-bcached] Unexpected argument: %s\n",
			argv[optind]);
		return EXIT_FAILURE;
	}

	/*
	 * Require root unless dry-run mode.
	 * Writing bcache sysfs control files and running hdparm both need
	 * elevated privileges; without them the daemon would just log
	 * permission errors in a loop.
	 */
	if (geteuid() != 0 && !cfg.dry_run) {
		fprintf(stderr,
			"[a-bcached] Must be run as root (or use --dry-run "
			"for testing).\n");
		return EXIT_FAILURE;
	}

	/* Soft-check for hdparm presence */
	if (system("command -v hdparm > /dev/null 2>&1") != 0) {
		fprintf(stderr,
			"[a-bcached] Warning: hdparm not found in PATH; "
			"HDD spin control will be unavailable.\n"
			"[a-bcached]          Install with: apt install "
			"hdparm  (or your distro's equivalent)\n");
	}

	g_log_level = cfg.log_level;

	/* Install signal handlers via sigaction (POSIX-correct) */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;  /* No SA_RESTART -- sleep() returns EINTR on signal */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	memset(&sa_ignore, 0, sizeof(sa_ignore));
	sa_ignore.sa_handler = SIG_IGN;
	sigemptyset(&sa_ignore.sa_mask);
	sigaction(SIGPIPE, &sa_ignore, NULL);
	sigaction(SIGHUP, &sa_ignore, NULL);

	/* Discover devices before daemonizing so errors are visible */
	ndevices = discover_bcache_devices(devices, MAX_DEVICES);
	if (ndevices < 0) {
		fprintf(stderr,
			"[a-bcached] Cannot read /sys/block: %s\n",
			strerror(-ndevices));
		return EXIT_FAILURE;
	}
	if (ndevices == 0) {
		fprintf(stderr,
			"[a-bcached] No bcache devices found.\n"
			"[a-bcached] Verify the bcache kernel module is loaded "
			"and a backing device is attached:\n"
			"[a-bcached]   modprobe bcache\n"
			"[a-bcached]   ls /sys/block/bcache*\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "[a-bcached] Found %d bcache device(s)\n", ndevices);
	for (i = 0; i < ndevices; i++)
		fprintf(stderr, "[a-bcached]   %s -> %s\n",
			devices[i].bcache_name, devices[i].backing_dev);

	fprintf(stderr,
		"[a-bcached] Config: spindown=%us min_wb=%uMB"
		" poll=%us max_cycles=%u/hr%s\n",
		cfg.spindown_delay_secs, cfg.min_writeback_mb,
		cfg.poll_interval_secs, cfg.max_spin_cycles_hour,
		cfg.dry_run ? " [DRY-RUN]" : "");

	/* Daemonize if requested. After this, log_msg routes to syslog. */
	if (!cfg.foreground) {
		if (daemon(0, 0) < 0) {
			fprintf(stderr,
				"[a-bcached] daemon() failed: %s\n",
				strerror(errno));
			return EXIT_FAILURE;
		}
		openlog("a-bcached", LOG_PID | LOG_CONS, LOG_DAEMON);
		g_use_syslog = true;
		log_msg(VERB_INFO,
			"a-bcached started, managing %d device(s)\n",
			ndevices);
	}

	/* Main loop */
	while (g_running) {
		for (i = 0; i < ndevices; i++)
			manage_device(&devices[i], &cfg);

		/*
		 * sleep() returns early on SIGTERM/SIGINT (no SA_RESTART),
		 * letting us exit promptly.
		 */
		sleep(cfg.poll_interval_secs);
	}

	/* Cleanup: re-enable writeback on exit so kernel keeps managing data */
	log_msg(VERB_INFO,
		"[a-bcached] Shutting down, re-enabling writeback\n");
	for (i = 0; i < ndevices; i++)
		bcache_set_writeback_running(devices[i].sysfs_path, 1);

	if (g_use_syslog)
		closelog();

	return EXIT_SUCCESS;
}
