include(${PROJECT_SOURCE_DIR}/cmake/module.cmake)

# Package INTERFACE library — every module target reachable in this package's
# build. Module tool executables link `statusbar`. The outer aggregate build
# supplies a single shared `statusbar`.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  add_library(statusbar INTERFACE)
endif()

# Every package adds its own source root to the package INTERFACE library's
# include path, so tool and example sources can #include headers
# ("statusbar/<dir>/...") that live inside this package.
target_include_directories(statusbar
                           INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)

define_property(
  GLOBAL
  PROPERTY STATUSBAR_TEST_FILES
  BRIEF_DOCS "test sources"
  FULL_DOCS "accumulated test sources")
define_property(
  GLOBAL
  PROPERTY STATUSBAR_INSTALL_TARGETS
  BRIEF_DOCS "install targets"
  FULL_DOCS "accumulated install targets")
# The outer aggregate build initialises these once and lets them accumulate
# across every package; a standalone build resets per package.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  set_property(GLOBAL PROPERTY STATUSBAR_TEST_FILES "")
  set_property(GLOBAL PROPERTY STATUSBAR_INSTALL_TARGETS "")
endif()

# Helper: build a libFuzzer harness (Linux) or standalone driver (macOS) for avb
# parsers. Mirrors crypto's add_crypto_fuzzer.
function(add_avb_fuzzer source_file)
  if(NOT ENABLE_FUZZING)
    return()
  endif()
  get_filename_component(FUZZ_NAME ${source_file} NAME_WE)
  if(APPLE)
    add_executable(${FUZZ_NAME} ${source_file})
    target_link_libraries(${FUZZ_NAME} PRIVATE statusbar)
    target_compile_options(${FUZZ_NAME} PRIVATE -fsanitize=address,undefined
                                                -fno-omit-frame-pointer)
    target_link_options(${FUZZ_NAME} PRIVATE -fsanitize=address,undefined)
  else()
    add_executable(${FUZZ_NAME} ${source_file})
    target_link_libraries(${FUZZ_NAME} PRIVATE statusbar)
    target_compile_options(
      ${FUZZ_NAME} PRIVATE -fsanitize=fuzzer,address,undefined
                           -fno-omit-frame-pointer)
    target_link_options(
      ${FUZZ_NAME} PRIVATE -fsanitize=fuzzer,address,undefined
      ${FUZZER_EXTRA_LINK_OPTIONS})
  endif()
endfunction()

# Module subdirectories — dependency order.
add_subdirectory(srp)
add_subdirectory(avtp)
add_subdirectory(gptp)
add_subdirectory(atdecc)
add_subdirectory(avtp_crypto)
add_subdirectory(nanoavb)
add_subdirectory(ptpclient)
add_subdirectory(avb_entity)
add_subdirectory(atdecc_tools)
add_subdirectory(avtp_tools)

# Networking and measurement modules. udptun and stun come before owlm (owlm
# links both). tools/ is last — its executables link the package `statusbar`
# umbrella.
add_subdirectory(udptun)
add_subdirectory(stun)
add_subdirectory(owlm)
add_subdirectory(netdump)
add_subdirectory(tools)

# Populate the package INTERFACE library — skipped under the outer aggregate
# build, which populates a single shared `statusbar` itself.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  target_link_libraries(
    statusbar
    INTERFACE statusbar-srp
              statusbar-avtp
              statusbar-gptp
              statusbar-atdecc
              statusbar-avtp_crypto
              statusbar-nanoavb
              statusbar-ptpclient
              statusbar-avb_entity
              statusbar-atdecc_tools
              statusbar-avtp_tools
              statusbar-udptun
              statusbar-stun
              statusbar-owlm
              statusbar-netdump
              statusbar::args
              statusbar::audio
              statusbar::benchmark
              statusbar::bpf
              statusbar::buffer
              statusbar::colbin
              statusbar::config
              statusbar::container
              statusbar::crypto
              statusbar::csv
              statusbar::dsp
              statusbar::engine
              statusbar::ieee
              statusbar::ip
              statusbar::itc
              statusbar::ltc
              statusbar::midi
              statusbar::net
              statusbar::osc
              statusbar::pcap
              statusbar::realtime
              statusbar::sm
              statusbar::stats
              statusbar::status
              statusbar::test
              statusbar::toml
              statusbar::tsn
              statusbar::tui)
endif()

# Aggregate test binary built from every module's test sources. Skipped under
# the outer aggregate build, which builds one combined statusbar_test from every
# package.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  get_property(STATUSBAR_TEST_FILES_ABS GLOBAL PROPERTY STATUSBAR_TEST_FILES)
  set(STATUSBAR_TEST_FILES "")
  foreach(TEST_FILE_ABS ${STATUSBAR_TEST_FILES_ABS})
    file(RELATIVE_PATH TEST_FILE_REL "${PROJECT_SOURCE_DIR}" "${TEST_FILE_ABS}")
    list(APPEND STATUSBAR_TEST_FILES "${TEST_FILE_REL}")
  endforeach()
  list(SORT STATUSBAR_TEST_FILES)
  if(STATUSBAR_TEST_FILES)
    create_test_sourcelist(STATUSBAR_TEST_SOURCES statusbar_test.cpp
                           ${STATUSBAR_TEST_FILES})
    add_executable(statusbar_test ${CMAKE_CURRENT_BINARY_DIR}/statusbar_test.cpp
                                  ${STATUSBAR_TEST_FILES_ABS})
    target_include_directories(statusbar_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(statusbar_test PRIVATE statusbar)
    foreach(TEST_FILE ${STATUSBAR_TEST_FILES})
      string(REPLACE ".cpp" "" TEST_NAME "${TEST_FILE}")
      add_test(NAME ${TEST_NAME} COMMAND statusbar_test ${TEST_NAME})
    endforeach()
    add_test(NAME statusbar_all_test COMMAND statusbar_test -A)
    # The -A run-all branch of CMake's create_test_sourcelist driver cannot
    # fail: it `return 0`s unconditionally, and it only prints "not ok" when a
    # test function returns -1 -- but TestRegister::run_section() returns 1.
    # Judge the run by the harness's own output instead of the driver's exit
    # code (same fix as the umbrella aggregate): no FAIL: line, and the run must
    # reach the end (so a crash part-way through cannot pass either).
    set_tests_properties(
      statusbar_all_test
      PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL: " PASS_REGULAR_EXPRESSION
                 "All tests finished\\.")
    statusbar_register_coverage_targets(statusbar_test)
  endif()
endif()
