# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  srp
  SOURCES
  srp_mrp.cpp
  srp_mrp_applicant_sm.cpp
  srp_mrp_leaveall_sm.cpp
  srp_mrp_participant.cpp
  srp_mrp_periodic_sm.cpp
  srp_mrp_registrar_sm.cpp
  srp_mrp_timers.cpp
  srp_msrp.cpp
  srp_msrp_participant.cpp
  srp_mvrp.cpp
  DEPS
  statusbar::buffer
  statusbar::ieee
  statusbar::sm
  statusbar::status
  statusbar::tsn
  TESTS
  srp_mrp_applicant_sm_test.cpp
  srp_mrp_registrar_sm_test.cpp
  srp_mrp_test.cpp
  srp_msrp_participant_test.cpp
  srp_msrp_test.cpp
  srp_mvrp_test.cpp)

add_executable(srp_msrp_functional_test_tool srp_msrp_functional_test_tool.cpp)
target_link_libraries(srp_msrp_functional_test_tool PRIVATE statusbar)
set_target_properties(srp_msrp_functional_test_tool
                      PROPERTIES OUTPUT_NAME statusbar-msrp-functional-test)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    srp_msrp_functional_test_tool)

add_executable(srp_sm_tool srp_sm_tool.cpp)
# Register for the docs-sm target (statusbar_register_sm_doc in
# cmake/module.cmake); the docs directory is package-relative.
statusbar_register_sm_doc(srp_sm_tool ${PROJECT_SOURCE_DIR}/docs/sm/srp)
target_link_libraries(srp_sm_tool PRIVATE statusbar)
set_target_properties(srp_sm_tool PROPERTIES OUTPUT_NAME statusbar-srp-sm)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS srp_sm_tool)

# Fuzz harnesses (built only when -DENABLE_FUZZING=ON; defaults ON for Clang).
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/srp_msrp_fuzzer.cpp)
add_avb_fuzzer(${CMAKE_CURRENT_LIST_DIR}/srp_mvrp_fuzzer.cpp)
