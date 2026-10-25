# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  ptpclient
  SOURCES
  ptpclient.cpp
  ptpclient_base.cpp
  ptpclient_bridge.cpp
  ptpclient_linuxptp.cpp
  ptpclient_management.cpp
  ptpclient_phc.cpp
  ptpclient_phc_servo.cpp
  ptpclient_setup.cpp
  ptpclient_timer.cpp
  ptpclient_ntpshm.cpp
  DEPS
  statusbar::config
  statusbar::container
  statusbar::dsp
  statusbar::gptp
  statusbar::realtime
  statusbar::status
  statusbar::itc
  statusbar::ieee
  statusbar::net
  TESTS
  ptpclient_base_test.cpp
  ptpclient_bridge_test.cpp
  ptpclient_freq_ratio_test.cpp
  ptpclient_management_test.cpp
  ptpclient_media_clock_test.cpp
  ptpclient_phc_servo_test.cpp
  ptpclient_phc_test.cpp
  ptpclient_tai_translator_test.cpp
  ptpclient_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ptpclient_wake_example ptpclient_wake_example.cpp)
  target_link_libraries(ptpclient_wake_example PRIVATE statusbar)
  set_target_properties(ptpclient_wake_example
                        PROPERTIES OUTPUT_NAME statusbar-ptpclient-wake)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      ptpclient_wake_example)
endif()

add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/ptpclient_management_fuzzer.cpp)
