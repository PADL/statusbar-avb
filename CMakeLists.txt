cmake_minimum_required(VERSION 3.31)
project(
  statusbar-avb
  VERSION 1.4.1
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
enable_testing()

# Modules register bench tools into this aggregate target. The outer aggregate
# build supplies a single shared target instead.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  add_custom_target(run-all-benches)
endif()

# Dependency packages — must be installed and discoverable.
find_package(statusbar-audio REQUIRED)
find_package(statusbar-core REQUIRED)
find_package(statusbar-crypto REQUIRED)

add_subdirectory(statusbar)

# Export each module target under the short statusbar::<name> form, so dependent
# packages find_package() them as statusbar::<module>.
set_target_properties(statusbar-atdecc PROPERTIES EXPORT_NAME atdecc)
set_target_properties(statusbar-atdecc_tools PROPERTIES EXPORT_NAME
                                                        atdecc_tools)
set_target_properties(statusbar-avb_entity PROPERTIES EXPORT_NAME avb_entity)
set_target_properties(statusbar-avtp PROPERTIES EXPORT_NAME avtp)
set_target_properties(statusbar-avtp_crypto PROPERTIES EXPORT_NAME avtp_crypto)
set_target_properties(statusbar-avtp_tools PROPERTIES EXPORT_NAME avtp_tools)
set_target_properties(statusbar-gptp PROPERTIES EXPORT_NAME gptp)
set_target_properties(statusbar-nanoavb PROPERTIES EXPORT_NAME nanoavb)
set_target_properties(statusbar-ptpclient PROPERTIES EXPORT_NAME ptpclient)
set_target_properties(statusbar-srp PROPERTIES EXPORT_NAME srp)
# Networking and measurement modules.
set_target_properties(statusbar-netdump PROPERTIES EXPORT_NAME netdump)
set_target_properties(statusbar-owlm PROPERTIES EXPORT_NAME owlm)
set_target_properties(statusbar-stun PROPERTIES EXPORT_NAME stun)
set_target_properties(statusbar-udptun PROPERTIES EXPORT_NAME udptun)

# Give every installed executable a statusbar- prefixed on-disk name, so the
# bin/ directory is uniform (tools that set no OUTPUT_NAME, and examples, would
# otherwise install unprefixed).
get_property(_exe_targets GLOBAL PROPERTY STATUSBAR_INSTALL_TARGETS)
foreach(_t ${_exe_targets})
  if(NOT TARGET ${_t})
    continue()
  endif()
  get_target_property(_t_type ${_t} TYPE)
  if(NOT _t_type STREQUAL "EXECUTABLE")
    continue()
  endif()
  get_target_property(_t_name ${_t} OUTPUT_NAME)
  if(NOT _t_name)
    set(_t_name "${_t}")
  endif()
  if(NOT _t_name MATCHES "^statusbar-")
    string(REPLACE "_" "-" _t_name "${_t_name}")
    set_target_properties(${_t} PROPERTIES OUTPUT_NAME "statusbar-${_t_name}")
  endif()
endforeach()

# ---- install + export ----
# Skipped under the outer aggregate build, which builds, not installs. Install
# rules are split into runtime (tools / shared libs) and devel (static libs,
# headers, CMake config) components for CPack.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  get_property(_install_targets GLOBAL PROPERTY STATUSBAR_INSTALL_TARGETS)
  install(
    TARGETS ${_install_targets}
    EXPORT statusbar-avbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT devel
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
  install(
    EXPORT statusbar-avbTargets
    FILE statusbar-avbTargets.cmake
    NAMESPACE statusbar::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/statusbar-avb
    COMPONENT devel)
  # Also export the build tree, so a sibling package can find this one without
  # an install step: pass -Dstatusbar-avb_DIR=<build-dir>.
  export(
    EXPORT statusbar-avbTargets
    FILE statusbar-avbTargets.cmake
    NAMESPACE statusbar::)
  install(
    DIRECTORY statusbar/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/statusbar
    COMPONENT devel
    FILES_MATCHING
    PATTERN "*.hpp"
    PATTERN "*.h")
  configure_package_config_file(
    cmake/statusbar-avbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/statusbar-avbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/statusbar-avb)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/statusbar-avbConfigVersion.cmake
    VERSION 1.4.1
    COMPATIBILITY AnyNewerVersion)
  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/statusbar-avbConfig.cmake
          ${CMAKE_CURRENT_BINARY_DIR}/statusbar-avbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/statusbar-avb
    COMPONENT devel)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/docs)
    install(
      DIRECTORY docs/
      DESTINATION ${CMAKE_INSTALL_DOCDIR}
      COMPONENT devel
      FILES_MATCHING
      PATTERN "*.md"
      PATTERN "*.svg"
      PATTERN "*.png")
  endif()

  # ---- udev rule (runtime component) ----
  # Grant non-root read access to the PTP hardware clock so the AVTP talker can
  # timestamp in the gPTP domain instead of silently falling back to the system
  # clock. Absolute DESTINATION (outside the /usr/local prefix) so udev finds
  # it.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(
      FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/udev/60-statusbar-ptp.rules
      DESTINATION /lib/udev/rules.d
      COMPONENT runtime)

    # ---- entity systemd service + /etc config (runtime component) ----
    # Optional run-at-boot for the AVB audio entity. The unit is shipped
    # DISABLED (postinst only daemon-reloads); an operator enables it after
    # editing the conffile. Absolute DESTINATIONs (outside the /usr/local
    # prefix) so systemd and the operator find them in the standard places.
    # entity.toml is registered as a dpkg conffile (see CONTROL_EXTRA below), so
    # per-site edits survive package upgrades. It ships GENERIC defaults only --
    # no site-specific peers/keys.
    install(
      FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/statusbar-avb-entity.service
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/statusbar-avb-tone-generator.service
      DESTINATION /lib/systemd/system
      COMPONENT runtime)
    install(
      FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/etc/entity.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/etc/tone-generator.toml
      DESTINATION /etc/statusbar-avb
      COMPONENT runtime)
  endif()

  # ---- Python helpers (runtime component) ----
  # aemxml/ is already a proper Python package (has __init__.py), so it can be
  # dropped into dist-packages as-is. aemxml_tool.py and
  # descriptor_storage_cross_check.py become /usr/local/bin/ CLI commands.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/python/aemxml/__init__.py)
    install(
      DIRECTORY python/aemxml/
      DESTINATION /usr/lib/python3/dist-packages/aemxml
      COMPONENT runtime
      FILES_MATCHING
      PATTERN "*.py"
      PATTERN "__pycache__" EXCLUDE)
    install(
      PROGRAMS python/aemxml/aemxml_tool.py
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      RENAME statusbar-aemxml
      COMPONENT runtime)
    install(
      PROGRAMS python/aemxml/descriptor_storage_cross_check.py
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      RENAME statusbar-aemxml-cross-check
      COMPONENT runtime)
  endif()

  # owlm_analyze post-processes the CSVs produced by statusbar-owlm-tool
  # --csv-output. Ships as /usr/local/bin/statusbar-owlm-analyze with apt-
  # resolved runtime deps (numpy / pandas / matplotlib).
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/python/owlm/owlm_analyze.py)
    install(
      PROGRAMS python/owlm/owlm_analyze.py
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      RENAME statusbar-owlm-analyze
      COMPONENT runtime)
  endif()
