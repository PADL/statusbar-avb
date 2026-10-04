cmake_minimum_required(VERSION 3.31)

# ---- version --------------------------------------------------------------
# Single source of truth: the repo's latest release tag. Resolution order: 1.
# -DSTATUSBAR_AVB_VERSION=x.y.z  (scripts/container-build.sh resolves the tag on
# the host -- the container mounts the tree read-only without a usable .git, so
# git describe cannot run in there) 2. git describe on this tree (developer
# builds) 3. the fallback literal below (tarball builds; bump on release only if
# neither of the above is available) The version flows from here into the
# package metadata, the entities' firmware_version (STATUSBAR_AVB_VERSION
# compile definition), and the AEM blob models (json2bin --set version=...).
if(NOT STATUSBAR_AVB_VERSION)
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --tags --abbrev=0
            --match "[0-9]*.[0-9]*.[0-9]*"
    OUTPUT_VARIABLE STATUSBAR_AVB_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT STATUSBAR_AVB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  set(STATUSBAR_AVB_VERSION 2.1.0)
endif()

project(
  statusbar-avb
  VERSION ${STATUSBAR_AVB_VERSION}
  LANGUAGES CXX)
message(STATUS "statusbar-avb version: ${PROJECT_VERSION}")

# Baked into the entities' ENTITY descriptor firmware_version (see
# statusbar/avb_entity/avb_entity_version.hpp).
add_compile_definitions(STATUSBAR_AVB_VERSION="${PROJECT_VERSION}")

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

# Uniform statusbar- prefixed on-disk names for installed executables
# (statusbar_prefix_tool_names in cmake/module.cmake).
statusbar_prefix_tool_names()

# ---- install + export ----
# Standalone install/export boilerplate lives in
# statusbar_package_install_export (cmake/module.cmake); no-op under an outer
# aggregate build.
statusbar_package_install_export(NAME avb)

if(NOT STATUSBAR_AGGREGATE_BUILD)

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
      FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/etc/entity.toml
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
      PATTERN "*_test.py" EXCLUDE
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
  statusbar_package_cpack_defaults(
    NAME
    avb
    DESCRIPTION
    "AVB/TSN protocol stack — SRP, AVTP, gPTP, ATDECC, NanoAVB, and AVTP crypto — with ATDECC controller and AVTP command-line tools."
  )
  # Hard dependencies only: aemxml is pure stdlib Python so it needs just the
  # interpreter, and libcap2-bin provides `setcap` for the postinst capability
  # grant (the postinst fails without it).
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "python3, libcap2-bin")
  # The numeric stack is needed only by statusbar-owlm-analyze, an optional
  # post-processing tool for the CSVs that statusbar-owlm-tool emits. Nothing
  # else in the package touches it, so it is a Recommends: apt still installs it
  # by default, but the package remains installable where the stack is not
  # available. That matters concretely for cross-builds -- python3-pandas has no
  # installable :arm64 multiarch variant on trixie or forky, so as a Depends it
  # made statusbar-avb (and anything depending on it) impossible to install in
  # the arm64 cross builder.
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_RECOMMENDS
      "python3-numpy, python3-pandas, python3-matplotlib")
  # -dev pulls its own runtime so its cmake config's IMPORTED targets resolve.
  # >= (not =) so an auto-incremented Debian revision (1.3.0-<rev>, see the
  # container-build scripts) still satisfies the cross-dependencies. Floors
  # track the versions this build was actually made against (find_package sets
  # statusbar-<dep>_VERSION), never a hardcoded literal that goes stale on the
  # next release.
  set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
      "statusbar-avb (>= ${PROJECT_VERSION}), statusbar-audio-dev (>= ${statusbar-audio_VERSION}), statusbar-core-dev (>= ${statusbar-core_VERSION}), statusbar-crypto-dev (>= ${statusbar-crypto_VERSION})"
  )
  set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES
      "statusbar-avb >= ${PROJECT_VERSION}, statusbar-audio-devel >= ${statusbar-audio_VERSION}, statusbar-core-devel >= ${statusbar-core_VERSION}, statusbar-crypto-devel >= ${statusbar-crypto_VERSION}"
  )
  set(CPACK_COMPONENT_DEVEL_DESCRIPTION
      "C++ headers, static libraries and CMake package config")

  # ---- Wireshark dissectors: a third component and its own packages ----
  # wireshark/ is Lua for Wireshark rather than part of the library, so it ships
  # on its own: the arch-independent Debian package statusbar-avb-wireshark and,
  # through CPACK_ARCHIVE_COMPONENT_INSTALL, a
  # statusbar-avb-<ver>-<sys>-wireshark tarball for other platforms. Nothing in
  # the runtime or devel packages depends on it.
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/wireshark/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/statusbar-avb/wireshark
    COMPONENT wireshark
    FILES_MATCHING
    PATTERN "*.lua"
    PATTERN "LICENSE"
    PATTERN "README.md")
  list(APPEND CPACK_COMPONENTS_ALL wireshark)
  set(CPACK_COMPONENT_WIRESHARK_DESCRIPTION
      "Wireshark Lua dissectors for IEEE 1722 / IEEE 1722.1 (MIT)")
  set(CPACK_DEBIAN_WIRESHARK_PACKAGE_NAME "statusbar-avb-wireshark")
  set(CPACK_DEBIAN_WIRESHARK_PACKAGE_ARCHITECTURE "all")
  set(CPACK_DEBIAN_WIRESHARK_PACKAGE_SHLIBDEPS OFF)
  set(CPACK_DEBIAN_WIRESHARK_PACKAGE_RECOMMENDS "wireshark | tshark")
  set(CPACK_DEBIAN_WIRESHARK_PACKAGE_SECTION "net")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
  endif()
  include(CPack)
endif()

# Auto-discovering fuzz-smoke / fuzz-all targets (no-op unless ENABLE_FUZZING).
statusbar_register_fuzz_targets()

# clang-tidy aggregate target (no-op unless clang-tidy is on PATH).
statusbar_register_clang_tidy_target()

# docs-sm: regenerate per-state-machine Markdown docs under docs/sm/. Only
# registered for standalone avb builds; the outer aggregate build creates a
# workspace-wide docs-sm from the same registry. Each SM tool registers itself
# via statusbar_register_sm_doc() beside its add_executable(), so neither this
# file nor the umbrella names the tools.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  statusbar_register_sm_docs_target()
endif()
