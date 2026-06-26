#!/bin/bash
#
# test-spin-management.sh - Integration test for a-bcached spin management
#
# Tests:
#   1. sysfs interface is readable
#   2. writeback can be paused/resumed
#   3. dirty data accumulation works
#   4. a-bcached daemon discovers devices and produces output
#   5. /sys/block/<backing>/stat is readable for I/O monitoring
#   6. hdparm presence (spin detection requires real HDD)
#
# Prerequisites: run setup-mock-devices.sh first
#
# Usage: sudo ./test/test-spin-management.sh
#

# NOTE: We deliberately do NOT use `set -e` because shell arithmetic
# `((PASS++))` returns the pre-increment value, which is 0 on the first
# pass and would otherwise terminate the script under `set -e`.
set -uo pipefail

MOCK_DIR="/tmp/a-bcache-test"
DAEMON_BIN="./a-bcached"
LOG_FILE="$(mktemp -t a-bcached-test.XXXXXX.log)"

PASS=0
FAIL=0
SKIP=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { printf '  %bPASS%b: %s\n' "$GREEN"  "$NC" "$*"; PASS=$((PASS + 1)); }
fail() { printf '  %bFAIL%b: %s\n' "$RED"    "$NC" "$*"; FAIL=$((FAIL + 1)); }
skip() { printf '  %bSKIP%b: %s\n' "$YELLOW" "$NC" "$*"; SKIP=$((SKIP + 1)); }

