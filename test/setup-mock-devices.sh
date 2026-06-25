#!/bin/bash
#
# setup-mock-devices.sh - Create mock HDD and SSD for bcache testing
#
# Creates loop devices that simulate:
#   - SSD: fast loop device on tmpfs (ramdisk)
#   - HDD: loop device with dm-delay adding 10ms latency
#
# Usage: sudo ./test/setup-mock-devices.sh [setup|teardown|status]
#
# After setup, you'll have:
#   /dev/loop-ssd   (128MB mock SSD - cache device)
#   /dev/loop-hdd   (1GB mock HDD - backing device)
#   /dev/dm-hdd     (dm-delay wrapper adding latency - optional)
#

set -euo pipefail

MOCK_DIR="/tmp/a-bcache-test"
SSD_SIZE_MB=128
HDD_SIZE_MB=1024
HDD_DELAY_MS=10    # simulated seek latency in milliseconds

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[a-bcache-test]${NC} $*"; }
warn() { echo -e "${YELLOW}[a-bcache-test]${NC} $*"; }
err() { echo -e "${RED}[a-bcache-test]${NC} $*" >&2; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        err "This script must be run as root (need loop device + dm access)"
        exit 1
    fi
}

check_deps() {
    local missing=()
    for cmd in losetup make-bcache hdparm; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    # dm-delay is optional
    if ! command -v dmsetup &>/dev/null; then
        warn "dmsetup not found -- dm-delay HDD simulation won't be available"
        warn "Install with: apt install dmsetup"
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        err "Missing dependencies: ${missing[*]}"
        err "Install with: apt install ${missing[*]}"
        exit 1
    fi
}

setup() {
    log "Creating mock device directory: $MOCK_DIR"
    mkdir -p "$MOCK_DIR"

    # --- Create backing files ---
    log "Creating ${SSD_SIZE_MB}MB mock SSD image..."
    dd if=/dev/zero of="$MOCK_DIR/ssd.img" bs=1M count=$SSD_SIZE_MB status=progress 2>&1

    log "Creating ${HDD_SIZE_MB}MB mock HDD image..."
    dd if=/dev/zero of="$MOCK_DIR/hdd.img" bs=1M count=$HDD_SIZE_MB status=progress 2>&1

    # --- Set up loop devices ---
    log "Attaching loop devices..."
    SSD_LOOP=$(losetup --find --show "$MOCK_DIR/ssd.img")
    HDD_LOOP=$(losetup --find --show "$MOCK_DIR/hdd.img")

    log "  Mock SSD: $SSD_LOOP"
    log "  Mock HDD: $HDD_LOOP"

    # Save loop device paths for teardown
    echo "$SSD_LOOP" > "$MOCK_DIR/ssd.loop"
    echo "$HDD_LOOP" > "$MOCK_DIR/hdd.loop"

    # --- Optional: dm-delay for realistic HDD latency ---
    if command -v dmsetup &>/dev/null; then
        local hdd_sectors
        hdd_sectors=$(blockdev --getsz "$HDD_LOOP")

        log "Setting up dm-delay with ${HDD_DELAY_MS}ms latency..."
        # dm-delay table: start_sector num_sectors delay target_dev offset delay_ms
        # Read delay + write delay
        echo "0 $hdd_sectors delay $HDD_LOOP 0 $HDD_DELAY_MS $HDD_LOOP 0 $HDD_DELAY_MS" | \
            dmsetup create mock-hdd

        HDD_DEV="/dev/mapper/mock-hdd"
        echo "$HDD_DEV" > "$MOCK_DIR/hdd.dev"
        log "  Mock HDD (with latency): $HDD_DEV"
    else
        HDD_DEV="$HDD_LOOP"
        echo "$HDD_DEV" > "$MOCK_DIR/hdd.dev"
    fi

    # --- Format for bcache ---
    log "Formatting mock SSD as bcache cache device..."
    make-bcache -C "$SSD_LOOP" --bucket 128k 2>&1 || {
        # If make-bcache is the new 'bcache' binary:
        if command -v bcache &>/dev/null; then
            bcache make -C "$SSD_LOOP" --bucket 128k 2>&1
        else
            err "Failed to format cache device"
        fi
    }

    log "Formatting mock HDD as bcache backing device (writeback mode)..."
    make-bcache -B "$HDD_DEV" --writeback 2>&1 || {
        if command -v bcache &>/dev/null; then
            bcache make -B "$HDD_DEV" --writeback 2>&1
        else
            err "Failed to format backing device"
        fi
    }

    # --- Register devices ---
    log "Registering bcache devices..."
    echo "$SSD_LOOP" > /sys/fs/bcache/register 2>/dev/null || true
    echo "$HDD_DEV" > /sys/fs/bcache/register 2>/dev/null || true

    # Wait for devices to appear
    sleep 2

    # --- Attach cache to backing device ---
    local cset_uuid
    if ls /sys/fs/bcache/ 2>/dev/null | grep -qE '^[0-9a-f]{8}'; then
        cset_uuid=$(ls /sys/fs/bcache/ | grep -E '^[0-9a-f]{8}' | head -1)
        log "Cache set UUID: $cset_uuid"
        echo "$cset_uuid" > "$MOCK_DIR/cset.uuid"

        # Find the bcache backing device and attach
        for bdev in /sys/block/bcache*/bcache; do
            if [[ -f "$bdev/attach" ]]; then
                echo "$cset_uuid" > "$bdev/attach" 2>/dev/null || true
                log "Attached cache to $(dirname "$bdev" | xargs basename)"
            fi
        done

        # Enable writeback mode
        for bdev in /sys/block/bcache*/bcache; do
            if [[ -f "$bdev/cache_mode" ]]; then
                echo "writeback" > "$bdev/cache_mode" 2>/dev/null || true
            fi
        done
    fi

    log ""
    log "============================================"
    log "  Mock bcache environment ready!"
    log "============================================"
    log "  SSD (cache):   $SSD_LOOP"
    log "  HDD (backing): $HDD_DEV"
    log "  bcache device: /dev/bcache0 (if available)"
    log ""
    log "  To test a-bcached:"
    log "    sudo ./a-bcached -f -n -v"
    log ""
    log "  To tear down:"
    log "    sudo ./test/setup-mock-devices.sh teardown"
    log "============================================"
}

