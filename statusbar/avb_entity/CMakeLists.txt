# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  avb_entity
  SOURCES
  avb_entity_stereo_io.cpp
  avb_entity_am824_io.cpp
  avb_entity_audio_io.cpp
  avb_entity_tone_generator.cpp
  avb_entity_host.cpp
  avb_entity_kit.cpp
  avb_entity_stream_counters.cpp
  avb_entity_stream_spec.cpp
  avb_entity_listener_streams.cpp
  avb_entity_talker_streams.cpp
  avb_entity_talker_gate.cpp
  avb_entity_udptun_bridge.cpp
  avb_entity_udptun_transport.cpp
  avb_entity_udptun_ingest_path.cpp
  avb_entity_udptun_egress_path.cpp
  DEPS
  statusbar::atdecc
  statusbar::avtp
  statusbar::buffer
  statusbar::crypto
  statusbar::dsp
  statusbar::gptp
  statusbar::ieee
  statusbar::nanoavb
  statusbar::net
  statusbar::pcap
  statusbar::ptpclient
  statusbar::realtime
  statusbar::sm
  statusbar::status
  statusbar::stun
  statusbar::tsn
  statusbar::udptun
  TESTS
  avb_entity_host_test.cpp
  avb_entity_talker_streams_test.cpp
  avb_entity_udptun_bridge_test.cpp
  avb_entity_stereo_io_test.cpp
  avb_entity_am824_io_test.cpp
  avb_entity_audio_io_test.cpp
  avb_entity_tone_generator_test.cpp
  avb_entity_stream_counters_test.cpp
  avb_entity_stream_spec_test.cpp
  avb_entity_listener_streams_test.cpp
  avb_entity_listener_bindings_test.cpp
  avb_entity_crf_clock_recovery_test.cpp
  avb_entity_media_rate_test.cpp
  avb_entity_udptun_egress_test.cpp
  avb_entity_udptun_ingest_test.cpp
  avb_entity_stream_rx_sink_test.cpp
  avb_entity_aaf_reframe_test.cpp
  tx_pcap_recorder_test.cpp
  log_sweep_generator_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(avb_entity_stereo_io_tool avb_entity_stereo_io_tool.cpp)
  target_link_libraries(avb_entity_stereo_io_tool PRIVATE statusbar)
  set_target_properties(avb_entity_stereo_io_tool
                        PROPERTIES OUTPUT_NAME statusbar-avb-stereo-io)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      avb_entity_stereo_io_tool)

  add_executable(avb_entity_am824_io_tool avb_entity_am824_io_tool.cpp)
  target_link_libraries(avb_entity_am824_io_tool PRIVATE statusbar)
  set_target_properties(avb_entity_am824_io_tool
                        PROPERTIES OUTPUT_NAME statusbar-avb-am824-io)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      avb_entity_am824_io_tool)

  add_executable(avb_entity_audio_io_tool avb_entity_audio_io_tool.cpp)
  target_link_libraries(avb_entity_audio_io_tool PRIVATE statusbar)
  # Default --descriptor-storage to the blob shipped by the package (generated
  # in statusbar/tools/CMakeLists.txt), so the entity runs with no extra setup.
  target_compile_definitions(
    avb_entity_audio_io_tool
    PRIVATE
      STATUSBAR_AVB_DEFAULT_BLOB="${CMAKE_INSTALL_FULL_DATADIR}/statusbar-avb/entity_audio.bin"
  )
  set_target_properties(avb_entity_audio_io_tool
                        PROPERTIES OUTPUT_NAME statusbar-avb-audio-io)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      avb_entity_audio_io_tool)

  add_executable(avb_entity_tone_generator_tool
                 avb_entity_tone_generator_tool.cpp)
  target_link_libraries(avb_entity_tone_generator_tool PRIVATE statusbar)
  # Default --descriptor-storage to the tone blob shipped by the package
  # (generated in statusbar/tools/CMakeLists.txt).
  target_compile_definitions(
    avb_entity_tone_generator_tool
    PRIVATE
      STATUSBAR_AVB_DEFAULT_TONE_BLOB="${CMAKE_INSTALL_FULL_DATADIR}/statusbar-avb/entity_tone.bin"
      STATUSBAR_AVB_DEFAULT_TONE_AAF_BLOB="${CMAKE_INSTALL_FULL_DATADIR}/statusbar-avb/entity_tone_aaf.bin"
      STATUSBAR_AVB_DEFAULT_TONE_AAF_CRF_BLOB="${CMAKE_INSTALL_FULL_DATADIR}/statusbar-avb/entity_tone_aaf_crf.bin"
  )
  set_target_properties(avb_entity_tone_generator_tool
                        PROPERTIES OUTPUT_NAME statusbar-avb-tone-generator)
  set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                      avb_entity_tone_generator_tool)
endif()
