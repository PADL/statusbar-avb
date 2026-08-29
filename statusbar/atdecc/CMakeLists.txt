# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  atdecc
  SOURCES
  atdecc_adp.cpp
  atdecc_adp_discovery.cpp
  atdecc_acmp.cpp
  atdecc_aecp.cpp
  atdecc_aecp_aem.cpp
  atdecc_aecp_aa.cpp
  atdecc_aecp_aa_analysis.cpp
  atdecc_aem_control_types.cpp
  atdecc_aem_control_values.cpp
  atdecc_aem_descriptor.cpp
  atdecc_aem_print.cpp
  atdecc_aem_units.cpp
  atdecc_acmp_listener_sm.cpp
  atdecc_acmp_talker_sm.cpp
  atdecc_acmp_controller_sm.cpp
  atdecc_aecp_aem_controller.cpp
  atdecc_error.cpp
  atdecc_jdks.cpp
  DEPS
  statusbar::avtp
  statusbar::buffer
  statusbar::container
  statusbar::ieee
  statusbar::ip
  statusbar::safe_arith
  statusbar::sm
  statusbar::status
  statusbar::tsn
  TESTS
  atdecc_acmp_sm_test.cpp
  atdecc_adp_test.cpp
  atdecc_aecp_test.cpp
  atdecc_aecp_aa_test.cpp
  atdecc_aem_control_accessors_test.cpp
  atdecc_aem_control_command_test.cpp
  atdecc_aem_control_types_test.cpp
  atdecc_aem_control_values_test.cpp
  atdecc_aem_print_test.cpp
  atdecc_aem_units_test.cpp
  atdecc_descriptor_storage_test.cpp
  atdecc_jdks_test.cpp
  atdecc_adp_discovery_test.cpp
  atdecc_aecp_aem_controller_test.cpp
  atdecc_test.cpp)

add_executable(acmp_controller_tool atdecc_acmp_controller_tool.cpp)
target_link_libraries(acmp_controller_tool PRIVATE statusbar)
set_target_properties(acmp_controller_tool PROPERTIES OUTPUT_NAME
                                                      statusbar-acmp-controller)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    acmp_controller_tool)

add_executable(aem_set_clock_source_tool atdecc_aem_set_clock_source_tool.cpp)
target_link_libraries(aem_set_clock_source_tool PRIVATE statusbar)
set_target_properties(aem_set_clock_source_tool
                      PROPERTIES OUTPUT_NAME statusbar-aem-set-clock-source)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    aem_set_clock_source_tool)

add_executable(aem_get_counters_tool atdecc_aem_get_counters_tool.cpp)
target_link_libraries(aem_get_counters_tool PRIVATE statusbar)
set_target_properties(aem_get_counters_tool
                      PROPERTIES OUTPUT_NAME statusbar-aem-get-counters)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    aem_get_counters_tool)

add_executable(aem_control_tool atdecc_aem_control_tool.cpp)
target_link_libraries(aem_control_tool PRIVATE statusbar)
set_target_properties(aem_control_tool PROPERTIES OUTPUT_NAME
                                                  statusbar-aem-control)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS aem_control_tool)

add_executable(descriptor_storage_tool atdecc_descriptor_storage_tool.cpp)
target_link_libraries(descriptor_storage_tool PRIVATE statusbar)
set_target_properties(descriptor_storage_tool
                      PROPERTIES OUTPUT_NAME statusbar-descriptor-storage)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    descriptor_storage_tool)

add_executable(atdecc_sm_tool atdecc_sm_tool.cpp)
target_link_libraries(atdecc_sm_tool PRIVATE statusbar)
set_target_properties(atdecc_sm_tool PROPERTIES OUTPUT_NAME statusbar-atdecc-sm)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS atdecc_sm_tool)

add_executable(aecp_aa_analyzer_tool atdecc_aecp_aa_analyzer_tool.cpp)
target_link_libraries(aecp_aa_analyzer_tool PRIVATE statusbar)
set_target_properties(aecp_aa_analyzer_tool
                      PROPERTIES OUTPUT_NAME statusbar-aecp-aa-analyzer)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    aecp_aa_analyzer_tool)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults ON for Clang).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_descriptor_storage_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_adp_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_acmp_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_aecp_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_aecp_aem_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_aecp_aa_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/atdecc_aem_descriptor_fuzzer.cpp)