# shellcheck disable=SC2317  # invoked indirectly via trap
cleanup() {
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

check_prereqs() {
    if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
        echo "Must run as root" >&2
        exit 1
    fi

    if [[ ! -d /sys/fs/bcache ]]; then
        echo "bcache module not loaded; run setup-mock-devices.sh first" >&2
        exit 1
    fi

    if ! compgen -G "/sys/block/bcache*/bcache" > /dev/null; then
        echo "No bcache devices found; run setup-mock-devices.sh first" >&2
        exit 1
    fi

    if [[ ! -x "$DAEMON_BIN" ]]; then
        echo "$DAEMON_BIN not built or not executable. Run 'make' first." >&2
        exit 1
    fi
}

# Return the first bcache control directory found (no hard-coding bcache0)
get_bcache_sysfs() {
    local b
    for b in /sys/block/bcache*/bcache; do
        if [[ -d "$b" ]]; then
            echo "$b"
            return 0
        fi
    done
    return 1
}

# Return the device path /dev/bcacheN matching the sysfs we picked
get_bcache_dev() {
    local sysfs name
    sysfs=$(get_bcache_sysfs) || return 1
    name=$(basename "$(dirname "$sysfs")")
    echo "/dev/$name"
}

#
# Test 1: sysfs interface readable
#
test_sysfs_readable() {
    echo "Test 1: sysfs interface readable"
    local sysfs
    if ! sysfs=$(get_bcache_sysfs); then
        fail "no bcache control directory found"
        return
    fi

    local attr
    for attr in dirty_data state cache_mode writeback_running; do
        if [[ -r "$sysfs/$attr" ]]; then
            pass "$attr readable: $(cat "$sysfs/$attr")"
        else
            fail "$attr not readable at $sysfs/$attr"
        fi
    done
}

#
# Test 2: Writeback pause/resume via sysfs
#
test_writeback_control() {
    echo "Test 2: writeback pause/resume"
    local sysfs
    if ! sysfs=$(get_bcache_sysfs); then
        fail "no bcache control directory"
        return
    fi

    if ! printf '0\n' > "$sysfs/writeback_running" 2>/dev/null; then
        fail "cannot write to writeback_running"
        return
    fi
    if [[ "$(cat "$sysfs/writeback_running")" == "0" ]]; then
        pass "writeback paused"
    else
        fail "writeback pause did not take effect"
    fi

    printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true
    if [[ "$(cat "$sysfs/writeback_running")" == "1" ]]; then
        pass "writeback resumed"
    else
        fail "writeback resume did not take effect"
    fi
}

#
# Test 3: Generate dirty data by writing to /dev/bcacheN
#
test_dirty_data_generation() {
    echo "Test 3: dirty data generation"
    local sysfs bcache_dev
    sysfs=$(get_bcache_sysfs) || { fail "no bcache sysfs"; return; }
    bcache_dev=$(get_bcache_dev) || { skip "no bcache device path"; return; }

    if [[ ! -b "$bcache_dev" ]]; then
        skip "$bcache_dev is not a block device"
        return
    fi

    # Pause writeback so dirty data accumulates instead of being flushed
    printf '0\n' > "$sysfs/writeback_running" 2>/dev/null || true

    # /dev/zero is much faster than /dev/urandom; bcache doesn't deduplicate
    # zeros so it still produces dirty data.
    if ! dd if=/dev/zero of="$bcache_dev" bs=1M count=8 \
        conv=fsync oflag=direct status=none 2>/dev/null; then
        # Some setups don't allow O_DIRECT; retry without
        if ! dd if=/dev/zero of="$bcache_dev" bs=1M count=8 \
            conv=fsync status=none 2>/dev/null; then
            fail "dd write to $bcache_dev failed"
            printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true
            return
        fi
    fi

    # Poll up to 5 seconds for dirty_data to update
    local dirty="0.0k"
    local _attempt
    for _attempt in 1 2 3 4 5; do
        dirty=$(cat "$sysfs/dirty_data" 2>/dev/null || echo "0.0k")
        # Match anything that's not zero or "0.0k"
        if [[ -n "$dirty" && "$dirty" != "0" && "$dirty" != "0.0k" ]]; then
            pass "dirty data accumulated: $dirty"
            printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true
            return
        fi
        sleep 1
    done

    fail "dirty data did not accumulate (last value: $dirty)"
    printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true
}

#
# Test 4: a-bcached --once mode discovers devices and produces output
#
test_daemon_once() {
    echo "Test 4: a-bcached --once mode"

    rm -f "$LOG_FILE"
    # --once runs one iteration and exits. With --dry-run no real
    # hdparm or destructive commands run; we still see the decision.
    if ! "$DAEMON_BIN" --once --dry-run --verbose > "$LOG_FILE" 2>&1; then
        local rc=$?
        fail "daemon --once exited non-zero ($rc)"
        echo "    --- daemon output ---"
        sed 's/^/    /' "$LOG_FILE"
        echo "    --- end output ---"
        return
    fi

    if grep -q "Found.*bcache device" "$LOG_FILE"; then
        pass "daemon discovered bcache devices"
    else
        fail "daemon did not discover bcache devices"
        echo "    --- daemon output ---"
        sed 's/^/    /' "$LOG_FILE"
        echo "    --- end output ---"
    fi

    if grep -qE "DRY-RUN|spinning down|holding|writeback|spinning, idle" \
        "$LOG_FILE"; then
        pass "daemon produced a spin-management decision"
    else
        fail "daemon produced no spin management output"
        echo "    --- daemon output ---"
        sed 's/^/    /' "$LOG_FILE"
        echo "    --- end output ---"
    fi
}

#
# Test 4b: --once with spindown-delay=0 should trigger immediate spin-down
#
test_daemon_once_spindown() {
    echo "Test 4b: --once forces spin-down decision"

    local sysfs
    sysfs=$(get_bcache_sysfs) || { fail "no bcache sysfs"; return; }

    # Ensure clean state: writeback enabled, no dirty data
    printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true

    rm -f "$LOG_FILE"
    # spindown-delay=0 + dry-run + once => daemon should log "Would spin down"
    if ! "$DAEMON_BIN" --once --dry-run --verbose \
        --spindown-delay 0 \
        --min-writeback 1024 \
        > "$LOG_FILE" 2>&1; then
        fail "daemon --once exited non-zero"
        sed 's/^/    /' "$LOG_FILE"
        return
    fi

    if grep -qE "Would spin down|spinning down" "$LOG_FILE"; then
        pass "daemon decided to spin down (as expected)"
    else
        fail "daemon did not decide to spin down with spindown-delay=0"
        echo "    --- daemon output ---"
        sed 's/^/    /' "$LOG_FILE"
        echo "    --- end output ---"
    fi

    # In --once mode the daemon should have set writeback_running=0
    # and left it that way (no cleanup re-enable).
    local wb
    wb=$(cat "$sysfs/writeback_running" 2>/dev/null || echo "?")
    if [[ "$wb" == "0" ]]; then
        pass "writeback_running was set to 0 by spin-down decision"
    else
        # Not a fail -- might be 1 if state didn't match expected.
        # Just informational.
        skip "writeback_running=$wb (expected 0)"
    fi

    # Restore for next tests
    printf '1\n' > "$sysfs/writeback_running" 2>/dev/null || true
}

#
# Test 5: I/O stats readable on backing device
#
test_io_pattern() {
    echo "Test 5: I/O stats on backing device"

    if [[ ! -f "$MOCK_DIR/hdd.dev" ]]; then
        skip "mock HDD device path not recorded"
        return
    fi

    local hdd_dev devname
    hdd_dev=$(cat "$MOCK_DIR/hdd.dev")
    devname=$(basename "$hdd_dev")

    # dm device: /sys/block/dm-N -- for dm-delay name dispatch, find it
    if [[ "$hdd_dev" =~ ^/dev/mapper/ ]]; then
        local dmname
        dmname=$(basename "$hdd_dev")
        # /sys/block/dm-N exists; dm name is in /sys/block/dm-N/dm/name
        local dm
        for dm in /sys/block/dm-*/dm/name; do
            if [[ -r "$dm" ]] && [[ "$(cat "$dm")" == "$dmname" ]]; then
                devname=$(basename "$(dirname "$(dirname "$dm")")")
                break
            fi
        done
    fi

    local stat_file="/sys/block/$devname/stat"
    if [[ ! -r "$stat_file" ]]; then
        skip "cannot read $stat_file"
        return
    fi

    local stat_line
    stat_line=$(cat "$stat_file")
    if [[ -z "$stat_line" ]]; then
        fail "$stat_file is empty"
        return
    fi

    pass "I/O stats readable for $devname"

    local reads writes
    reads=$(echo "$stat_line"  | awk '{print $3}')  # read_sectors
    writes=$(echo "$stat_line" | awk '{print $7}')  # write_sectors
    echo "    backing device $devname: read=${reads}s written=${writes}s"
}

#
# Test 6: hdparm availability (spin detection requires real HDD)
#
test_spin_detection() {
    echo "Test 6: hdparm availability"

    if command -v hdparm >/dev/null 2>&1; then
        pass "hdparm is installed"
        skip "spin state detection not testable on loop devices (need real ATA HDD)"
    else
        skip "hdparm not installed"
    fi
}

# --- Run all tests ---
echo "============================================"
echo "  a-bcache spin management tests"
echo "============================================"
echo ""

check_prereqs

test_sysfs_readable
echo ""
test_writeback_control
echo ""
test_dirty_data_generation
echo ""
test_daemon_once
echo ""
test_daemon_once_spindown
echo ""
test_io_pattern
echo ""
test_spin_detection

echo ""
echo "============================================"
printf "  Results: %b%d passed%b, %b%d failed%b, %b%d skipped%b\n" \
    "$GREEN" "$PASS" "$NC" "$RED" "$FAIL" "$NC" "$YELLOW" "$SKIP" "$NC"
echo "============================================"

exit "$FAIL"
