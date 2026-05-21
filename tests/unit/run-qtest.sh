#!/bin/bash
#
# Run PhantomFPGA QTest unit tests
#
# This script helps run the QTest against a pre-built QEMU.
# It handles QEMU startup, test execution, and cleanup.
#
# Usage:
#   ./run-qtest.sh                    # Auto-detect QEMU
#   ./run-qtest.sh /path/to/qemu      # Use specific QEMU
#   ./run-qtest.sh --list             # List available tests
#   ./run-qtest.sh --test <name>      # Run specific test
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOPDIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Default paths
QEMU_BINARY="${QEMU_BINARY:-$TOPDIR/platform/qemu/build/qemu-system-x86_64}"
QEMU_SRC="${QEMU_SRC:-$TOPDIR/platform/qemu/upstream}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
PhantomFPGA QTest Runner

Usage: $0 [OPTIONS] [QEMU_PATH]

Options:
    --list          List available tests
    --test <name>   Run specific test (e.g., /phantomfpga/dev_id)
    --verbose       Verbose output
    --help          Show this help

Environment:
    QEMU_BINARY     Path to QEMU binary (default: $QEMU_BINARY)
    QEMU_SRC        Path to QEMU source tree (default: $QEMU_SRC)

Examples:
    $0                              # Run all tests
    $0 --test /phantomfpga/dev_id   # Run specific test
    $0 /custom/path/qemu            # Use custom QEMU

Note:
    For full QTest execution, this script requires QEMU to be built
    with the test infrastructure. If that's not available, it will
    show test descriptions instead.

EOF
}

# Parse arguments
SPECIFIC_TEST=""
VERBOSE=""
LIST_TESTS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --list)
            LIST_TESTS=1
            shift
            ;;
        --test)
            SPECIFIC_TEST="$2"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            if [[ -x "$1" ]]; then
                QEMU_BINARY="$1"
            else
                echo -e "${RED}Error: Unknown option or invalid QEMU path: $1${NC}"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Check QEMU binary
if [[ ! -x "$QEMU_BINARY" ]]; then
    echo -e "${RED}Error: QEMU binary not found: $QEMU_BINARY${NC}"
    echo ""
    echo "Please build QEMU first:"
    echo "  cd $TOPDIR/platform/qemu"
    echo "  ./setup.sh"
    echo ""
    echo "Or specify a different QEMU:"
    echo "  $0 /path/to/qemu-system-x86_64"
    exit 1
fi

echo -e "${GREEN}PhantomFPGA QTest Unit Tests${NC}"
echo "=============================="
echo ""
echo "QEMU: $QEMU_BINARY"
echo ""

# Check for PhantomFPGA device
echo -n "Checking for PhantomFPGA device... "
if $QEMU_BINARY -device help 2>&1 | grep -q phantomfpga; then
    echo -e "${GREEN}found${NC}"
else
    echo -e "${RED}not found${NC}"
    echo ""
    echo "The QEMU binary does not have the PhantomFPGA device."
    echo "Make sure you built QEMU with the PhantomFPGA patches applied."
    exit 1
fi
echo ""

