# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
#
# Tool executables that link the package `statusbar` INTERFACE library, which
# transitively provides every module in this package and its dependency
# packages. Collected here (rather than next to each library module) so the
# build-target paths are uniform.

# --- packet-capture / dump tools (consume core's bpf + pcap) ---

# bpf_dump_tool consumes statusbar-bpf directly; skip it when the bpf module
# isn't built. Same gate the core package uses.
if(STATUSBAR_ENABLE_BPF)
  add_executable(bpf_dump_tool bpf_dump_tool.cpp)
  target_link_libraries(bpf_dump_tool PRIVATE statusbar)
  set_target_properties(bpf_dump_tool PROPERTIES OUTPUT_NAME statusbar-bpf-dump)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS bpf_dump_tool)
endif()

add_executable(pcap_dump_tool pcap_dump_tool.cpp)
target_link_libraries(pcap_dump_tool PRIVATE statusbar)
set_target_properties(pcap_dump_tool PROPERTIES OUTPUT_NAME statusbar-pcap-dump)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS pcap_dump_tool)

# --- STUN tools (link the stun module via the package umbrella) ---

add_executable(stun_server_tool stun_server_tool.cpp)
target_link_libraries(stun_server_tool PRIVATE statusbar)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS stun_server_tool)

add_executable(stun_client_tool stun_client_tool.cpp)
target_link_libraries(stun_client_tool PRIVATE statusbar)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS stun_client_tool)

add_executable(stun_sm_tool stun_sm_tool.cpp)
target_link_libraries(stun_sm_tool PRIVATE statusbar)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS stun_sm_tool)

# --- AEM entity descriptor-storage blob generator (cross-platform) ---

add_executable(aem_entity_blob_tool aem_entity_blob_tool.cpp)
target_link_libraries(aem_entity_blob_tool PRIVATE statusbar)
set_target_properties(aem_entity_blob_tool PROPERTIES OUTPUT_NAME
                                                      statusbar-aem-entity-blob)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    aem_entity_blob_tool)

# Unit test for the AemEntityBlob writer (statusbar/tools/aem_entity_blob.hpp).
# tools/ is not a statusbar_add_module, so register the test file directly into
# the global list the umbrella feeds to create_test_sourcelist.
set_property(
  GLOBAL APPEND PROPERTY STATUSBAR_TEST_FILES
                         "${CMAKE_CURRENT_LIST_DIR}/aem_entity_blob_test.cpp")

# --- Runtime entity blobs: the declarative JSON models are the source of truth
# ---
# The runtime entity_*.bin descriptor-storage blobs the entities load at startup
# are generated at build time from the declarative avb/examples/*.json models,
# compiled by the pure-stdlib Python aemxml tool (`aemxml_tool.py json2bin`).
# This makes the JSON the single source of truth (editable without recompiling).
# The C++ aem_entity_blob_tool above is retained (a) as the reference for the
# CONTENT-parity regression test (verify_models.py) and (b) as a fallback
# generator when Python3 is unavailable or STATUSBAR_AVB_BLOBS_FROM_JSON is OFF.
# The C++ DescriptorStorage parser remains the runtime consumer either way.
find_package(Python3 QUIET COMPONENTS Interpreter)
set(_aem_examples "${CMAKE_CURRENT_LIST_DIR}/../../examples")
set(_aemxml_tool "${CMAKE_CURRENT_LIST_DIR}/../../python/aemxml/aemxml_tool.py")

option(
  STATUSBAR_AVB_BLOBS_FROM_JSON
  "Generate the runtime AEM entity blobs from examples/*.json (Python aemxml json2bin) instead of the C++ generator"
  ON)

if(STATUSBAR_AVB_BLOBS_FROM_JSON AND NOT Python3_FOUND)
  message(
    WARNING
      "STATUSBAR_AVB_BLOBS_FROM_JSON=ON but Python3 was not found; falling back to the C++ aem_entity_blob_tool for the runtime entity blobs."
  )
endif()

