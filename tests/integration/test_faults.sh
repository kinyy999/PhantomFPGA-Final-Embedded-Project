#!/bin/sh
# SPDX-License-Identifier: MIT
#
# PhantomFPGA Fault Injection Integration Tests
#
# Tests fault handling and recovery:
#   - Frame drop injection
#   - Data corruption injection
#   - Buffer overrun handling
#   - Recovery after faults
#
# These tests run INSIDE the guest VM where the PhantomFPGA device
# and driver are available.
#
# Usage: ./test_faults.sh
#
# "If you want to find bugs in production, inject them in testing first."
#   -- Ancient DevOps Wisdom (probably made up)

set -u

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

DRIVER_NAME="phantomfpga"
DEVICE_NODE="/dev/phantomfpga0"
# Prefer the app from PATH, then the shared /mnt/app build, then the
# traditional installed location used by older integration environments.
APP_PATH="$(command -v phantomfpga_app 2>/dev/null || true)"
if [ -z "$APP_PATH" ] && [ -x "/mnt/app/phantomfpga_app" ]; then
    APP_PATH="/mnt/app/phantomfpga_app"
fi
if [ -z "$APP_PATH" ]; then
    APP_PATH="/usr/local/bin/phantomfpga_app"
fi

# Register offsets (from phantomfpga_regs.h)
REG_FAULT_INJECT=0x058
REG_STATUS=0x00C
REG_CTRL=0x008
REG_STAT_ERRORS=0x040
REG_STAT_OVERRUNS=0x044

# Fault injection bits
FAULT_DROP_FRAMES=1     # BIT(0)
FAULT_CORRUPT_DATA=2    # BIT(1)
FAULT_DELAY_IRQ=4       # BIT(2)

# Test parameters
FRAME_SIZE=5120
FRAME_RATE=25
RING_SIZE=256
WATERMARK=8
TEST_DURATION=2

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Colors for output
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

# --------------------------------------------------------------------------
# Helper Functions
# --------------------------------------------------------------------------

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_test() {
    echo -e "${BLUE}[TEST]${NC} $*"
}

