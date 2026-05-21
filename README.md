# PhantomFPGA Final Embedded Project — Developer Submission

## Overview

This repository contains my developer-side implementation for the **PhantomFPGA Final Embedded Systems Project**.

PhantomFPGA is a simulated PCIe FPGA device running inside QEMU. The project demonstrates a complete embedded software stack:

- a Linux kernel driver for a PCIe FPGA device
- a userspace application running inside the QEMU VM
- TCP streaming from the VM to the host
- a host-side viewer
- integration and unit-test support

This repository intentionally contains only the developer submission files:

- `driver/`
- `app/`
- `viewer/`
- `tests/`
- `README.md`

The original course framework, QEMU platform, Buildroot output, kernel images, root filesystem images, compiled binaries, and generated build artifacts are intentionally not included.

---

## Repository Structure

```text
.
├── app/        # Userspace frame receiver and TCP streamer
├── driver/     # Linux kernel driver for the PhantomFPGA PCIe device
├── tests/      # Integration and unit-test support
└── viewer/     # Host-side TCP frame viewer
```

### `driver/`

The `driver/` directory contains the Linux kernel module for the PhantomFPGA PCIe device.

Main files:

```text
driver/
├── Kbuild
├── Makefile
├── phantomfpga_drv.c
├── phantomfpga_regs.h
└── phantomfpga_uapi.h
```

The driver is responsible for:

- probing the PhantomFPGA PCIe device
- mapping the device BAR registers
- creating the `/dev/phantomfpga0` character device
- handling ioctl control from userspace
- allocating and initializing descriptor buffers
- supporting normal read-based frame consumption
- supporting `poll()` notification
- supporting `mmap()` zero-copy access
- reporting device statistics
- handling device reset and cleanup
- keeping repeated streaming sessions stable

---

### `app/`

The `app/` directory contains the userspace application that runs inside the QEMU guest VM.

Main files:

```text
app/
├── Makefile
├── phantomfpga_app.cpp
├── phantomfpga_app.h
└── phantomfpga_app_impl.cpp
```

The app is responsible for:

- opening `/dev/phantomfpga0`
- configuring the device through ioctl calls
- starting and stopping streaming
- receiving frames from the driver
- validating frame magic values
- validating sequence numbers
- validating CRC32
- streaming frames over TCP port `5000`
- supporting normal read mode
- supporting zero-copy `mmap()` mode
- printing app, network, and device statistics

---

### `viewer/`

The `viewer/` directory contains the host-side TCP viewer.

Main files:

```text
viewer/
├── Makefile
├── phantomfpga_view.cpp
├── phantomfpga_view.h
└── phantomfpga_view_impl.cpp
```

The viewer is responsible for:

- connecting to the app TCP server
- receiving framed TCP data
- validating frame magic and CRC32
- detecting dropped frames through sequence numbers
- displaying ASCII frame payloads in the terminal
- printing viewer statistics
- recording raw frames to disk using `--record`

---

### `tests/`

The `tests/` directory contains integration and unit-test support.

Main files:

```text
tests/
├── integration/
│   ├── run_all.sh
│   ├── test_driver.sh
│   ├── test_faults.sh
│   └── test_streaming.sh
└── unit/
    ├── Makefile
    ├── meson.build
    ├── phantomfpga-test.c
    └── run-qtest.sh
```

The tests verify:

- driver load and unload behavior
- PCI device visibility
- device node creation
- ioctl access
- streaming correctness
- frame sequence validation
- frame magic validation
- statistics behavior
- recovery after faults
- repeated sessions
- combined integration flow through `run_all.sh`
- standalone unit-test build support

---

## System Architecture

The implemented system follows this flow:

```text
QEMU PhantomFPGA PCIe Device
        |
        | PCI BAR registers, DMA descriptors, status registers
        v
Linux Kernel Driver
        |
        | /dev/phantomfpga0
        | ioctl / read / poll / mmap
        v
Userspace App inside VM
        |
        | TCP port 5000
        | 4-byte length prefix + 5120-byte frame
        v
Host Viewer
        |
        | validation / display / optional recording
        v
User
```

