#!/bin/sh
# SPDX-License-Identifier: MIT
#
# PhantomFPGA Integration Test Runner
#
# Runs all integration tests and reports results.
# Suitable for local VM runs and CI/CD pipelines.

set -u

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JUNIT_OUTPUT=""
STOP_ON_FAIL=false
QUICK_MODE=false
VERBOSE=false

TEST_SUITES="test_driver.sh test_streaming.sh test_faults.sh"

DRIVER_RESULT="SKIP"
STREAMING_RESULT="SKIP"
FAULTS_RESULT="SKIP"

TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_SKIPPED=0

START_TIME=0
END_TIME=0

if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    BOLD=''
    NC=''
fi

# --------------------------------------------------------------------------
# Helper Functions
# --------------------------------------------------------------------------

usage() {
    cat << EOF
PhantomFPGA Integration Test Runner

Usage: $(basename "$0") [OPTIONS]

Options:
    --quick          Run tests in quick mode
    --verbose        Show full output from test suites
    --stop-on-fail   Stop at first failing test suite
    --junit FILE     Generate JUnit XML report
    --help           Show this help

Exit codes:
    0   All tests passed
    1   Some tests failed
    2   Test infrastructure error

Examples:
    $(basename "$0")
    $(basename "$0") --quick
    $(basename "$0") --junit report.xml
EOF
}

cecho() {
    # Usage: cecho "$COLOR" "message"
    printf '%b%s%b\n' "$1" "$2" "$NC"
}

log_header() {
    echo ""
    cecho "$BOLD" "========================================"
    cecho "$BOLD" "$*"
    cecho "$BOLD" "========================================"
    echo ""
}

log_info() {
    cecho "$BLUE" "[INFO] $*"
}

log_pass() {
    cecho "$GREEN" "[PASS] $*"
}

log_fail() {
    cecho "$RED" "[FAIL] $*"
}

log_warn() {
    cecho "$YELLOW" "[WARN] $*"
}

timestamp() {
    date +%s
}

format_duration() {
    seconds=$1
    minutes=$((seconds / 60))
    secs=$((seconds % 60))

    if [ "$minutes" -gt 0 ]; then
        echo "${minutes}m ${secs}s"
    else
        echo "${secs}s"
    fi
}

set_suite_result() {
    suite_name=$1
    result=$2

    case "$suite_name" in
        test_driver)
            DRIVER_RESULT="$result"
            ;;
        test_streaming)
            STREAMING_RESULT="$result"
            ;;
        test_faults)
            FAULTS_RESULT="$result"
            ;;
    esac
}

get_suite_result() {
    suite_name=$1

    case "$suite_name" in
        test_driver)
            echo "$DRIVER_RESULT"
            ;;
        test_streaming)
            echo "$STREAMING_RESULT"
            ;;
        test_faults)
            echo "$FAULTS_RESULT"
            ;;
        *)
            echo "SKIP"
            ;;
    esac
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_warn "Not running as root. Some tests may fail."
        log_info "Inside the PhantomFPGA VM, run this script as root."
        return 1
    fi

    return 0
}

check_environment() {
    log_info "Checking test environment..."
    log_info "Kernel: $(uname -r)"

    if [ -f "/mnt/driver/phantomfpga.ko" ]; then
        log_info "Driver module: found at /mnt/driver/phantomfpga.ko"
    elif [ -f "/lib/modules/$(uname -r)/extra/phantomfpga.ko" ] ||
         modinfo phantomfpga >/dev/null 2>&1; then
        log_info "Driver module: found in installed module path"
    else
        log_warn "Driver module: not found"
        log_warn "Expected /mnt/driver/phantomfpga.ko in the local VM workflow"
    fi

    if command -v phantomfpga_app >/dev/null 2>&1; then
        log_info "App binary: found in PATH"
    elif [ -x "/mnt/app/phantomfpga_app" ]; then
        log_info "App binary: found at /mnt/app/phantomfpga_app"
    elif [ -x "/usr/local/bin/phantomfpga_app" ]; then
        log_info "App binary: found at /usr/local/bin/phantomfpga_app"
    else
        log_warn "App binary: not found"
        log_warn "Expected /mnt/app/phantomfpga_app in the local VM workflow"
    fi

    if command -v lspci >/dev/null 2>&1; then
        if lspci -nn 2>/dev/null | grep -q "0dad:f00d"; then
            log_info "PCI device: found"
        else
            log_warn "PCI device: not found"
        fi
    fi

    return 0
}

