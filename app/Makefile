# SPDX-License-Identifier: MIT
# PhantomFPGA Userspace Application (C++17)
#
# Usage:
#   make          - Build (auto-detects Buildroot cross-compiler)
#   make clean    - Clean build artifacts

APP_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(realpath $(APP_DIR)/..)
BR_OUTPUT := $(PROJECT_ROOT)/platform/buildroot/output

# Auto-detect Buildroot cross-compiler
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else ifeq ($(HOST_ARCH),arm64)
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else
  CROSS_PREFIX := x86_64-buildroot-linux-gnu-
endif

TARGET_ARCH := $(if $(filter aarch64 arm64,$(HOST_ARCH)),aarch64,x86_64)
BR_CXX := $(BR_OUTPUT)/$(TARGET_ARCH)/host/bin/$(CROSS_PREFIX)g++

# Use Buildroot compiler if available, otherwise host g++
CXX := $(if $(wildcard $(BR_CXX)),$(BR_CXX),g++)

CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I$(APP_DIR)/../driver
LDFLAGS := -lpthread -lrt

TARGET := phantomfpga_app
SRCS := phantomfpga_app.cpp phantomfpga_app_impl.cpp
OBJS := $(SRCS:.cpp=.o)
HDRS := $(wildcard *.h) $(wildcard $(APP_DIR)/../driver/*.h)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