The device produces frames.  
The driver exposes those frames through a Linux character device.  
The app consumes and validates the frames, then streams them over TCP.  
The viewer receives, validates, displays, and optionally records the frames.

---

## Frame Format

Each PhantomFPGA frame is exactly **5120 bytes**.

```text
Offset 0      : Frame header
Offset 16     : ASCII frame payload
Offset 5116   : CRC32
Total size    : 5120 bytes
```

The TCP protocol between the app and the viewer is:

```text
4-byte frame length in network byte order
+
5120-byte raw PhantomFPGA frame
```

The viewer first reads the 4-byte length, converts it using `ntohl()`, verifies that the length equals `5120`, and then reads the full frame.

---

## Driver Implementation Details

The driver implements the kernel-side control and data path for the PhantomFPGA PCIe device.

Implemented driver areas include:

- PCI probe and remove flow
- BAR0 mapping
- character-device registration
- `/dev/phantomfpga0` file operations
- ioctl configuration path
- descriptor-ring configuration
- DMA descriptor initialization
- frame consumption path
- statistics reporting
- `poll()` support
- `mmap()` zero-copy support
- reset and cleanup handling

### Repeated-Session Reset Fix

A key lifecycle fix was implemented in the driver.

When userspace applies a new configuration through the `SET_CFG` ioctl path, the driver performs a soft reset before reprogramming the device configuration and descriptor ring.

This prevents stale state from previous application runs from affecting new sessions.

The reset clears stale state such as:

- descriptor tail/head state
- frame sequence state
- pending interrupt status
- stale statistics
- previous streaming state

This made repeated application runs stable without requiring a manual `pf-reload` between every app execution.

---

## App Implementation Details

The app is the bridge between the kernel driver and external TCP clients.

High-level app flow:

```text
open /dev/phantomfpga0
configure device
start streaming
receive frames
validate frames
send valid frames over TCP
print statistics on exit
```

Implemented and verified behavior:

- normal read-based frame reception
- zero-copy `mmap()` frame reception
- magic validation
- sequence validation
- CRC32 validation
- TCP server mode
- device statistics printing
- clean shutdown on `Ctrl+C`

The app listens on TCP port `5000` and sends frames using:

```text
uint32_t frame_length in network byte order
raw 5120-byte frame
```

---

## Viewer Implementation Details

The viewer runs on the host and connects to the app through TCP.

The viewer implementation completes all required viewer TODO methods:

- `receive_frame()`
- `validate_frame()`
- `check_sequence()`
- `display_frame()`
- `frame_delay()`
- `print_stats()`
- record-mode integration

### `receive_frame()`

Receives one full frame from the TCP stream.

Logic:

1. read the 4-byte length prefix
2. convert the length from network byte order using `ntohl()`
3. verify the length equals `frame::SIZE`
4. read exactly `5120` bytes into the viewer frame buffer
5. if recording is enabled, write the raw frame to the record file

### `validate_frame()`

Validates the frame contents.

Checks:

- frame magic equals the expected PhantomFPGA magic value
- computed CRC32 matches the CRC stored at the end of the frame

Validation errors update:

```text
stats_.magic_errors
stats_.crc_errors
```

### `check_sequence()`

Checks sequence continuity.

If the current sequence is not the expected next sequence, the viewer calculates how many frames were dropped and updates:

```text
stats_.frames_dropped
```

The calculation handles wraparound using `frame::COUNT`.

### `display_frame()`

Displays the ASCII frame payload in the terminal.

The visible payload begins at `frame::DATA_OFFSET` and already contains newline characters, so the viewer writes the payload directly to `stdout`.

### `frame_delay()`

Uses `nanosleep()` to maintain the expected viewer display rate.

### `print_stats()`

Prints final viewer statistics to `stderr` so it does not interfere with the live ASCII frame display printed to `stdout`.

### Record Mode

The viewer supports raw frame recording:

```bash
./viewer/phantomfpga_view 127.0.0.1 5000 --record /tmp/viewer_record.bin
```

Recording format:

