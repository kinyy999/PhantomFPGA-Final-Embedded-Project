# QEMU QTest build integration for PhantomFPGA
#
# This file integrates PhantomFPGA tests into QEMU's test infrastructure.
# To use: copy or symlink to qemu/tests/qtest/phantomfpga-test.c
# and add the entry below to qemu/tests/qtest/meson.build
#
# Add to qtests_i386 or qtests_x86_64 in tests/qtest/meson.build:
#   'phantomfpga-test': [],
#
# Then build QEMU with tests enabled:
#   ../configure --enable-qtest
#   make check-qtest-x86_64
#
# SPDX-License-Identifier: GPL-2.0-or-later

# When built as part of QEMU:
#
# The test requires libqos for PCI device testing. Add to the appropriate
# architecture test list in the main qemu/tests/qtest/meson.build:
#
# Example entry for qtests_i386 or qtests_x86_64:
#
#   if have_device('phantomfpga')
#     qtests_x86_64 += 'phantomfpga-test'
#   endif
#
# Or unconditionally:
#
#   qtests_x86_64 += {
#     'phantomfpga-test': [],
#   }
#
# The test uses libqos and libqtest, which are linked automatically
# for tests in the qtests_* lists.

# Test source files
phantomfpga_test_srcs = files('phantomfpga-test.c')

# Dependencies (when building standalone or within QEMU)
# glib is required for g_test_* functions
# libqtest and libqos are QEMU internal libraries

# To run standalone (outside QEMU build system), use the Makefile instead.
