# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  nanoavb
  SOURCES
  nanoavb_base.cpp
  nanoavb_acmp.cpp
  nanoavb_aem_blob_writer.cpp
  nanoavb_aem_fingerprint.cpp
  nanoavb_aem_model_store.cpp
  nanoavb_aem_symbols.cpp
  nanoavb_acmp_listener_sm.cpp
  nanoavb_acmp_talker_sm.cpp
  nanoavb_adp.cpp
  nanoavb_aem_entity_model.cpp
  nanoavb_entity.cpp
  nanoavb_entity_model.cpp
  nanoavb_gptp_sm.cpp
  nanoavb_msrp_listener_sm.cpp
  nanoavb_msrp_talker_sm.cpp
  nanoavb_mvrp_sm.cpp
  nanoavb_srp.cpp
  nanoavb_supervisor_sm.cpp
  nanoavb_components.cpp
  nanoavb_controller.cpp
  DEPS
  statusbar::atdecc
  statusbar::avtp
  statusbar::buffer
  statusbar::crypto
  statusbar::container
  statusbar::gptp
  statusbar::ieee
  statusbar::net
  statusbar::sm
  statusbar::srp
  statusbar::status
  statusbar::itc
  statusbar::tsn
  TESTS
  nanoavb_acmp_test.cpp
  nanoavb_adp_test.cpp
  nanoavb_aem_blob_writer_test.cpp
  nanoavb_aem_fingerprint_test.cpp
  nanoavb_aem_model_store_test.cpp
  nanoavb_aem_symbols_test.cpp
  nanoavb_aem_entity_model_test.cpp
  nanoavb_base_test.cpp
  nanoavb_entity_model_test.cpp
  nanoavb_entity_test.cpp
  nanoavb_gptp_sm_test.cpp
  nanoavb_msrp_sm_test.cpp
  nanoavb_sm_test.cpp
  nanoavb_srp_test.cpp
  nanoavb_controller_test.cpp
  nanoavb_integration_test.cpp)

add_executable(nanoavb_example nanoavb_example.cpp)
target_link_libraries(nanoavb_example PRIVATE statusbar)
set_target_properties(nanoavb_example PROPERTIES OUTPUT_NAME statusbar-nanoavb)

add_executable(nanoavb_supervisor_sm_tool nanoavb_supervisor_sm_tool.cpp)
target_link_libraries(nanoavb_supervisor_sm_tool PRIVATE statusbar)
set_target_properties(nanoavb_supervisor_sm_tool
                      PROPERTIES OUTPUT_NAME statusbar-nanoavb-sm)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS nanoavb_example
                                    nanoavb_supervisor_sm_tool)
