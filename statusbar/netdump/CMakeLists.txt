# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  netdump
  INTERFACE
  DEPS
  statusbar::atdecc
  statusbar::avtp
  statusbar::gptp
  statusbar::ieee
  statusbar::ip
  statusbar::srp
  statusbar::tsn
  TESTS
  netdump_print_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults OFF).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/netdump_format_fuzzer.cpp)