endif()

# ---- packaging (CPack: TGZ + ZIP, plus DEB/RPM on Linux) ----
# Standalone builds only — run `cpack` from the build directory to produce this
# package's archives.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  set(CPACK_PACKAGE_NAME "statusbar-avb")
  set(CPACK_PACKAGE_VENDOR "Jeff Koftinoff")
  set(CPACK_PACKAGE_VERSION "1.4.1")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "AVB/TSN protocol stack — SRP, AVTP, gPTP, ATDECC, NanoAVB, and AVTP crypto — with ATDECC controller and AVTP command-line tools."
  )
  set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER
      "Jeff Koftinoff <jeff.koftinoff@statusbar.com>")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
  set(CPACK_RPM_PACKAGE_AUTOREQ ON)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  endif()
  set(CPACK_DEB_COMPONENT_INSTALL ON)
  set(CPACK_RPM_COMPONENT_INSTALL ON)
  set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
  # Name DEB/RPM files the package-manager-conventional way
  # (<name>_<version>_<arch>.deb), so dependent builds and the orchestrator
  # locate them by package name.
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "statusbar-avb")
  set(CPACK_RPM_RUNTIME_PACKAGE_NAME "statusbar-avb")
  set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Command-line tools and executables")
  set(CPACK_COMPONENTS_ALL runtime devel)
  set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME "statusbar-avb-dev")
  set(CPACK_RPM_DEVEL_PACKAGE_NAME "statusbar-avb-devel")
  # aemxml is pure stdlib Python; owlm_analyze needs numpy/pandas/matplotlib;
  # libcap2-bin provides `setcap` for the postinst capability grant.
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS
      "python3, python3-numpy, python3-pandas, python3-matplotlib, libcap2-bin")
  # -dev pulls its own runtime so its cmake config's IMPORTED targets resolve.
  # >= (not =) so an auto-incremented Debian revision (1.3.0-<rev>, see the
  # container-build scripts) still satisfies the cross-dependencies.
  set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
      "statusbar-avb (>= 1.3.0), statusbar-audio-dev (>= 1.3.0), statusbar-core-dev (>= 1.3.0), statusbar-crypto-dev (>= 1.3.0)"
  )
  set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES
      "statusbar-avb >= 1.3.0, statusbar-audio-devel >= 1.3.0, statusbar-core-devel >= 1.3.0, statusbar-crypto-devel >= 1.3.0"
  )
  set(CPACK_COMPONENT_DEVEL_DESCRIPTION
      "C++ headers, static libraries and CMake package config")
  set(CPACK_GENERATOR "TGZ;ZIP")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPACK_GENERATOR "DEB")
    # Grant raw-ethernet + realtime capabilities to the installed tools on
    # install, so the L2/RT executables run without root (deploy .deb -> ssh ->
    # run). Baked into the runtime package's postinst; DEB-only. @ONLY keeps the
    # shell `$1`/`${...}` syntax intact and substitutes only @SETCAP_BINDIR@.
    set(SETCAP_BINDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/packaging/deb/postinst" @ONLY)
    file(
      CHMOD
      "${CMAKE_CURRENT_BINARY_DIR}/packaging/deb/postinst"
      PERMISSIONS
      OWNER_READ
      OWNER_WRITE
      OWNER_EXECUTE
      GROUP_READ
      GROUP_EXECUTE
      WORLD_READ
      WORLD_EXECUTE)
    # postinst (setcap + daemon-reload), prerm (stop the entity unit on
    # remove/upgrade), postrm (daemon-reload), and conffiles (mark
    # /etc/statusbar-avb/entity.toml so per-site edits survive upgrades).
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/deb/postinst"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/prerm"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/conffiles")
    find_program(_rpmbuild rpmbuild)
    if(_rpmbuild)
      list(APPEND CPACK_GENERATOR "RPM")
    endif()
  endif()
  include(CPack)
endif()

# Auto-discovering fuzz-smoke / fuzz-all targets (no-op unless ENABLE_FUZZING).
statusbar_register_fuzz_targets()

# clang-tidy aggregate target (no-op unless clang-tidy is on PATH).
statusbar_register_clang_tidy_target()

# docs-sm: regenerate per-state-machine Markdown docs under docs/sm/. Only
# registered for standalone avb builds; the outer aggregate build provides its
# own workspace-wide docs-sm.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  set(_sm_render ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sm-docs-render.sh)
  add_custom_target(
    docs-sm
    COMMAND ${_sm_render} $<TARGET_FILE:atdecc_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/atdecc
    COMMAND ${_sm_render} $<TARGET_FILE:avtp_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/avtp
    COMMAND ${_sm_render} $<TARGET_FILE:gptp_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/gptp
    COMMAND ${_sm_render} $<TARGET_FILE:srp_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/srp
    COMMAND ${_sm_render} $<TARGET_FILE:nanoavb_supervisor_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/nanoavb
    COMMAND ${_sm_render} $<TARGET_FILE:stun_sm_tool>
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/sm/stun
    DEPENDS atdecc_sm_tool avtp_sm_tool gptp_sm_tool srp_sm_tool
            nanoavb_supervisor_sm_tool stun_sm_tool
    COMMENT
      "Regenerating per-SM Markdown + SVG under avb/docs/sm/ (needs graphviz)"
    VERBATIM USES_TERMINAL)
endif()
