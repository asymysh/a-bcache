#!/bin/bash
#
# setup-mock-devices.sh - Create mock HDD and SSD for bcache testing
#
# Creates loop devices simulating:
#   - SSD: 128MB loop device (fast cache device)
#   - HDD: 1GB loop device, optionally wrapped in dm-delay (10ms latency)
#
# Usage:
#   sudo ./test/setup-mock-devices.sh [setup|teardown|status]
#

set -euo pipefail

MOCK_DIR="/tmp/a-bcache-test"
SSD_SIZE_MB=128
HDD_SIZE_MB=1024
HDD_DELAY_MS=10    # simulated seek latency in milliseconds

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { printf '%b[a-bcache-test]%b %s\n' "$GREEN"  "$NC" "$*"; }
warn() { printf '%b[a-bcache-test]%b %s\n' "$YELLOW" "$NC" "$*"; }
err()  { printf '%b[a-bcache-test]%b %s\n' "$RED"    "$NC" "$*" >&2; }
die()  { err "$@"; exit 1; }

check_root() {
    if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
        die "This script must be run as root (need loop device + dm access)"
    fi
}

check_bcache_module() {
    if ! lsmod 2>/dev/null | grep -q '^bcache '; then
        log "Loading bcache kernel module..."
        if ! modprobe bcache 2>&1; then
            die "Failed to load bcache module. Install with: apt install linux-modules-extra-\$(uname -r)"
        fi
    fi
    # Verify sysfs is now populated
    if [[ ! -d /sys/fs/bcache ]]; then
        die "/sys/fs/bcache does not exist even after modprobe -- kernel may not support bcache"
    fi
}

check_deps() {
    local missing=()
    for cmd in losetup dd blkid; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done

    # Either make-bcache (old) or bcache (new unified tool) must exist
    if ! command -v make-bcache >/dev/null 2>&1 && \
       ! command -v bcache >/dev/null 2>&1; then
        missing+=("make-bcache (or bcache)")
    fi

    # hdparm is needed for the daemon, not setup, but warn
    if ! command -v hdparm >/dev/null 2>&1; then
        warn "hdparm not installed -- spin management won't work in tests"
    fi

    # dmsetup is optional but recommended
    if ! command -v dmsetup >/dev/null 2>&1; then
        warn "dmsetup not found -- dm-delay HDD simulation unavailable"
        warn "Install with: apt install dmsetup"
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        die "Missing dependencies: ${missing[*]}"
    fi
}

# Run make-bcache or fall back to the new "bcache make" command.
run_make_bcache() {
    if command -v make-bcache >/dev/null 2>&1; then
        make-bcache "$@"
    elif command -v bcache >/dev/null 2>&1; then
        bcache make "$@"
    else
        return 127
    fi
}

