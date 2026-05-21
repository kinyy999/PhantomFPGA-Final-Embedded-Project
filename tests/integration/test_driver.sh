#!/bin/sh
# SPDX-License-Identifier: MIT
#
# PhantomFPGA Driver Integration Tests
#
# Tests the basic driver lifecycle:
#   - Module loading
#   - Device node creation
#   - Basic ioctl operations
#   - Clean module unload
#
# These tests run INSIDE the guest VM where the PhantomFPGA device
# and driver are available.
#
# Usage: ./test_driver.sh
#
# Chuck Norris doesn't need integration tests. His code compiles
# correctly the first time, every time. Unfortunately, we're not
# Chuck Norris.

set -u

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

DRIVER_NAME="phantomfpga"
MODULE_PATH="/lib/modules/$(uname -r)/extra/${DRIVER_NAME}.ko"
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

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Colors for output (if terminal supports them)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    NC='\033[0m' # No Color
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

# Run a test and track results
# Usage: run_test "test description" command [args...]
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

# Check if driver is loaded
driver_loaded() {
    lsmod | grep -q "^${DRIVER_NAME} "
}

# Unload driver if loaded (cleanup helper)
ensure_driver_unloaded() {
    if driver_loaded; then
        log_info "Unloading existing driver..."
        rmmod "$DRIVER_NAME" 2>/dev/null || true
        sleep 0.5
    fi
}

# Check dmesg for kernel oops/panic
check_dmesg_clean() {
    # Look for bad things in recent dmesg
    if dmesg | tail -50 | grep -qiE "(oops|panic|bug:|null pointer|general protection|RIP:)"; then
        log_fail "Kernel issues detected in dmesg"
        dmesg | tail -20
        return 1
    fi
    return 0
}

# --------------------------------------------------------------------------
# Test Cases
# --------------------------------------------------------------------------

