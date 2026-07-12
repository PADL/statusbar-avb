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
  atdecc_ctl_resolve_test.cpp
  atdecc_aem_validate_test.cpp)

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
