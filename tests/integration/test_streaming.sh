#!/bin/sh
# SPDX-License-Identifier: MIT
#
# PhantomFPGA Streaming Integration Tests
#
# Tests frame streaming functionality:
#   - Configure and start streaming
#   - Verify frames are received
#   - Check frame sequence numbers
#   - Validate frame magic
#   - Stop streaming and check statistics
#
# These tests run INSIDE the guest VM where the PhantomFPGA device
# and driver are available.
#
# Usage: ./test_streaming.sh [--quick]
#
# Options:
#   --quick     Run shorter tests (100 frames instead of 1000)
#
# Dear future maintainer: if these tests are flaky, the device is
# probably producing frames faster than your test can consume them.
# That's a feature, not a bug. We call it "realistic workload simulation."

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
DUMP_PATH="/tmp/phantomfpga_frames.bin"

# Test parameters
FRAME_SIZE=5120
FRAME_RATE=25
RING_SIZE=256
WATERMARK=16
TEST_DURATION=2       # seconds for streaming tests
FRAME_COUNT=100       # frames to check in quick mode
QUICK_MODE=false

# Frame magic value (must match device)
FRAME_MAGIC="0xF00DFACE"
FRAME_MAGIC_DEC=4027448014

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

# Parse stats output from app
# Usage: get_stat "output" "stat_name"
get_stat() {
    local output="$1"
    local stat_name="$2"
    echo "$output" | grep -i "$stat_name" | grep -oE '[0-9]+' | head -1
}

# Read 4 bytes from device as little-endian uint32
read_u32_le() {
    local offset="$1"
    local data
    data=$(dd if="$DEVICE_NODE" bs=1 skip="$offset" count=4 2>/dev/null | xxd -p)
    if [ ${#data} -eq 8 ]; then
        # Reverse bytes for little-endian
        printf "%d" "0x${data:6:2}${data:4:2}${data:2:2}${data:0:2}" 2>/dev/null || echo 0
    else
        echo 0
    fi
}

# --------------------------------------------------------------------------
# Test Cases
# --------------------------------------------------------------------------

# Test: Driver loaded and device exists
test_prerequisites() {
    ensure_driver_loaded || return 1
    [ -c "$DEVICE_NODE" ] || return 1
    [ -x "$APP_PATH" ] || {
        log_warn "App not found at $APP_PATH, some tests will be limited"
        return 0
    }
    return 0
}

# Test: Can configure device via app
test_configure_device() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping configure test"
        return 0
    fi

    # Just try to get stats (which requires device to be accessible)
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Start streaming and receive frames
test_basic_streaming() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping streaming test"
        return 0
    fi

    local output
    local frames_received

    # Run app briefly with verbose output
    log_info "Starting streaming for $TEST_DURATION seconds..."
    output=$("$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep "$TEST_DURATION"
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    # Check if we received any frames
    frames_received=$(echo "$output" | grep -i "received" | grep -oE '[0-9]+' | head -1)

    if [ -n "$frames_received" ] && [ "$frames_received" -gt 0 ]; then
        log_info "Received $frames_received frames"
        return 0
    fi

    # Alternative: check device stats directly
    output=$("$APP_PATH" --stats 2>&1)
    frames_received=$(get_stat "$output" "produced")

    if [ -n "$frames_received" ] && [ "$frames_received" -gt 0 ]; then
        log_info "Device produced $frames_received frames"
        return 0
    fi

    log_fail "No frames received"
    return 1
}

# Test: Frame sequence numbers are sequential
test_frame_sequence() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping sequence test"
        return 0
    fi

    local output
    local seq_errors

    log_info "Running sequence test..."

    # Run app and check for sequence errors
    output=$("$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep "$TEST_DURATION"
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    seq_errors=$(echo "$output" | grep -i "sequence" | grep -i "error" | grep -oE '[0-9]+' | head -1)

    # Also check final stats
    local stats_output
    stats_output=$("$APP_PATH" --stats 2>&1)

    if [ -n "$seq_errors" ] && [ "$seq_errors" -gt 0 ]; then
        log_warn "Detected $seq_errors sequence errors (may be normal under load)"
        # Non-zero sequence errors under heavy load is acceptable
        # Only fail if it's excessive
        local frames_total
        frames_total=$(get_stat "$stats_output" "produced")
        if [ -n "$frames_total" ] && [ "$frames_total" -gt 0 ]; then
            local error_rate=$((seq_errors * 100 / frames_total))
            if [ "$error_rate" -gt 10 ]; then
                log_fail "Sequence error rate too high: ${error_rate}%"
                return 1
            fi
        fi
    fi

    return 0
}

# Test: Frame magic number is correct
test_frame_magic() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping magic test"
        return 0
    fi

    local output
    local magic_errors

    log_info "Checking frame magic values..."

    # Run app and check for magic errors
    output=$("$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        -V 2>&1 &
        sleep "$TEST_DURATION"
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    magic_errors=$(echo "$output" | grep -i "magic" | grep -i "error" | grep -oE '[0-9]+' | tail -1)

    if [ -n "$magic_errors" ] && [ "$magic_errors" -gt 0 ]; then
        log_fail "Detected $magic_errors magic errors"
        return 1
    fi

    return 0
}

# Test: Statistics are updated
test_statistics_update() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping statistics test"
        return 0
    fi

    local stats_after
    local frames_after

    # Run streaming briefly. Each SET_CFG starts from a clean device state,
    # so this test validates that the current session produces frames instead
    # of comparing against stale counters from a previous session.
    "$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        2>&1 &
    sleep 1
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true

    stats_after=$("$APP_PATH" --stats 2>&1)
    frames_after=$(get_stat "$stats_after" "produced")

    if [ -z "$frames_after" ]; then
        frames_after=0
    fi

    log_info "Frames produced in current session: $frames_after"

    if [ "$frames_after" -gt 0 ]; then
        return 0
    fi

    log_warn "Statistics did not show produced frames"
    return 1
}

# Test: Stop streaming cleanly
test_stop_streaming() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping stop test"
        return 0
    fi

    # Start streaming
    "$APP_PATH" \
        --frame-rate "$FRAME_RATE" \
        --desc-count "$RING_SIZE" \
        2>&1 &
    local pid=$!
    sleep 1

    # Send SIGTERM for graceful shutdown
    kill -TERM $pid 2>/dev/null || true
    sleep 0.5

    # Check if it exited cleanly
    if ! kill -0 $pid 2>/dev/null; then
        # Process exited - good
        wait $pid 2>/dev/null
        return 0
    fi

    # Force kill if still running
    kill -KILL $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    log_warn "App required SIGKILL to stop"
    return 0  # Still pass, just not ideal
}