run_suite() {
    suite=$1
    suite_path="${SCRIPT_DIR}/${suite}"
    suite_name="${suite%.sh}"
    suite_args=""
    suite_start=0
    suite_end=0
    exit_code=0

    if [ ! -x "$suite_path" ]; then
        log_warn "Test suite not found or not executable: $suite_path"
        set_suite_result "$suite_name" "SKIP"
        TOTAL_SKIPPED=$((TOTAL_SKIPPED + 1))
        return 0
    fi

    if $QUICK_MODE && [ "$suite" != "test_driver.sh" ]; then
        suite_args="--quick"
    fi

    log_header "Running: $suite_name"

    suite_start=$(timestamp)

    if $VERBOSE; then
        "$suite_path" $suite_args
        exit_code=$?
    else
        output=$("$suite_path" $suite_args 2>&1)
        exit_code=$?

        echo "$output" | grep -E "^\[(PASS|FAIL|WARN|INFO|TEST)\]|^Tests|^---" || true
    fi

    suite_end=$(timestamp)
    suite_duration=$((suite_end - suite_start))

    if [ "$exit_code" -eq 0 ]; then
        set_suite_result "$suite_name" "PASS"
        TOTAL_PASSED=$((TOTAL_PASSED + 1))
        log_pass "$suite_name completed in $(format_duration "$suite_duration")"
    else
        set_suite_result "$suite_name" "FAIL"
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        log_fail "$suite_name failed with exit code $exit_code"

        if $VERBOSE; then
            log_info "Recent dmesg:"
            dmesg | tail -20
        fi

        if $STOP_ON_FAIL; then
            log_fail "Stopping due to --stop-on-fail"
            return 1
        fi
    fi

    return 0
}

generate_junit_report() {
    output_file=$1
    total=$((TOTAL_PASSED + TOTAL_FAILED + TOTAL_SKIPPED))
    duration=$((END_TIME - START_TIME))

    log_info "Generating JUnit report: $output_file"

    cat > "$output_file" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="PhantomFPGA Integration Tests" tests="$total" failures="$TOTAL_FAILED" skipped="$TOTAL_SKIPPED" time="$duration">
EOF

    for suite in $TEST_SUITES; do
        suite_name="${suite%.sh}"
        result=$(get_suite_result "$suite_name")
        failure=""
        failures=0
        skipped=0

        if [ "$result" = "FAIL" ]; then
            failure='<failure message="Test suite failed">See test output for details</failure>'
            failures=1
        elif [ "$result" = "SKIP" ]; then
            failure='<skipped message="Test suite skipped"/>'
            skipped=1
        fi

        cat >> "$output_file" << EOF
  <testsuite name="$suite_name" tests="1" failures="$failures" skipped="$skipped">
    <testcase name="$suite_name" classname="integration">
      $failure
    </testcase>
  </testsuite>
EOF
    done

    cat >> "$output_file" << EOF
</testsuites>
EOF
}

print_summary() {
    total=$((TOTAL_PASSED + TOTAL_FAILED + TOTAL_SKIPPED))
    duration=$((END_TIME - START_TIME))

    log_header "Test Summary"

    echo "Test suites:"
    for suite in $TEST_SUITES; do
        suite_name="${suite%.sh}"
        result=$(get_suite_result "$suite_name")

        case "$result" in
            PASS)
                cecho "$GREEN" "  [PASS] $suite_name"
                ;;
            FAIL)
                cecho "$RED" "  [FAIL] $suite_name"
                ;;
            SKIP)
                cecho "$YELLOW" "  [SKIP] $suite_name"
                ;;
        esac
    done

    echo ""
    echo "Results:"
    echo "  Total:   $total suites"
    cecho "$GREEN" "  Passed:  $TOTAL_PASSED"
    cecho "$RED" "  Failed:  $TOTAL_FAILED"
    cecho "$YELLOW" "  Skipped: $TOTAL_SKIPPED"
    echo ""
    echo "Duration: $(format_duration "$duration")"
    echo ""

    if [ "$TOTAL_FAILED" -gt 0 ]; then
        cecho "$RED$BOLD" "OVERALL: FAILED"
        return 1
    elif [ "$TOTAL_PASSED" -eq 0 ]; then
        cecho "$YELLOW$BOLD" "OVERALL: NO TESTS RAN"
        return 2
    else
        cecho "$GREEN$BOLD" "OVERALL: PASSED"
        return 0
    fi
}

cleanup() {
    if lsmod | grep -q "^phantomfpga "; then
        log_info "Leaving driver loaded for inspection"
    fi
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

main() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --quick)
                QUICK_MODE=true
                ;;
            --verbose)
                VERBOSE=true
                ;;
            --stop-on-fail)
                STOP_ON_FAIL=true
                ;;
            --junit)
                shift
                if [ "$#" -eq 0 ]; then
                    echo "Error: --junit requires a file path"
                    exit 2
                fi
                JUNIT_OUTPUT="$1"
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                usage
                exit 2
                ;;
        esac
        shift
    done

    trap cleanup EXIT

    log_header "PhantomFPGA Integration Tests"

    echo "Configuration:"
    echo "  Quick mode:     $QUICK_MODE"
    echo "  Verbose:        $VERBOSE"
    echo "  Stop on fail:   $STOP_ON_FAIL"
    echo "  JUnit output:   ${JUNIT_OUTPUT:-none}"
    echo ""

    check_root || true
    check_environment || true

    START_TIME=$(timestamp)

    for suite in $TEST_SUITES; do
        if ! run_suite "$suite"; then
            break
        fi
    done

    END_TIME=$(timestamp)

    if [ -n "$JUNIT_OUTPUT" ]; then
        generate_junit_report "$JUNIT_OUTPUT"
    fi

    print_summary
    exit $?
}

main "$@"