setup() {
    log "Creating mock device directory: $MOCK_DIR"
    mkdir -p "$MOCK_DIR"

    # Create backing files
    log "Creating ${SSD_SIZE_MB}MB mock SSD image..."
    dd if=/dev/zero of="$MOCK_DIR/ssd.img" bs=1M count="$SSD_SIZE_MB" \
        status=none

    log "Creating ${HDD_SIZE_MB}MB mock HDD image..."
    dd if=/dev/zero of="$MOCK_DIR/hdd.img" bs=1M count="$HDD_SIZE_MB" \
        status=none

    # Attach loop devices
    log "Attaching loop devices..."
    local SSD_LOOP HDD_LOOP
    SSD_LOOP=$(losetup --find --show "$MOCK_DIR/ssd.img") || \
        die "losetup failed for SSD image (no free loop devices?)"
    HDD_LOOP=$(losetup --find --show "$MOCK_DIR/hdd.img") || \
        die "losetup failed for HDD image"

    log "  Mock SSD: $SSD_LOOP"
    log "  Mock HDD: $HDD_LOOP"

    printf '%s\n' "$SSD_LOOP" > "$MOCK_DIR/ssd.loop"
    printf '%s\n' "$HDD_LOOP" > "$MOCK_DIR/hdd.loop"

    # Optional: dm-delay for realistic HDD latency
    local HDD_DEV
    if command -v dmsetup >/dev/null 2>&1; then
        local hdd_sectors
        hdd_sectors=$(blockdev --getsz "$HDD_LOOP")

        log "Setting up dm-delay with ${HDD_DELAY_MS}ms read+write latency..."
        # dm-delay table format:
        #   <start> <length> delay <read_dev> <read_off> <read_delay_ms> \
        #     [<write_dev> <write_off> <write_delay_ms>]
        if printf '0 %s delay %s 0 %s %s 0 %s\n' \
            "$hdd_sectors" "$HDD_LOOP" "$HDD_DELAY_MS" \
            "$HDD_LOOP" "$HDD_DELAY_MS" \
            | dmsetup create mock-hdd 2>&1; then
            HDD_DEV="/dev/mapper/mock-hdd"
            log "  Mock HDD (with latency): $HDD_DEV"
        else
            warn "dmsetup create failed, falling back to raw loop device"
            HDD_DEV="$HDD_LOOP"
        fi
    else
        HDD_DEV="$HDD_LOOP"
    fi
    printf '%s\n' "$HDD_DEV" > "$MOCK_DIR/hdd.dev"

    # Format devices for bcache
    log "Formatting mock SSD as bcache cache device..."
    if ! run_make_bcache -C "$SSD_LOOP" --bucket 128k --wipe-bcache; then
        die "Failed to format cache device"
    fi

    log "Formatting mock HDD as bcache backing device (writeback)..."
    if ! run_make_bcache -B "$HDD_DEV" --writeback --wipe-bcache; then
        die "Failed to format backing device"
    fi

    # Register devices. udev should auto-attach, but force it manually.
    log "Registering bcache devices..."
    if [[ -w /sys/fs/bcache/register ]]; then
        printf '%s\n' "$SSD_LOOP" > /sys/fs/bcache/register 2>/dev/null \
            || warn "SSD registration returned non-zero (may already be registered)"
        printf '%s\n' "$HDD_DEV"  > /sys/fs/bcache/register 2>/dev/null \
            || warn "HDD registration returned non-zero (may already be registered)"
    else
        warn "/sys/fs/bcache/register not writable; udev may auto-register"
    fi

    # Wait for /dev/bcacheN to appear and sysfs to populate
    local tries=0
    while [[ $tries -lt 20 ]]; do
        if compgen -G "/sys/block/bcache*/bcache" > /dev/null; then
            break
        fi
        sleep 0.5
        tries=$((tries + 1))
    done

    if ! compgen -G "/sys/block/bcache*/bcache" > /dev/null; then
        warn "No bcache devices appeared in /sys/block after 10s"
    fi

    # Find the cache set UUID and attach it to all backing devices
    local cset_uuid=""
    if [[ -d /sys/fs/bcache ]]; then
        cset_uuid=$(find /sys/fs/bcache -maxdepth 1 -type d \
            -regex '.*/[0-9a-f]\{8\}-.*' -printf '%f\n' 2>/dev/null \
            | head -1)
    fi

    if [[ -n "$cset_uuid" ]]; then
        log "Cache set UUID: $cset_uuid"
        printf '%s\n' "$cset_uuid" > "$MOCK_DIR/cset.uuid"

        local bdev
        for bdev in /sys/block/bcache*/bcache; do
            [[ -d "$bdev" ]] || continue
            if [[ -w "$bdev/attach" ]]; then
                printf '%s\n' "$cset_uuid" > "$bdev/attach" 2>/dev/null \
                    || warn "Attach failed for $bdev (may already be attached)"
                log "  Attached cache to $(basename "$(dirname "$bdev")")"
            fi
        done

        # Ensure writeback mode is set
        for bdev in /sys/block/bcache*/bcache; do
            [[ -d "$bdev" ]] || continue
            if [[ -w "$bdev/cache_mode" ]]; then
                printf 'writeback\n' > "$bdev/cache_mode" 2>/dev/null || true
            fi
        done
    else
        warn "Could not find cache set UUID -- bcache may not be ready"
    fi

    log ""
    log "============================================"
    log "  Mock bcache environment ready"
    log "============================================"
    log "  SSD (cache):   $SSD_LOOP"
    log "  HDD (backing): $HDD_DEV"
    log "  bcache:        $(compgen -G "/dev/bcache*" | head -1 || echo 'not present')"
    log ""
    log "  Test daemon:   sudo ./a-bcached -f -n -v"
    log "  Run tests:     sudo ./test/test-spin-management.sh"
    log "  Teardown:      sudo ./test/setup-mock-devices.sh teardown"
    log "============================================"
}

