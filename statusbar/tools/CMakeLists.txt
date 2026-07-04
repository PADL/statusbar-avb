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

# Ship the canonical dual-format AEM descriptor blob. Generate it at build time
# from the just-built tool so the entity always has a correct, build-matched
# descriptor present (no hand-placed /tmp blob that can go stale). Installed to
# <datadir>/statusbar-avb/entity_audio.bin; the audio-io tool defaults to it.
set(STATUSBAR_AVB_DESCRIPTOR_BLOB
    "${CMAKE_CURRENT_BINARY_DIR}/entity_audio.bin")
add_custom_command(
  OUTPUT "${STATUSBAR_AVB_DESCRIPTOR_BLOB}"
  COMMAND aem_entity_blob_tool --dual --channels 8 --sample-rate 96000 --name
          "AVB Audio IO" --out "${STATUSBAR_AVB_DESCRIPTOR_BLOB}"
  DEPENDS aem_entity_blob_tool
  COMMENT "Generating canonical AEM descriptor blob entity_audio.bin"
  VERBATIM)
add_custom_target(statusbar_avb_descriptor_blob ALL
                  DEPENDS "${STATUSBAR_AVB_DESCRIPTOR_BLOB}")
install(
  FILES "${STATUSBAR_AVB_DESCRIPTOR_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# Ship the talker-only tone-generator blob (3 stream outputs, no inputs).
# Consumed by the avb_entity_tone_generator tool; installed alongside the
# audio-io blob as entity_tone.bin.
set(STATUSBAR_AVB_TONE_BLOB "${CMAKE_CURRENT_BINARY_DIR}/entity_tone.bin")
add_custom_command(
  OUTPUT "${STATUSBAR_AVB_TONE_BLOB}"
  COMMAND aem_entity_blob_tool --tone --channels 8 --sample-rate 96000 --name
          "Statusbar Tone Generator" --out "${STATUSBAR_AVB_TONE_BLOB}"
  DEPENDS aem_entity_blob_tool
  COMMENT "Generating tone-generator AEM descriptor blob entity_tone.bin"
  VERBATIM)
add_custom_target(statusbar_avb_tone_blob ALL DEPENDS "${STATUSBAR_AVB_TONE_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# AAF-only tone-generator blob (single 8-ch AAF stream output, no AM824/CRF).
# Consumed by the tone-generator tool with --streams=aaf.
set(STATUSBAR_AVB_TONE_AAF_BLOB "${CMAKE_CURRENT_BINARY_DIR}/entity_tone_aaf.bin")
add_custom_command(
  OUTPUT "${STATUSBAR_AVB_TONE_AAF_BLOB}"
  COMMAND aem_entity_blob_tool --tone-aaf --channels 8 --sample-rate 96000 --name
          "Statusbar Tone Generator" --out "${STATUSBAR_AVB_TONE_AAF_BLOB}"
  DEPENDS aem_entity_blob_tool
  COMMENT "Generating AAF-only tone-generator AEM descriptor blob entity_tone_aaf.bin"
  VERBATIM)
add_custom_target(statusbar_avb_tone_aaf_blob ALL
                  DEPENDS "${STATUSBAR_AVB_TONE_AAF_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_AAF_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# AAF + CRF tone-generator blob (AAF audio stream 0 + CRF media clock stream 1).
# Consumed by the tone-generator tool with --streams=aaf+crf.
set(STATUSBAR_AVB_TONE_AAF_CRF_BLOB
    "${CMAKE_CURRENT_BINARY_DIR}/entity_tone_aaf_crf.bin")
add_custom_command(
  OUTPUT "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}"
  COMMAND aem_entity_blob_tool --tone-aaf-crf --channels 8 --sample-rate 96000
          --name "Statusbar Tone Generator" --out
          "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}"
  DEPENDS aem_entity_blob_tool
  COMMENT "Generating AAF+CRF tone-generator AEM descriptor blob entity_tone_aaf_crf.bin"
  VERBATIM)
add_custom_target(statusbar_avb_tone_aaf_crf_blob ALL
                  DEPENDS "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}")
install(
  FILES "${STATUSBAR_AVB_TONE_AAF_CRF_BLOB}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/statusbar-avb"
  COMPONENT runtime)

# --- AEM model JSON -> blob (Python aemxml pipeline) + migration verify ---
# The declarative models in avb/examples/*.json are the emerging single source of
# truth for the AEM descriptor blobs; the pure-stdlib Python aemxml tool compiles
# them (`aemxml_tool.py json2bin`). The C++ aem_entity_blob_tool above still ships
# the runtime entity_*.bin blobs; this block proves the JSON models are ready to
# take over by compiling each one and checking its descriptor-type multiset matches
# the C++ generator's (byte-identity is not required -- only what the C++
# DescriptorStorage parser sees at runtime). To flip the switch later, point the
# entity_*.bin commands above at `${Python3_EXECUTABLE} <aemxml_tool> json2bin
# examples/<model>.json` and retire the C++ model builders (keep the DescriptorStorage
# parser -- it stays the runtime consumer).
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
  set(_aem_examples "${CMAKE_CURRENT_LIST_DIR}/../../examples")
  set(_aemxml_tool "${CMAKE_CURRENT_LIST_DIR}/../../python/aemxml/aemxml_tool.py")

  # Compile every declarative model to a build-tree blob (build-time proof the
  # json2bin pipeline runs; not installed -- the C++ blobs above stay canonical).
  set(_aem_json_blobs)
  foreach(_m bridge dual tone tone-aaf tone-aaf-crf simple_stereo)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/aem"
      COMMAND ${Python3_EXECUTABLE} "${_aemxml_tool}" json2bin
              "${_aem_examples}/${_m}.json"
              "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem"
      DEPENDS "${_aem_examples}/${_m}.json"
      COMMENT "AEM blob (json2bin): ${_m}.aem <- examples/${_m}.json"
      VERBATIM)
    list(APPEND _aem_json_blobs "${CMAKE_CURRENT_BINARY_DIR}/aem/${_m}.aem")
  endforeach()
  add_custom_target(statusbar_avb_json_blobs ALL DEPENDS ${_aem_json_blobs})

  # Regression net: each JSON model's blob must match the C++ generator's descriptor
  # set, so the C++ model builders can be retired with confidence.
  add_test(
    NAME statusbar/avb/aem_json_models_match_cpp
    COMMAND ${Python3_EXECUTABLE} "${_aem_examples}/verify_models.py"
            $<TARGET_FILE:aem_entity_blob_tool>)
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
