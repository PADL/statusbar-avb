# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  owlm
  SOURCES
  owlm_error.cpp
  owlm_packet.cpp
  DEPS
  statusbar::buffer
  statusbar::gptp
  statusbar::ieee
  statusbar::net
  statusbar::stats
  statusbar::status
  statusbar::stun
  statusbar::udptun
  TESTS
  owlm_codec_test.cpp
  owlm_error_test.cpp
  owlm_eui64_test.cpp
  owlm_packet_test.cpp)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults OFF).
# OWLM measurement packets arrive over UDP from an untrusted source.
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/owlm_packet_fuzzer.cpp)

# owlm_tool (and its loopback smoke test) live in ../tools/ — the package's
# shared tools directory — not next to the library module.
