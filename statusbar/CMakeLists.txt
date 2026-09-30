# Package INTERFACE library, include path and the global accumulation properties
# (statusbar_package_module_init in cmake/module.cmake).
statusbar_package_module_init()

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

# Standalone per-package test binary (statusbar_package_test_binary in
# cmake/module.cmake); the outer aggregate build builds one combined
# statusbar_test from every aggregated package instead.
statusbar_package_test_binary()