teardown() {
    log "Tearing down mock bcache environment..."

    # Stop bcache devices
    for bdev in /sys/block/bcache*/bcache; do
        if [[ -f "$bdev/stop" ]]; then
            log "Stopping $(dirname "$bdev" | xargs basename)..."
            echo 1 > "$bdev/stop" 2>/dev/null || true
        fi
    done

    # Unregister cache set
    if [[ -f "$MOCK_DIR/cset.uuid" ]]; then
        local uuid
        uuid=$(cat "$MOCK_DIR/cset.uuid")
        if [[ -f "/sys/fs/bcache/$uuid/stop" ]]; then
            log "Stopping cache set $uuid..."
            echo 1 > "/sys/fs/bcache/$uuid/stop" 2>/dev/null || true
        fi
    fi

    sleep 1

    # Remove dm-delay device
    if dmsetup info mock-hdd &>/dev/null 2>&1; then
        log "Removing dm-delay device..."
        dmsetup remove mock-hdd 2>/dev/null || true
    fi

    # Detach loop devices
    if [[ -f "$MOCK_DIR/ssd.loop" ]]; then
        local loop
        loop=$(cat "$MOCK_DIR/ssd.loop")
        log "Detaching SSD loop: $loop"
        losetup -d "$loop" 2>/dev/null || true
    fi

    if [[ -f "$MOCK_DIR/hdd.loop" ]]; then
        local loop
        loop=$(cat "$MOCK_DIR/hdd.loop")
        log "Detaching HDD loop: $loop"
        losetup -d "$loop" 2>/dev/null || true
    fi

    # Clean up files
    log "Removing mock images..."
    rm -rf "$MOCK_DIR"

    log "Teardown complete."
}

status() {
    echo "=== Loop Devices ==="
    losetup -l 2>/dev/null | grep -E "a-bcache|BACK-FILE" || echo "  (none)"

    echo ""
    echo "=== DM Devices ==="
    dmsetup ls 2>/dev/null | grep mock || echo "  (none)"

    echo ""
    echo "=== bcache Devices ==="
    if ls /sys/block/bcache* 2>/dev/null; then
        for bdev in /sys/block/bcache*/bcache; do
            echo "  $(dirname "$bdev" | xargs basename):"
            [[ -f "$bdev/state" ]] && echo "    state: $(cat "$bdev/state")"
            [[ -f "$bdev/cache_mode" ]] && echo "    cache_mode: $(cat "$bdev/cache_mode")"
            [[ -f "$bdev/dirty_data" ]] && echo "    dirty_data: $(cat "$bdev/dirty_data")"
            [[ -f "$bdev/writeback_running" ]] && echo "    writeback_running: $(cat "$bdev/writeback_running")"
        done
    else
        echo "  (none)"
    fi

    echo ""
    echo "=== Cache Sets ==="
    ls /sys/fs/bcache/ 2>/dev/null | grep -E '^[0-9a-f]{8}' || echo "  (none)"
}

# --- Entry point ---
check_root

case "${1:-setup}" in
    setup)
        check_deps
        setup
        ;;
    teardown)
        teardown
        ;;
    status)
        status
        ;;
    *)
        echo "Usage: $0 [setup|teardown|status]"
        exit 1
        ;;
esac
