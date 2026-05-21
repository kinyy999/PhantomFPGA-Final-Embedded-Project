# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA Out-of-Tree Kernel Module Makefile
#
# Usage:
#   make                    - Build using Buildroot output (auto-detected)
#   make KDIR=/path/to/src  - Build against specific kernel
#   make clean              - Clean build artifacts
#   make help               - Show this help
#
# The Makefile auto-detects the Buildroot output directory and cross-compiler
# based on your host architecture. Just run "make" from the driver/ directory.

# Current directory (where our source is)
PWD := $(shell pwd)

# Module name
MODULE := phantomfpga

# Enable debug output if requested
ifdef DEBUG
ccflags-y += -DDEBUG -g
endif

# ---------------------------------------------------------------------------
# Auto-detect Buildroot paths
# ---------------------------------------------------------------------------
# If KDIR isn't set, try to find the Buildroot output for the host arch.
# This lets trainees just run "make" without worrying about paths.

DRIVER_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(realpath $(DRIVER_DIR)/..)
BR_OUTPUT := $(PROJECT_ROOT)/platform/buildroot/output

HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
  TARGET_ARCH := aarch64
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else ifeq ($(HOST_ARCH),arm64)
  TARGET_ARCH := aarch64
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else
  TARGET_ARCH := x86_64
  CROSS_PREFIX := x86_64-buildroot-linux-gnu-
endif

BR_ARCH_OUTPUT := $(BR_OUTPUT)/$(TARGET_ARCH)

# Find KDIR: use Buildroot kernel if available, fall back to running kernel
ifndef KDIR
  BR_KDIR := $(wildcard $(BR_ARCH_OUTPUT)/build/linux-[0-9]*)
  ifneq ($(BR_KDIR),)
    KDIR := $(BR_KDIR)
    # Also set CROSS_COMPILE when using Buildroot kernel
    CROSS_COMPILE ?= $(BR_ARCH_OUTPUT)/host/bin/$(CROSS_PREFIX)
    ARCH ?= $(if $(filter aarch64,$(TARGET_ARCH)),arm64,x86_64)
    export ARCH CROSS_COMPILE
  else
    KDIR := /lib/modules/$(shell uname -r)/build
  endif
endif

# Default target
all: modules

# Build the module
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f Module.symvers modules.order

# Install the module
install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Uninstall the module
uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/$(MODULE).ko
	depmod -a

# Load the module (for testing)
load: modules
	@echo "Loading $(MODULE) module..."
	insmod $(MODULE).ko

# Unload the module
unload:
	@echo "Unloading $(MODULE) module..."
	-rmmod $(MODULE)

# Reload the module
reload: unload load

# Show module info
info: modules
	modinfo $(MODULE).ko

# Check coding style (requires checkpatch.pl from kernel source)
checkpatch:
	@if [ -f $(KDIR)/scripts/checkpatch.pl ]; then \
		$(KDIR)/scripts/checkpatch.pl --no-tree -f *.c *.h; \
	else \
		echo "checkpatch.pl not found in $(KDIR)/scripts/"; \
	fi

# Help target
help:
	@echo "PhantomFPGA Driver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the kernel module (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  install    - Install module to system (requires root)"
	@echo "  uninstall  - Remove module from system (requires root)"
	@echo "  load       - Load module with insmod (requires root)"
	@echo "  unload     - Unload module with rmmod (requires root)"
	@echo "  reload     - Unload and reload module"
	@echo "  info       - Show module information"
	@echo "  checkpatch - Check code style"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR       - Kernel source/build directory"
	@echo "               Auto-detected: $(KDIR)"
	@echo "  DEBUG      - Enable debug build (DEBUG=1)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build (auto-detects Buildroot)"
	@echo "  make KDIR=/usr/src/linux-5.15 # Build for specific kernel"
	@echo "  make DEBUG=1                  # Build with debug symbols"

.PHONY: all modules clean install uninstall load unload reload info checkpatch help