teardown() {
    log "Tearing down mock bcache environment..."

    # Stop bcache backing devices (flushes any dirty data)
    local bdev
    for bdev in /sys/block/bcache*/bcache; do
        [[ -d "$bdev" ]] || continue
        if [[ -w "$bdev/stop" ]]; then
            log "Stopping $(basename "$(dirname "$bdev")")..."
            printf '1\n' > "$bdev/stop" 2>/dev/null || true
        fi
    done

    # Stop cache set
    if [[ -f "$MOCK_DIR/cset.uuid" ]]; then
        local uuid
        uuid=$(cat "$MOCK_DIR/cset.uuid")
        if [[ -w "/sys/fs/bcache/$uuid/stop" ]]; then
            log "Stopping cache set $uuid..."
            printf '1\n' > "/sys/fs/bcache/$uuid/stop" 2>/dev/null || true
        fi
    fi

    # Give the kernel a moment to release devices
    sleep 1

    # Remove dm-delay device (must come before loop detach)
    if command -v dmsetup >/dev/null 2>&1; then
        if dmsetup info mock-hdd >/dev/null 2>&1; then
            log "Removing dm-delay device..."
            dmsetup remove mock-hdd 2>/dev/null || \
                warn "dmsetup remove failed; device may be busy"
        fi
    fi

    # Detach loop devices
    local loop
    for f in ssd.loop hdd.loop; do
        if [[ -f "$MOCK_DIR/$f" ]]; then
            loop=$(cat "$MOCK_DIR/$f")
            if [[ -e "$loop" ]]; then
                log "Detaching loop: $loop"
                losetup -d "$loop" 2>/dev/null || \
                    warn "losetup -d failed for $loop"
            fi
        fi
    done

    log "Removing mock images..."
    rm -rf "$MOCK_DIR"

    log "Teardown complete."
}

status() {
    echo "=== Loop Devices (a-bcache test) ==="
    if [[ -d "$MOCK_DIR" ]]; then
        losetup -l 2>/dev/null | grep "$MOCK_DIR" || echo "  (none)"
    else
        echo "  (mock dir not present)"
    fi

    echo ""
    echo "=== DM Devices ==="
    if command -v dmsetup >/dev/null 2>&1; then
        dmsetup ls 2>/dev/null | grep mock-hdd || echo "  (none)"
    else
        echo "  (dmsetup not installed)"
    fi

    echo ""
    echo "=== bcache Devices ==="
    if compgen -G "/sys/block/bcache*" > /dev/null; then
        local bdev
        for bdev in /sys/block/bcache*/bcache; do
            [[ -d "$bdev" ]] || continue
            local name
            name=$(basename "$(dirname "$bdev")")
            echo "  $name:"
            for attr in state cache_mode dirty_data writeback_running; do
                if [[ -r "$bdev/$attr" ]]; then
                    echo "    $attr: $(cat "$bdev/$attr")"
                fi
            done
        done
    else
        echo "  (none)"
    fi

    echo ""
    echo "=== Cache Sets ==="
    if [[ -d /sys/fs/bcache ]]; then
        find /sys/fs/bcache -maxdepth 1 -type d \
            -regex '.*/[0-9a-f]\{8\}-.*' -printf '  %f\n' 2>/dev/null \
            | head -10 || echo "  (none)"
    else
        echo "  (/sys/fs/bcache not present -- bcache module not loaded)"
    fi
}

usage() {
    cat <<EOF
Usage: $0 [setup|teardown|status]

  setup     Create mock SSD and HDD loop devices, format and register
            them with bcache, and attach the cache to the backing device.
  teardown  Stop bcache, remove dm-delay, detach loop devices, delete
            mock files.
  status    Show the state of mock devices and bcache.
EOF
}

# --- Entry point ---
case "${1:-setup}" in
    setup)
        check_root
        check_deps
        check_bcache_module
        setup
        ;;
    teardown)
        check_root
        teardown
        ;;
    status)
        status
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