# List available tests
TESTS=(
    "/phantomfpga/dev_id:DEV_ID register returns 0xF00DFACE"
    "/phantomfpga/dev_ver:DEV_VER register is readable"
    "/phantomfpga/dev_id_readonly:DEV_ID register is read-only"
    "/phantomfpga/pci_ids:PCI vendor/device IDs match"
    "/phantomfpga/ctrl_write_read:CTRL register write/read"
    "/phantomfpga/ctrl_reset:Reset via CTRL is self-clearing"
    "/phantomfpga/frame_size:FRAME_SIZE register"
    "/phantomfpga/frame_size_clamping:FRAME_SIZE value clamping"
    "/phantomfpga/frame_rate:FRAME_RATE register"
    "/phantomfpga/frame_rate_clamping:FRAME_RATE value clamping"
    "/phantomfpga/ring_size:RING_SIZE power-of-2 rounding"
    "/phantomfpga/ring_size_clamping:RING_SIZE value clamping"
    "/phantomfpga/watermark:WATERMARK register"
    "/phantomfpga/watermark_clamping:WATERMARK must be < ring_size"
    "/phantomfpga/dma_addr:DMA address (lo/hi) registers"
    "/phantomfpga/dma_size:DMA_SIZE register"
    "/phantomfpga/prod_idx:PROD_IDX is read-only"
    "/phantomfpga/cons_idx:CONS_IDX write/read"
    "/phantomfpga/cons_idx_wrapping:CONS_IDX modulo ring_size"
    "/phantomfpga/irq_mask:IRQ_MASK register"
    "/phantomfpga/irq_status_initial:IRQ_STATUS initial value is zero"
    "/phantomfpga/irq_status_w1c:IRQ_STATUS write-1-to-clear"
    "/phantomfpga/stats_initial:Statistics initially zero"
    "/phantomfpga/stats_readonly:Statistics are read-only"
    "/phantomfpga/fault_inject:FAULT_INJECT register"
    "/phantomfpga/status_initial:STATUS initial value"
    "/phantomfpga/status_running:STATUS shows RUNNING when started"
    "/phantomfpga/frame_production:Producer index increments when running"
    "/phantomfpga/no_frames_without_dma:No frames produced without DMA config"
    "/phantomfpga/full_reset:Full device reset clears all state"
    "/phantomfpga/reset_stops_device:Reset stops a running device"
)

if [[ -n "$LIST_TESTS" ]]; then
    echo "Available tests:"
    echo ""
    for test_entry in "${TESTS[@]}"; do
        name="${test_entry%%:*}"
        desc="${test_entry#*:}"
        printf "  %-40s %s\n" "$name" "$desc"
    done
    echo ""
    echo "Total: ${#TESTS[@]} tests"
    exit 0
fi

# Check if we can run actual QTest
QTEST_BINARY="$TOPDIR/platform/qemu/build/tests/qtest/phantomfpga-test"
if [[ -x "$QTEST_BINARY" ]]; then
    echo "Running QTest binary: $QTEST_BINARY"
    echo ""

    export QTEST_QEMU_BINARY="$QEMU_BINARY"

    if [[ -n "$SPECIFIC_TEST" ]]; then
        $QTEST_BINARY -p "$SPECIFIC_TEST"
    elif [[ -n "$VERBOSE" ]]; then
        $QTEST_BINARY --verbose
    else
        $QTEST_BINARY
    fi

    exit $?
fi

# Fall back to description mode
echo -e "${YELLOW}Note: Full QTest binary not found.${NC}"
echo ""
echo "To run actual tests, integrate with QEMU's test system:"
echo "  1. Copy phantomfpga-test.c to qemu/tests/qtest/"
echo "  2. Add to tests/qtest/meson.build"
echo "  3. Rebuild QEMU with tests"
echo ""
echo "Test descriptions:"
echo ""

for test_entry in "${TESTS[@]}"; do
    name="${test_entry%%:*}"
    desc="${test_entry#*:}"

    if [[ -n "$SPECIFIC_TEST" ]] && [[ "$name" != "$SPECIFIC_TEST" ]]; then
        continue
    fi

    echo -e "  ${GREEN}[INFO]${NC} $name"
    echo "         $desc"
    echo ""
done

echo ""
echo "Total: ${#TESTS[@]} test cases defined"
echo ""
echo "To run these tests for real:"
echo "  cd $QEMU_SRC"
echo "  cp $SCRIPT_DIR/phantomfpga-test.c tests/qtest/"
echo "  # Add to tests/qtest/meson.build: qtests_x86_64 += {'phantomfpga-test': []}"
echo "  meson setup build --reconfigure"
echo "  ninja -C build phantomfpga-test"
echo "  QTEST_QEMU_BINARY=$QEMU_BINARY build/tests/qtest/phantomfpga-test"
