# SPDX-License-Identifier: MIT
# PhantomFPGA Viewer - Terminal Client (C++17)
#
# Simple build - no cross-compilation needed.
# This runs on the host machine, not in the VM.

CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS =

TARGET = phantomfpga_view
SRCS = phantomfpga_view.cpp phantomfpga_view_impl.cpp
OBJS = $(SRCS:.cpp=.o)
HDRS = $(wildcard *.h)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

# For debugging
debug: CXXFLAGS += -g -O0
debug: clean $(TARGET)