# _statusbar_avb_entity_blob(<out-abspath> <cpp-flag> <json-model> <cpp-name>
# <label>) Emit an add_custom_command producing <out> either from json2bin
# <json-model>.json (JSON source of truth) or, as a fallback, from
# `aem_entity_blob_tool <cpp-flag>`.
function(_statusbar_avb_entity_blob out cpp_flag json_model cpp_name label)
  if(STATUSBAR_AVB_BLOBS_FROM_JSON AND Python3_FOUND)
    add_custom_command(
      OUTPUT "${out}"
      COMMAND ${Python3_EXECUTABLE} "${_aemxml_tool}" json2bin
              "${_aem_examples}/${json_model}.json" "${out}"
      DEPENDS "${_aem_examples}/${json_model}.json" "${_aemxml_tool}"
      COMMENT "Generating ${label} from examples/${json_model}.json (json2bin)"
      VERBATIM)
  else()
    add_custom_command(
      OUTPUT "${out}"
      COMMAND aem_entity_blob_tool ${cpp_flag} --channels 8 --sample-rate 96000
              --name "${cpp_name}" --out "${out}"
      DEPENDS aem_entity_blob_tool
      COMMENT "Generating ${label} from the C++ aem_entity_blob_tool (fallback)"
      VERBATIM)
  endif()
endfunction()

# Dual-format audio-io blob (2 in + 3 out). Installed to
# <datadir>/statusbar-avb/entity_audio.bin; the audio-io tool defaults to it.
set(STATUSBAR_AVB_DESCRIPTOR_BLOB
    "${CMAKE_CURRENT_BINARY_DIR}/entity_audio.bin")
_statusbar_avb_entity_blob("${STATUSBAR_AVB_DESCRIPTOR_BLOB}" "--dual" "dual"
                           "AVB Audio IO" "entity_audio.bin")
add_custom_target(statusbar_avb_descriptor_blob ALL
                  DEPENDS "${STATUSBAR_AVB_DESCRIPTOR_BLOB}")
