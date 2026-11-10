# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  stun
  SOURCES
  stun_auth.cpp
  stun_client.cpp
  stun_error.cpp
  stun_message.cpp
  stun_register.cpp
  stun_rendezvous.cpp
  stun_server.cpp
  DEPS
  statusbar::buffer
  statusbar::crypto
  statusbar::ieee
  statusbar::ip
  statusbar::net
  statusbar::secure_random
  statusbar::sm
  statusbar::status
  statusbar::itc
  TESTS
  stun_client_sm_test.cpp
  stun_loopback_test.cpp
  stun_message_test.cpp
  stun_register_test.cpp
  stun_rendezvous_test.cpp
  stun_server_sm_test.cpp)

# stun_server_tool / stun_client_tool / stun_sm_tool live in ../tools/ — the
# package's shared tools directory — not next to the library module.

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults OFF).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/stun_decode_fuzzer.cpp)
