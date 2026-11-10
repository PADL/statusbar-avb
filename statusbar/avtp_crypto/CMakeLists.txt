# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  avtp_crypto
  SOURCES
  avtp_crypto.cpp
  avtp_keychain.cpp
  avtp_crypto_pdu.cpp
  avtp_aem_frame.cpp
  p256_wire.cpp
  DEPS
  statusbar::avtp
  statusbar::buffer
  statusbar::crypto
  TESTS
  avtp_aem_frame_test.cpp
  avtp_crypto_pdu_test.cpp
  avtp_crypto_test.cpp
  avtp_keychain_test.cpp
  p256_wire_test.cpp)

add_executable(avtp_crypto_tool ${CMAKE_CURRENT_LIST_DIR}/avtp_crypto_tool.cpp)
target_link_libraries(avtp_crypto_tool PRIVATE statusbar)
set_target_properties(avtp_crypto_tool PROPERTIES OUTPUT_NAME
                                                  statusbar-avtp-crypto-tool)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS avtp_crypto_tool)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults OFF).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_pdu_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_keychain_fuzzer.cpp)