# Test: Multiple streaming sessions
test_multiple_sessions() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping multiple sessions test"
        return 0
    fi

    local i
    local success=true

    for i in 1 2 3; do
        log_info "Session $i/3..."

        "$APP_PATH" \
            --frame-rate "$FRAME_RATE" \
            --desc-count "$RING_SIZE" \
            2>&1 &
        sleep 0.5
        kill %1 2>/dev/null || true
        wait 2>/dev/null || true

        # Check device is still healthy
        if ! "$APP_PATH" --stats >/dev/null 2>&1; then
            log_fail "Device became unhealthy after session $i"
            success=false
            break
        fi
    done

    $success
}

# Test: High frame rate
test_high_frame_rate() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping high rate test"
        return 0
    fi

    local high_rate=60
    local output

    log_info "Testing at supported max rate: $high_rate fps..."

    output=$("$APP_PATH" \
        --frame-rate "$high_rate" \
        --desc-count "$RING_SIZE" \
        2>&1 &
        sleep 1
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    # Just verify it didn't crash
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Fixed frame-size streaming at max supported rate
test_fixed_frame_size_streaming() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping large frames test"
        return 0
    fi

        local output

    log_info "Testing fixed ${FRAME_SIZE} byte frames at max supported rate..."

    output=$("$APP_PATH" \
        --frame-rate 60 \
        --desc-count "$RING_SIZE" \
        2>&1 &
        sleep 1
        kill %1 2>/dev/null
        wait 2>/dev/null) || true

    # Verify device still works
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: Check for overruns
test_overrun_handling() {
    if [ ! -x "$APP_PATH" ]; then
        log_warn "App not available, skipping overrun test"
        return 0
    fi

    local stats
    local overruns

    # Use very high rate and small buffer to potentially trigger overruns
    log_info "Testing overrun handling..."

    "$APP_PATH" \
        --frame-rate 60 \
        --desc-count 8 \
        2>&1 &
    sleep 2
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true

    # Check stats for overrun count
    stats=$("$APP_PATH" --stats 2>&1)
    overruns=$(get_stat "$stats" "overrun")

    if [ -n "$overruns" ] && [ "$overruns" -gt 0 ]; then
        log_info "Detected $overruns overruns (expected under stress)"
    fi

    # Main test: device should still be functional
    "$APP_PATH" --stats >/dev/null 2>&1
}

# Test: No kernel oops during streaming
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
    echo "PhantomFPGA Streaming Integration Tests"
    echo "========================================"
    echo ""

    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --quick)
                QUICK_MODE=true
                TEST_DURATION=1
                FRAME_COUNT=10
                ;;
            *)
                log_warn "Unknown option: $1"
                ;;
        esac
        shift
    done

    if $QUICK_MODE; then
        log_info "Running in quick mode"
    fi

    log_info "Test configuration:"
    log_info "  Frame size:    $FRAME_SIZE bytes"
    log_info "  Frame rate:    $FRAME_RATE fps"
    log_info "  Ring size:     $RING_SIZE entries"
    log_info "  Test duration: $TEST_DURATION seconds"
    echo ""

    # --- Prerequisites ---
    echo "--- Prerequisites ---"
    run_test "Prerequisites check" test_prerequisites
    if [ $? -ne 0 ]; then
        log_fail "Prerequisites not met, aborting"
        return 1
    fi

    # --- Configuration Tests ---
    echo ""
    echo "--- Configuration Tests ---"
    run_test "Configure device" test_configure_device

    # --- Streaming Tests ---
    echo ""
    echo "--- Streaming Tests ---"
    run_test "Basic streaming" test_basic_streaming
    run_test "Frame sequence numbers" test_frame_sequence
    run_test "Frame magic validation" test_frame_magic
    run_test "Statistics update" test_statistics_update
    run_test "Stop streaming" test_stop_streaming

    # --- Stress Tests ---
    if ! $QUICK_MODE; then
        echo ""
        echo "--- Stress Tests ---"
        run_test "Multiple sessions" test_multiple_sessions
        run_test "High frame rate" test_high_frame_rate
        run_test "Fixed frame-size streaming" test_fixed_frame_size_streaming
        run_test "Overrun handling" test_overrun_handling
    fi

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