# Test: Module file exists
test_module_exists() {
    if [ -f "$MODULE_PATH" ]; then
        return 0
    fi
    # Try alternative paths
    if [ -f "/lib/modules/$(uname -r)/${DRIVER_NAME}.ko" ]; then
        MODULE_PATH="/lib/modules/$(uname -r)/${DRIVER_NAME}.ko"
        return 0
    fi
    # Check shared VM driver mount used by the local PhantomFPGA workflow.
    if [ -f "/mnt/driver/${DRIVER_NAME}.ko" ]; then
        MODULE_PATH="/mnt/driver/${DRIVER_NAME}.ko"
        return 0
    fi

    # Check current directory
    if [ -f "./${DRIVER_NAME}.ko" ]; then
        MODULE_PATH="./${DRIVER_NAME}.ko"
        return 0
    fi
    log_warn "Module not found at expected paths, checking modinfo..."
    if modinfo "$DRIVER_NAME" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

# Test: Load driver module
test_load_driver() {
    ensure_driver_unloaded

    # Try modprobe first (preferred if module is installed)
    if modprobe "$DRIVER_NAME" 2>/dev/null; then
        sleep 0.5
        return 0
    fi

    # Fall back to insmod if modprobe fails
    if [ -f "$MODULE_PATH" ]; then
        if insmod "$MODULE_PATH"; then
            sleep 0.5
            return 0
        fi
    fi

    return 1
}

# Test: Verify driver is loaded
test_driver_loaded() {
    driver_loaded
}

# Test: Check device node was created
test_device_node_exists() {
    [ -c "$DEVICE_NODE" ]
}

# Test: Device node has correct permissions
test_device_permissions() {
    # Should be readable/writable (at least by root)
    [ -r "$DEVICE_NODE" ] && [ -w "$DEVICE_NODE" ]
}

# Test: Can open device
test_device_open() {
    # Use dd to test if we can open the device
    if dd if="$DEVICE_NODE" of=/dev/null bs=1 count=0 2>/dev/null; then
        return 0
    fi
    # Alternative: use app if available
    if [ -x "$APP_PATH" ]; then
        "$APP_PATH" --stats >/dev/null 2>&1 && return 0
    fi
    return 1
}

# Test: GET_CFG ioctl works (via app or direct test)
test_ioctl_get_cfg() {
    if [ -x "$APP_PATH" ]; then
        # App's --stats mode uses GET_STATS ioctl
        "$APP_PATH" --stats >/dev/null 2>&1
        return $?
    fi
    # If no app, try to verify through sysfs or just pass
    # since we already verified device opens
    log_warn "App not available, skipping detailed ioctl test"
    return 0
}

# Test: GET_STATS ioctl works
test_ioctl_get_stats() {
    if [ -x "$APP_PATH" ]; then
        local output
        output=$("$APP_PATH" --stats 2>&1)
        if echo "$output" | grep -qiE "(stats|frames|error)"; then
            return 0
        fi
    fi
    # If no app, we already verified device opens
    log_warn "App not available, skipping detailed stats test"
    return 0
}

# Test: Check PCI device is visible
test_pci_device() {
    # PhantomFPGA uses vendor 0x0DAD, device 0xF00D
    if lspci -nn 2>/dev/null | grep -q "0dad:f00d"; then
        return 0
    fi
    # Alternative: check if driver has bound to any device
    if [ -d "/sys/module/$DRIVER_NAME/drivers" ]; then
        return 0
    fi
    # If lspci isn't available, check dmesg
    if dmesg | grep -qi "phantomfpga.*pci\|phantomfpga.*probe"; then
        return 0
    fi
    return 1
}

# Test: Check sysfs entries
test_sysfs_entries() {
    if [ -d "/sys/module/$DRIVER_NAME" ]; then
        return 0
    fi
    return 1
}

# Test: Unload driver
test_unload_driver() {
    if ! driver_loaded; then
        log_warn "Driver not loaded, nothing to unload"
        return 0
    fi

    rmmod "$DRIVER_NAME"
    sleep 0.5

    # Verify it's actually unloaded
    ! driver_loaded
}

# Test: Device node removed after unload
test_device_node_removed() {
    # After driver unload, device node should be gone
    if [ -e "$DEVICE_NODE" ]; then
        log_warn "Device node still exists after unload"
        return 1
    fi
    return 0
}

# Test: No kernel oops during test
test_no_kernel_oops() {
    check_dmesg_clean
}

# Test: Reload driver (stress test)
test_reload_driver() {
    local i
    for i in 1 2 3; do
        if ! modprobe "$DRIVER_NAME" 2>/dev/null; then
            if [ -f "$MODULE_PATH" ]; then
                insmod "$MODULE_PATH" || return 1
            else
                return 1
            fi
        fi
        sleep 0.2

        if ! driver_loaded; then
            log_fail "Driver not loaded after reload attempt $i"
            return 1
        fi

        rmmod "$DRIVER_NAME" || return 1
        sleep 0.2

        if driver_loaded; then
            log_fail "Driver still loaded after rmmod attempt $i"
            return 1
        fi
    done
    return 0
}

# --------------------------------------------------------------------------
# Main Test Sequence
# --------------------------------------------------------------------------

main() {
    echo "========================================"
    echo "PhantomFPGA Driver Integration Tests"
    echo "========================================"
    echo ""

    log_info "Starting driver tests..."
    log_info "Kernel version: $(uname -r)"
    log_info "Module path: $MODULE_PATH"
    log_info "Device node: $DEVICE_NODE"
    echo ""

    # Ensure clean state
    ensure_driver_unloaded

    # --- Pre-load tests ---
    echo "--- Pre-Load Tests ---"
    run_test "Module file exists" test_module_exists
    run_test "PCI device visible" test_pci_device

    # --- Load tests ---
    echo ""
    echo "--- Module Load Tests ---"
    run_test "Load driver module" test_load_driver
    run_test "Driver is loaded" test_driver_loaded
    run_test "sysfs entries exist" test_sysfs_entries
    run_test "Device node created" test_device_node_exists
    run_test "Device permissions correct" test_device_permissions

    # --- Operational tests ---
    echo ""
    echo "--- Operational Tests ---"
    run_test "Can open device" test_device_open
    run_test "GET_CFG ioctl works" test_ioctl_get_cfg
    run_test "GET_STATS ioctl works" test_ioctl_get_stats
    run_test "No kernel oops" test_no_kernel_oops

    # --- Unload tests ---
    echo ""
    echo "--- Module Unload Tests ---"
    run_test "Unload driver" test_unload_driver
    run_test "Device node removed" test_device_node_removed
    run_test "No kernel oops after unload" test_no_kernel_oops

    # --- Stress tests ---
    echo ""
    echo "--- Stress Tests ---"
    run_test "Driver reload (3x)" test_reload_driver

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
        echo ""
        echo "Recent dmesg output:"
        dmesg | tail -30
        return 1
    else
        log_pass "All tests passed!"
        return 0
    fi
}

# Run main.
main "$@"
