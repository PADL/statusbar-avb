# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  avtp
  SOURCES
  avtp_aaf.cpp
  avtp_aaf_v1.cpp
  avtp_aaf_v1_stream_input.cpp
  avtp_aaf_v1_stream_output.cpp
  avtp_aaf_stream_input.cpp
  avtp_aaf_stream_output.cpp
  avtp_acf.cpp
  avtp_acf_checksum.cpp
  avtp_acf_crc.cpp
  avtp_aef.cpp
  avtp_am824.cpp
  avtp_am824_v1.cpp
  avtp_am824_stream.cpp
  avtp_am824_stream_input.cpp
  avtp_am824_stream_output.cpp
  avtp_stream_common.cpp
  avtp_crf.cpp
  avtp_crf_v1.cpp
  avtp_crf_stream_input.cpp
  avtp_crf_stream_output.cpp
  avtp_eecf.cpp
  avtp_escf.cpp
  avtp_maap.cpp
  avtp_ntscf.cpp
  avtp_ntscf_v1.cpp
  avtp_tscf.cpp
  avtp_tscf_v1.cpp
  avtp_maap_sm.cpp
  avtp_maap_handler.cpp
  avtp_types.cpp
  DEPS
  statusbar::buffer
  statusbar::checksum
  statusbar::ieee
  statusbar::ip
  statusbar::sm
  statusbar::status
  statusbar::tsn
  TESTS
  avtp_aaf_test.cpp
  avtp_aaf_stream_input_test.cpp
  avtp_aaf_stream_output_test.cpp
  avtp_aaf_v1_test.cpp
  avtp_aaf_v1_stream_input_test.cpp
  avtp_aaf_v1_stream_output_test.cpp
  avtp_acf_test.cpp
  avtp_acf_checksum_test.cpp
  avtp_acf_crc_test.cpp
  avtp_acf_flexray_test.cpp
  avtp_acf_can_test.cpp
  avtp_acf_can_v2_test.cpp
  avtp_acf_can_brief_test.cpp
  avtp_acf_can_brief_v2_test.cpp
  avtp_acf_lin_test.cpp
  avtp_acf_lin_v2_test.cpp
  avtp_acf_most_test.cpp
  avtp_acf_can_xl_test.cpp
  avtp_acf_can_xl_brief_test.cpp
  avtp_acf_gpc_test.cpp
  avtp_acf_serial_test.cpp
  avtp_acf_parallel_test.cpp
  avtp_acf_sensor_test.cpp
  avtp_acf_sensor_brief_test.cpp
  avtp_acf_aecp_test.cpp
  avtp_acf_ancillary_test.cpp
  avtp_aef_test.cpp
  avtp_am824_test.cpp
  avtp_am824_v1_test.cpp
  avtp_am824_stream_test.cpp
  avtp_am824_stream_output_test.cpp
  avtp_crf_test.cpp
  avtp_crf_stream_test.cpp
  avtp_crf_v1_test.cpp
  avtp_eecf_test.cpp
  avtp_escf_test.cpp
  avtp_ip_encap_test.cpp
  avtp_maap_test.cpp
  avtp_ntscf_test.cpp
  avtp_ntscf_v1_test.cpp
  avtp_tscf_test.cpp
  avtp_tscf_v1_test.cpp
  avtp_maap_sm_test.cpp
  avtp_maap_handler_test.cpp
  avtp_stream_format_test.cpp
  avtp_types_test.cpp)

add_executable(avtp_retransmit_tool avtp_retransmit_tool.cpp)
target_link_libraries(avtp_retransmit_tool PRIVATE statusbar)
set_target_properties(avtp_retransmit_tool PROPERTIES OUTPUT_NAME
                                                      statusbar-avtp-retransmit)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    avtp_retransmit_tool)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults ON for Clang).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_aaf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_aaf_v1_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_checksum_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_crc_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_flexray_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_v2_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_brief_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_brief_v2_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_lin_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_lin_v2_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_most_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_xl_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_can_xl_brief_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_gpc_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_serial_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_parallel_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_sensor_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_sensor_brief_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_aecp_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_acf_ancillary_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_am824_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_am824_v1_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_crf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_crf_v1_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_eecf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_escf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_ntscf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_ntscf_v1_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_tscf_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_tscf_v1_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_ip_encap_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_aef_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/avtp_maap_fuzzer.cpp)

add_executable(avtp_sm_tool avtp_sm_tool.cpp)
# Register for the docs-sm target (statusbar_register_sm_doc in
# cmake/module.cmake); the docs directory is package-relative.
statusbar_register_sm_doc(avtp_sm_tool ${PROJECT_SOURCE_DIR}/docs/sm/avtp)
target_link_libraries(avtp_sm_tool PRIVATE statusbar)
set_target_properties(avtp_sm_tool PROPERTIES OUTPUT_NAME statusbar-avtp-sm)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS avtp_sm_tool)
