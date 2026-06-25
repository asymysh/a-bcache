# a-bcache

**bcache fork with HDD spin management and lifespan-first optimizations.**

a-bcache extends the standard bcache-tools with a spin management daemon
(`a-bcached`) that minimizes HDD mechanical wear through:

- **I/O coalescing**: accumulates dirty data on the SSD cache before flushing
- **Spin-down awareness**: spins down the HDD after idle periods, pauses writeback to prevent unwanted wake-ups
- **Spin cycle budgeting**: limits spin-up/spin-down transitions per hour to reduce motor/bearing wear
- **Sequential-only writeback**: leverages bcache's existing sequential writeback to avoid random HDD seeks

## How it works

```
┌─────────────────────────────────────────┐
│  Applications                           │
├─────────────────────────────────────────┤
│  /dev/bcache0  (bcache block device)    │
├──────────────┬──────────────────────────┤
│  SSD Cache   │  a-bcached (this daemon) │
│  (fast I/O)  │  - monitors dirty data   │
│              │  - pauses/resumes wb     │
│              │  - manages HDD spin      │
├──────────────┴──────────────────────────┤
│  HDD (backing device)                   │
│  - spun down when idle                  │
│  - sequential writeback only            │
│  - spin cycles budgeted                 │
└─────────────────────────────────────────┘
```

## Quick Start

### 1. Standard bcache setup

```bash
# Format devices
make-bcache -C /dev/ssd_device -B /dev/hdd_device --writeback

# Register
echo /dev/ssd_device > /sys/fs/bcache/register
echo /dev/hdd_device > /sys/fs/bcache/register
```

### 2. Build and install a-bcached

```bash
make
sudo make install
```

### 3. Run the spin management daemon

```bash
# Foreground with verbose logging (for testing)
sudo a-bcached --foreground --verbose

# Or install as a service
sudo cp a-bcached.service /etc/systemd/system/
sudo systemctl enable --now a-bcached
```

### Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--spindown-delay` | 300s | Seconds of HDD idle before spin-down |
| `--min-writeback` | 64MB | Minimum dirty data before allowing flush |
| `--poll-interval` | 5s | How often to check bcache state |
| `--max-spin-cycles` | 4/hr | Maximum spin transitions per hour |
| `--writeback-rate` | kernel | Writeback rate in sectors/sec |
| `--dry-run` | off | Log actions without executing |

## Testing with Mock Devices

You can test without real hardware using loop devices and dm-delay:

```bash
# Create mock SSD (128MB) and HDD (1GB with 10ms latency)
sudo ./test/setup-mock-devices.sh setup

# Check status
sudo ./test/setup-mock-devices.sh status

# Run integration tests
sudo ./test/test-spin-management.sh

# Test daemon in dry-run mode
sudo ./a-bcached -f -n -v

# Clean up
sudo ./test/setup-mock-devices.sh teardown
```

### What the mock environment provides

| Mock Device | Implementation | Simulates |
|---|---|---|
| **SSD** | Loop device on file (128MB) | Fast cache device |
| **HDD** | Loop device + dm-delay (1GB, 10ms) | Slow rotational drive with seek latency |

**Note**: `hdparm -C` (spin state detection) and `hdparm -y` (spin-down)
won't work on loop devices. These features can only be tested on real
hardware. The test suite skips those tests and marks them accordingly.

## Original bcache-tools

This repo includes all the original bcache-tools:

- `make-bcache` - Format devices for bcache
- `bcache-super-show` - Print bcache superblock info
- `probe-bcache` - Probe for bcache signatures
- `bcache-register` - Register bcache devices

See the [bcache documentation](https://www.kernel.org/doc/html/latest/admin-guide/bcache.html)
for full details on bcache itself.

## License

GPLv2 (same as bcache)
