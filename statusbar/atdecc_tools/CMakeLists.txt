# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  atdecc_tools
  SOURCES
  atdecc_controller_tui.cpp
  atdecc_controller_simple.cpp
  atdecc_controller_service_rawnet.cpp
  atdecc_aem_validate.cpp
  DEPS
  statusbar::atdecc
  statusbar::avtp
  statusbar::buffer
  statusbar::config
  statusbar::ieee
  statusbar::nanoavb
  statusbar::net
  statusbar::status
  statusbar::toml
  statusbar::tui
  TESTS
  atdecc_controller_tui_test.cpp
  atdecc_controller_service_test.cpp
  atdecc_reactor_marshal_test.cpp
  atdecc_ctl_resolve_test.cpp
  atdecc_aem_validate_test.cpp)

# macOS backend over the system AVB framework (AudioVideoBridging). The
# Objective-C++ TU is confined here; the portable build never sees it.
if(APPLE)
  enable_language(OBJCXX)
  target_sources(statusbar-atdecc_tools
                 PRIVATE atdecc_controller_service_avb.mm)
  set_source_files_properties(atdecc_controller_service_avb.mm
                              PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(
    statusbar-atdecc_tools PROPERTIES OBJCXX_STANDARD 23
                                      OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(
    statusbar-atdecc_tools PUBLIC "-framework AudioVideoBridging"
                                  "-framework Foundation")
endif()

add_executable(atdecc_monitor_tool atdecc_monitor_tool.cpp)
target_link_libraries(atdecc_monitor_tool PRIVATE statusbar)
set_target_properties(atdecc_monitor_tool PROPERTIES OUTPUT_NAME
                                                     statusbar-atdecc-monitor)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    atdecc_monitor_tool)

add_executable(atdecc_controller_tool atdecc_controller_tool.cpp)
target_link_libraries(atdecc_controller_tool PRIVATE statusbar)
set_target_properties(atdecc_controller_tool
                      PROPERTIES OUTPUT_NAME statusbar-atdecc-controller)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    atdecc_controller_tool)

# Scriptable (non-interactive) controller: discovery, connect-by-name,
# set-clock-source, and TOML batch. Ships in the avb package.
add_executable(atdecc_ctl_tool atdecc_ctl_tool.cpp)
target_link_libraries(atdecc_ctl_tool PRIVATE statusbar)
set_target_properties(atdecc_ctl_tool PROPERTIES OUTPUT_NAME
                                                 statusbar-atdecc-ctl)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS atdecc_ctl_tool)
