# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

set(_gptp_sources
    gptp_base.cpp
    gptp_config.cpp
    gptp_error.cpp
    gptp_link_delay.cpp
    gptp_md_pdelay_req.cpp
    gptp_messages.cpp
    gptp_port_state_sm.cpp
    gptp_servo.cpp
    gptp_slave_port.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND _gptp_sources gptp_clock_ops_linux.cpp gptp_linux_socket.cpp
       gptp_ntpshm.cpp gptp_slave_session.cpp)
endif()

statusbar_add_module(
  NAME
  gptp
  SOURCES
  ${_gptp_sources}
  DEPS
  statusbar::buffer
  statusbar::ieee
  statusbar::itc
  statusbar::sm
  statusbar::status
  statusbar::tsn
  TESTS
  gptp_servo_test.cpp
  gptp_slave_port_test.cpp
  gptp_slave_session_test.cpp
  gptp_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(gptp_read_ntpshm_tool gptp_read_ntpshm_tool.cpp)
  target_link_libraries(gptp_read_ntpshm_tool PRIVATE statusbar)
  set_target_properties(gptp_read_ntpshm_tool PROPERTIES OUTPUT_NAME
                                                         statusbar-gptp-ntpshm)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      gptp_read_ntpshm_tool)

  add_executable(gptp_slave_linux_tool gptp_slave_linux_tool.cpp)
  target_link_libraries(gptp_slave_linux_tool PRIVATE statusbar)
  set_target_properties(gptp_slave_linux_tool PROPERTIES OUTPUT_NAME
                                                         statusbar-gptp-slave)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      gptp_slave_linux_tool)
endif()

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults ON for Clang).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/gptp_message_fuzzer.cpp)

add_executable(gptp_sm_tool gptp_sm_tool.cpp)
# Register for the docs-sm target (statusbar_register_sm_doc in
# cmake/module.cmake); the docs directory is package-relative.
statusbar_register_sm_doc(gptp_sm_tool ${PROJECT_SOURCE_DIR}/docs/sm/gptp)
target_link_libraries(gptp_sm_tool PRIVATE statusbar)
set_target_properties(gptp_sm_tool PROPERTIES OUTPUT_NAME statusbar-gptp-sm)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS gptp_sm_tool)