```text
raw 5120-byte frames, back-to-back
```

No extra headers or metadata are added.

Validation rule:

```text
record_file_size = frames_received × 5120
```

---

## Build Instructions

This submission is intended to be used inside the original PhantomFPGA course environment.

### Build the driver

```bash
cd driver
make clean && make
```

### Build the app

```bash
cd app
make clean && make
```

### Build the viewer

```bash
cd viewer
make clean && make
```

---

## Running the System

### Inside the QEMU VM

Load the driver and start the app:

```sh
pf-reload
/mnt/app/phantomfpga_app
```

### On the host

Run the viewer:

```bash
./viewer/phantomfpga_view 127.0.0.1 5000
```

Run the viewer with recording enabled:

```bash
./viewer/phantomfpga_view 127.0.0.1 5000 --record /tmp/viewer_record.bin
```

---

## Testing

### Integration Tests

The integration tests are intended to run inside the QEMU VM.

Run all integration tests:

```sh
/mnt/share/integration-tests/run_all.sh
```

Run quick mode:

```sh
/mnt/share/integration-tests/run_all.sh --quick
```

Run individual tests:

```sh
/mnt/share/integration-tests/test_driver.sh
/mnt/share/integration-tests/test_streaming.sh
/mnt/share/integration-tests/test_faults.sh
```

### Unit-Test Support

The unit-test folder contains QTest-oriented support and a standalone unit-test build path.

```bash
cd tests/unit
make clean && make test
```

The standalone target verifies that the test wrapper builds and lists the intended QTest cases.

---

## Verified Results

The project was verified with the following checks:

- driver build passed
- app build passed
- viewer build passed
- TODO/stub scan returned empty
- integration runner quick mode passed
- integration runner full mode passed
- driver integration test passed
- streaming integration test passed
- fault/recovery integration test passed
- zero-copy mode passed
- host-to-VM TCP forwarding passed
- viewer normal mode passed
- viewer record mode passed

### Integration Runner

The full integration runner completed successfully:

```text
test_driver     PASS
test_streaming  PASS
test_faults     PASS
OVERALL         PASSED
```

### Zero-Copy Verification

Zero-copy mode was verified with:

```sh
/mnt/app/phantomfpga_app --zero-copy -V
```

Result:

```text
Frames valid    = Frames received
Sequence errors = 0
Magic errors    = 0
CRC errors      = 0
```

### TCP Forwarding Verification

QEMU forwarded host port `5000` to VM port `5000`.

The app reported a host-side viewer connection from:

```text
10.0.2.2
```

The app statistics confirmed:

```text
Frames sent > 0
Bytes sent  > 0
Send errors = 0
```

### Viewer Normal Mode Verification

The viewer was run from the host:

```bash
./viewer/phantomfpga_view 127.0.0.1 5000
```

Verified result:

```text
Frames received > 0
Frames dropped  = 0
CRC errors      = 0
Magic errors    = 0
```

### Viewer Record Mode Verification

The viewer was run with recording enabled:

```bash
./viewer/phantomfpga_view 127.0.0.1 5000 --record /tmp/viewer_record.bin
```

Verified result:

```text
Frames received : 157
Frames dropped  : 0
CRC errors      : 0
Magic errors    : 0
```

Record file validation:

```text
157 × 5120 = 803840 bytes
```

The recorded file size was exactly:

```text
803840 bytes
```

---

## Final Implementation Status

```text
Driver implementation:      complete
App implementation:         complete
Viewer implementation:      complete
Integration tests:          complete
Standalone unit target:     builds/runs
Normal streaming:           verified
Zero-copy streaming:        verified
TCP forwarding:             verified
Viewer display:             verified
Viewer recording:           verified
TODO/stub scan:             clean
```

---

## Excluded From This Repository

The following are intentionally excluded:

- full course framework
- QEMU source/build tree
- Buildroot output
- kernel images
- root filesystem images
- generated binaries
- object files
- kernel module binaries
- generated kernel module metadata
- temporary test artifacts

This keeps the repository focused only on the developer implementation and tests.