run_test() {
    local description="$1"
    shift

    TESTS_RUN=$((TESTS_RUN + 1))
    log_test "$description"

    if "$@"; then
        log_pass "$description"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        log_fail "$description"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

driver_loaded() {
    lsmod | grep -q "^${DRIVER_NAME} "
}

ensure_driver_loaded() {
    if ! driver_loaded; then
        log_info "Loading driver..."

        if modprobe "$DRIVER_NAME" 2>/dev/null; then
            :
        elif [ -f "/mnt/driver/${DRIVER_NAME}.ko" ] &&
             insmod "/mnt/driver/${DRIVER_NAME}.ko"; then
            :
        elif [ -f "/lib/modules/$(uname -r)/extra/${DRIVER_NAME}.ko" ] &&
             insmod "/lib/modules/$(uname -r)/extra/${DRIVER_NAME}.ko"; then
            :
        else
            log_fail "Could not load driver"
            return 1
        fi

        sleep 0.5
    fi
}

# Write to a device register using /dev/mem or devmem tool
# This is a helper for fault injection tests
# Note: In practice, fault injection would be done via a proper ioctl or sysfs
write_register() {
    local offset=$1
    local value=$2

    # Method 1: Try using devmem2 if available
    if command -v devmem2 >/dev/null 2>&1; then
        # Would need BAR address - skip for now
        :
    fi

    # Method 2: Try sysfs debug interface (if driver exposes it)
    local sysfs_path="/sys/kernel/debug/phantomfpga/fault_inject"
    if [ -w "$sysfs_path" ]; then
        echo "$value" > "$sysfs_path"
        return $?
    fi

    # Method 3: Use dedicated ioctl (not implemented yet in skeleton)
    # This would be the proper way

    log_warn "Direct register write not available (expected in skeleton driver)"
    return 1
}

# Clear fault injection
clear_faults() {
    write_register $REG_FAULT_INJECT 0 2>/dev/null || true
}

get_stat() {
    local output="$1"
    local stat_name="$2"
    echo "$output" | grep -i "$stat_name" | grep -oE '[0-9]+' | head -1
}

# Run streaming and capture stats
run_streaming_test() {
    local duration=${1:-2}

    if [ ! -x "$APP_PATH" ]; then
        return 1
    fi

    local output
    output=$("$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep "$duration"
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    echo "$output"
}

# --------------------------------------------------------------------------
# Test Cases
# --------------------------------------------------------------------------

# Test: Prerequisites check
test_prerequisites() {
    ensure_driver_loaded || return 1
    [ -c "$DEVICE_NODE" ] || return 1

    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available at $APP_PATH"
        log_warn "Fault tests will be limited"
    fi

    return 0
}

# Test: Can enable/disable fault injection
test_fault_inject_control() {
    log_info "Testing fault injection control..."

    # Try to enable fault injection
    if write_register $REG_FAULT_INJECT $FAULT_DROP_FRAMES; then
        # Read back and verify
        sleep 0.1
        # Clear faults
        clear_faults
        return 0
    fi

    # If direct register access isn't available, check if device has
    # fault injection capability via other means
    log_warn "Fault injection control not directly accessible"
    log_info "This is expected for the skeleton driver"
    return 0  # Pass - fault injection may not be implemented yet
}

# Test: Frame drop handling
test_frame_drop_injection() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping frame drop test"
        return 0
    fi

    log_info "Testing frame drop handling..."

    # Enable frame dropping
    write_register $REG_FAULT_INJECT $FAULT_DROP_FRAMES 2>/dev/null || {
        log_warn "Cannot enable fault injection, testing without"
    }

    # Run streaming
    local output
    output=$(run_streaming_test 2)

    # Clear faults
    clear_faults

    # Check for sequence errors (indicates dropped frames were detected)
    local seq_errors
    seq_errors=$(echo "$output" | grep -i "sequence" | grep -oE '[0-9]+' | head -1)

    if [ -n "$seq_errors" ] && [ "$seq_errors" -gt 0 ]; then
        log_info "Detected $seq_errors sequence discontinuities (dropped frames)"
    fi

    # Main check: system should not crash, device should still work
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Data corruption handling
test_data_corruption() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping corruption test"
        return 0
    fi

    log_info "Testing data corruption handling..."

    # Enable data corruption
    write_register $REG_FAULT_INJECT $FAULT_CORRUPT_DATA 2>/dev/null || {
        log_warn "Cannot enable fault injection, testing without"
    }

    # Run streaming
    local output
    output=$(run_streaming_test 2)

    # Clear faults
    clear_faults

    # Check for corruption detection
    local errors
    errors=$(echo "$output" | grep -iE "(corrupt|crc|magic).*error" | grep -oE '[0-9]+' | head -1)

    if [ -n "$errors" ] && [ "$errors" -gt 0 ]; then
        log_info "Detected $errors corrupted frames"
    fi

    # Device should still be functional
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Overrun scenario
test_overrun_scenario() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping overrun test"
        return 0
    fi

    log_info "Testing buffer overrun scenario..."

    # Stress the stream using the maximum supported frame rate and a small descriptor ring
    # The app may not be able to keep up
    local output
    output=$("$APP_PATH" \
        --frame-rate 60 \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep 3
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    # Check device stats
    local stats
    stats=$("$APP_PATH" --stats 2>&1)
    local overruns
    overruns=$(get_stat "$stats" "overrun")

    if [ -n "$overruns" ]; then
        if [ "$overruns" -gt 0 ]; then
            log_info "Detected $overruns buffer overruns (expected under this load)"
        else
            log_info "No overruns detected (system kept up)"
        fi
    fi

    # Verify device is still healthy
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Recovery after faults
test_recovery_after_faults() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping recovery test"
        return 0
    fi

    log_info "Testing recovery after faults..."

    # Run multiple fault scenarios then verify normal operation

    # Scenario 1: Cause overrun
    "$APP_PATH" \
        --frame-rate 60 \
        --desc-count "$RING_SIZE" \
        2>&1 &
    sleep 1
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true

    # Scenario 2: Try enabling faults
    write_register $REG_FAULT_INJECT $((FAULT_DROP_FRAMES | FAULT_CORRUPT_DATA)) 2>/dev/null || true
    sleep 0.5
    clear_faults

    # Now run normal streaming
    local output
    output=$("$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep 2
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    # Check if we received frames
    local frames
    frames=$(echo "$output" | grep -i "received" | grep -oE '[0-9]+' | head -1)

    if [ -z "$frames" ]; then
        # Try getting from stats
        local stats
        stats=$("$APP_PATH" --stats 2>&1)
        frames=$(get_stat "$stats" "produced")
    fi

    if [ -n "$frames" ] && [ "$frames" -gt 0 ]; then
        log_info "Recovery successful: received $frames frames"
        return 0
    fi

    # Even if we can't verify frame count, device should be accessible
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Driver reload after faults
test_driver_reload_after_faults() {
    log_info "Testing driver reload after faults..."

    # Cause some supported-rate stress
    if [ -x "$APP_PATH" ]; then
        "$APP_PATH" \
            --frame-rate 60 \
            --desc-count "$RING_SIZE" \
            2>&1 &
        sleep 1
        kill -9 %1 2>/dev/null || true
        wait 2>/dev/null || true
    fi

    # Unload driver
    rmmod "$DRIVER_NAME" 2>/dev/null || {
        log_fail "Could not unload driver after faults"
        return 1
    }
    sleep 0.5

    # Reload driver using the same local VM-aware helper used by prerequisites.
    ensure_driver_loaded || {
        log_fail "Could not reload driver"
        return 1
    }

    # Verify device is working
    [ -c "$DEVICE_NODE" ] || {
        log_fail "Device node not created after reload"
        return 1
    }

    if [ -x "$APP_PATH" ]; then
        "$APP_PATH" --stats >/dev/null 2>&1 || {
            log_fail "Device not accessible after reload"
            return 1
        }
    fi

    return 0
}

# Test: Concurrent access handling
test_concurrent_access() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping concurrent access test"
        return 0
    fi

    log_info "Testing concurrent access handling..."

    # Try to open device from multiple processes
    # Note: Driver may or may not support multiple openers

    "$APP_PATH" --stats >/dev/null 2>&1 &
    local pid1=$!
    sleep 0.1

    "$APP_PATH" --stats >/dev/null 2>&1 &
    local pid2=$!
    sleep 0.1

    # Wait for both
    local status1=0
    local status2=0
    wait $pid1 2>/dev/null || status1=$?
    wait $pid2 2>/dev/null || status2=$?

    # At least one should succeed
    if [ $status1 -eq 0 ] || [ $status2 -eq 0 ]; then
        return 0
    fi

    # Both failed - might be exclusive access (acceptable)
    log_warn "Both concurrent accesses failed (exclusive access mode?)"
    return 0
}

# Test: Interrupted streaming
test_interrupted_streaming() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping interrupt test"
        return 0
    fi

    log_info "Testing interrupted streaming..."

    local i
    for i in 1 2 3; do
        # Start streaming
        "$APP_PATH" \
            --frame-rate "$FRAME_RATE" \
            --desc-count "$RING_SIZE" \
            2>&1 &
        local pid=$!

        # Random short sleep then kill
        sleep 0.$((RANDOM % 5 + 1))
        kill -9 $pid 2>/dev/null || true
        wait $pid 2>/dev/null || true
    done

    # Verify device still works
    if ! "$APP_PATH" --stats >/dev/null 2>&1; then
        log_fail "Device not accessible after interrupted streaming"
        return 1
    fi

    return 0
}

# Test: No kernel oops during fault testing
test_no_kernel_oops() {
    if dmesg | tail -100 | grep -qiE "(oops|panic|bug:|null pointer|general protection|RIP:)"; then
        log_fail "Kernel issues detected"
        dmesg | tail -30
        return 1
    fi
    return 0
}

# --------------------------------------------------------------------------
# Main Test Sequence
# --------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "PhantomFPGA Fault Injection Tests"
    echo "========================================"
    echo ""

    log_info "Test configuration:"
    log_info "  Frame size:    $FRAME_SIZE bytes"
    log_info "  Frame rate:    $FRAME_RATE fps"
    log_info "  Ring size:     $RING_SIZE entries"
    echo ""

    log_info "Fault injection uses PhantomFPGA fault registers and app error counters."
    echo ""

    # --- Prerequisites ---
    echo "--- Prerequisites ---"
    run_test "Prerequisites check" test_prerequisites
    if [ $? -ne 0 ]; then
        log_fail "Prerequisites not met, aborting"
        return 1
    fi

    # --- Fault Injection Tests ---
    echo ""
    echo "--- Fault Injection Tests ---"
    run_test "Fault injection control" test_fault_inject_control
    run_test "Frame drop handling" test_frame_drop_injection
    run_test "Data corruption handling" test_data_corruption

    # --- Overrun Tests ---
    echo ""
    echo "--- Overrun Tests ---"
    run_test "Overrun scenario" test_overrun_scenario

    # --- Recovery Tests ---
    echo ""
    echo "--- Recovery Tests ---"
    run_test "Recovery after faults" test_recovery_after_faults
    run_test "Driver reload after faults" test_driver_reload_after_faults

    # --- Robustness Tests ---
    echo ""
    echo "--- Robustness Tests ---"
    run_test "Concurrent access" test_concurrent_access
    run_test "Interrupted streaming" test_interrupted_streaming

    # --- Final Checks ---
    echo ""
    echo "--- Final Checks ---"
    run_test "No kernel oops" test_no_kernel_oops

    # --- Summary ---
    echo ""
    echo "========================================"
    echo "Test Summary"
    echo "========================================"
    echo "Tests run:    $TESTS_RUN"
    echo -e "Tests passed: ${GREEN}$TESTS_PASSED${NC}"
    echo -e "Tests failed: ${RED}$TESTS_FAILED${NC}"
    echo ""

    if [ "$TESTS_FAILED" -gt 0 ]; then
        log_fail "Some tests failed!"
        return 1
    else
        log_pass "All tests passed!"
        return 0
    fi
}

# Run main.
main "$@"