install(
  FILES "${STATUSBAR_AVB_DESCRIPTOR_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# Talker-only tone-generator blob (3 stream outputs, no inputs).
# entity_tone.bin.
set(STATUSBAR_AVB_TONE_BLOB "${CMAKE_CURRENT_BINARY_DIR}/entity_tone.bin")
_statusbar_avb_entity_blob("${STATUSBAR_AVB_TONE_BLOB}" "--tone" "tone"
                           "Statusbar Tone Generator" "entity_tone.bin")
add_custom_target(statusbar_avb_tone_blob ALL
                  DEPENDS "${STATUSBAR_AVB_TONE_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# AAF-only tone-generator blob (single 8-ch AAF output). entity_tone_aaf.bin.
set(STATUSBAR_AVB_TONE_AAF_BLOB
    "${CMAKE_CURRENT_BINARY_DIR}/entity_tone_aaf.bin")
_statusbar_avb_entity_blob(
  "${STATUSBAR_AVB_TONE_AAF_BLOB}" "--tone-aaf" "tone-aaf"
  "Statusbar Tone Generator" "entity_tone_aaf.bin")
add_custom_target(statusbar_avb_tone_aaf_blob ALL
                  DEPENDS "${STATUSBAR_AVB_TONE_AAF_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_AAF_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# AAF + CRF tone-generator blob (AAF audio + CRF media clock).
# entity_tone_aaf_crf.bin.
set(STATUSBAR_AVB_TONE_AAF_CRF_BLOB
    "${CMAKE_CURRENT_BINARY_DIR}/entity_tone_aaf_crf.bin")
_statusbar_avb_entity_blob(
  "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}" "--tone-aaf-crf" "tone-aaf-crf"
  "Statusbar Tone Generator" "entity_tone_aaf_crf.bin")
add_custom_target(statusbar_avb_tone_aaf_crf_blob ALL
                  DEPENDS "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# --- AEM model JSON -> blob build-proof + CONTENT-parity regression test ---
# Compile every declarative model to a build-tree blob (build-time proof the
# json2bin pipeline runs for all shapes, including the ones with no runtime
# blob) and assert each one's descriptor CONTENT matches the C++ generator's, so
# the C++ model builders can be retired with confidence (the C++
# DescriptorStorage parser stays -- it is the runtime consumer). Also run the
# aemxml Python unit suite so the pipeline can't rot.
if(Python3_FOUND)
  set(_aem_json_blobs)
  foreach(_m bridge dual tone tone-aaf tone-aaf-crf simple_stereo)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/aem"
      COMMAND
        ${Python3_EXECUTABLE} "${_aemxml_tool}" json2bin
        "${_aem_examples}/${_m}.json"
        "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem"
      DEPENDS "${_aem_examples}/${_m}.json" "${_aemxml_tool}"
      COMMENT "AEM blob (json2bin): ${_m}.aem <- examples/${_m}.json"
      VERBATIM)
    list(APPEND _aem_json_blobs "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem")
  endforeach()
  add_custom_target(statusbar_avb_json_blobs ALL DEPENDS ${_aem_json_blobs})

  # Regression net: each JSON model's blob must CONTENT-match the C++
  # generator's descriptor set (byte-identity per (type, index), modulo
  # non-authored fields).
  add_test(NAME statusbar/avb/aem_json_models_match_cpp
           COMMAND ${Python3_EXECUTABLE} "${_aem_examples}/verify_models.py"
                   $<TARGET_FILE:aem_entity_blob_tool>)

  # Unit suite for the aemxml pipeline (model / flatten / blob / AEMXML / JSON).
  add_test(
    NAME statusbar/avb/aemxml_python_unit
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_LIST_DIR}/../../python/aemxml/aemxml_test.py")
endif()

# --- L2 ADPDU send probe (Linux: AF_PACKET, diagnoses PACKET_QDISC_BYPASS) ---

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(adp_l2send_probe_tool adp_l2send_probe_tool.cpp)
  target_link_libraries(adp_l2send_probe_tool PRIVATE statusbar)
  set_target_properties(adp_l2send_probe_tool
                        PROPERTIES OUTPUT_NAME statusbar-adp-l2send-probe)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      adp_l2send_probe_tool)
endif()

# --- owlm one-way latency measurement tool (Linux: raw sockets) ---

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(owlm_tool owlm_tool.cpp)
  target_link_libraries(owlm_tool PRIVATE statusbar)
  set_target_properties(owlm_tool PROPERTIES OUTPUT_NAME owlm_tool)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS owlm_tool)

  # Smoke test — skipped (exit 77) unless OWLM_TEST_IFACE is set in the
  # environment. Hand-run with: OWLM_TEST_IFACE=eth0 ctest -R owlm_loopback
  add_test(NAME owlm_loopback_smoke
           COMMAND ${PROJECT_SOURCE_DIR}/scripts/owlm/loopback_smoke.sh
                   $<TARGET_FILE:owlm_tool>)
  set_tests_properties(
    owlm_loopback_smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS
                                   "smoke;needs-network")
endif()

# --- real-time AVTP latency tester (Linux: PACKET_MMAP) ---

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(rttest_send_avtp_tool rttest_send_avtp_tool.cpp)
  target_link_libraries(rttest_send_avtp_tool PRIVATE statusbar)
  set_target_properties(rttest_send_avtp_tool
                        PROPERTIES OUTPUT_NAME statusbar-rttest-send-avtp-tool)
  target_link_options(rttest_send_avtp_tool PRIVATE -static-libgcc
                      -static-libstdc++)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      rttest_send_avtp_tool)
endif()

# --- PHC<->GPS frequency-ratio tracker (Linux: /dev/ptp0 + CLOCK_REALTIME) ---

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(gps_ratio_tracker gps_ratio_tracker.cpp)
  target_link_libraries(gps_ratio_tracker PRIVATE statusbar)
  set_target_properties(gps_ratio_tracker
                        PROPERTIES OUTPUT_NAME statusbar-gps-ratio-tracker)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      gps_ratio_tracker)
endif()
