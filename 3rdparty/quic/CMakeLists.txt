CMAKE_MINIMUM_REQUIRED(VERSION 2.8.11)
CMAKE_POLICY(SET CMP0003 NEW)

PROJECT(picoquic C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
SET(CMAKE_C_STANDARD 11)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -g -rdynamic -no-pie")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -rdynamic -no-pie")

# FIND_PACKAGE(PkgConfig REQUIRED)

if((NOT DEFINED ENV{NS3}) OR !$ENV{NS3})
    SET(CMAKE_C_FLAGS "-std=gnu99 -Wall -O2 -g ${CC_WARNING_FLAGS} ${CMAKE_C_FLAGS}")
    SET(GCC_COVERAGE_LINK_FLAGS    "-Wl,--no-as-needed,-lprofiler,--as-needed")
    SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${GCC_COVERAGE_LINK_FLAGS}")
endif()

if(DEFINED OSX_SYSROOT)
    MESSAGE("Before, CMAKE_OSX_SYSROOT set at: ${CMAKE_OSX_SYSROOT}")
    SET(CMAKE_OSX_SYSROOT ${OSX_SYSROOT})
    MESSAGE("CMAKE_OSX_SYSROOT now set at: ${CMAKE_OSX_SYSROOT}")
endif()

if($ENV{DISABLE_DEBUG_PRINTF})
    SET(CMAKE_C_FLAGS "-DDISABLE_DEBUG_PRINTF ${CMAKE_C_FLAGS}")
endif()

if($ENV{DISABLE_QLOG})
    SET(CMAKE_C_FLAGS "-DDISABLE_QLOG ${CMAKE_C_FLAGS}")
endif()

if($ENV{STATIC_RESPONSE})
    SET(CMAKE_C_FLAGS "-DSTATIC_RESPONSE=$ENV{STATIC_RESPONSE} ${CMAKE_C_FLAGS}")
endif()

if($ENV{DEBUG_PLUGIN_EXECUTION_TIME})
    SET(CMAKE_C_FLAGS "-DDEBUG_PLUGIN_EXECUTION_TIME ${CMAKE_C_FLAGS}")
endif()

if($ENV{NS3})
    SET(GCC_COVERAGE_LINK_FLAGS "")
    SET(CMAKE_C_FLAGS "-std=gnu99 -Wall -O2 -g -fPIC -DNS3 ${CC_WARNING_FLAGS} ${CMAKE_C_FLAGS}")
    SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -pie -rdynamic")
endif()

INCLUDE_DIRECTORIES(picoquic picoquictest ../../../picotls/include
    ${PICOTLS_INCLUDE_DIR})

SET(MICHELFRALLOC_STATIC_LIBS
         ${PROJECT_SOURCE_DIR}/picoquic/michelfralloc/libmichelfralloc.a
         ${PROJECT_SOURCE_DIR}/picoquic/michelfralloc/libptmalloc3.a)

SET(PICOQUIC_LIBRARY_FILES
    picoquic/cubic.c
    picoquic/endianness.c
    picoquic/fnv1a.c
    picoquic/frames.c
    picoquic/getset.c
    picoquic/http0dot9.c
    picoquic/intformat.c
    picoquic/logger.c
    picoquic/memory.c
    picoquic/memcpy.c
    picoquic/newreno.c
    picoquic/packet.c
    picoquic/picohash.c
    picoquic/picosocks.c
    picoquic/picosplay.c
    picoquic/plugin.c
    picoquic/protoop.c
    picoquic/queue.c
    picoquic/quicctx.c
    picoquic/sacks.c
    picoquic/sender.c
    picoquic/ticket_store.c
    picoquic/tls_api.c
    picoquic/transport.c
    picoquic/ubpf.c
    picoquic/util.c
    picoquic/red_black_tree.c
        picoquic/michelfralloc/sbrk.c
        picoquic/michelfralloc/sbrk.h
        picoquic/michelfralloc/michelfralloc.c
        picoquic/michelfralloc/michelfralloc.h
    picoquic/cc_common.c picoquic/cc_common.h picoquic/bbr.c picoquic/ibbr.c picoquic/ccpara.c)

set(PICOHTTP_LIBRARY_FILES
    picohttp/democlient.c
    picohttp/demoserver.c
    picohttp/h3zero.c
)

SET(PICOQUIC_TEST_LIBRARY_FILES
    picoquictest/ack_of_ack_test.c
    picoquictest/cleartext_aead_test.c
    picoquictest/cnx_creation_test.c
    picoquictest/float16test.c
    picoquictest/fnv1atest.c
    picoquictest/hashtest.c
    picoquictest/http0dot9test.c
    picoquictest/intformattest.c
    picoquictest/parseheadertest.c
    picoquictest/pn2pn64test.c
    picoquictest/sacktest.c
    picoquictest/skip_frame_test.c
    picoquictest/sim_link.c
    picoquictest/socket_test.c
    picoquictest/splay_test.c
    picoquictest/stream0_frame_test.c
    picoquictest/stresstest.c
    picoquictest/ticket_store_test.c
    picoquictest/tls_api_test.c
    picoquictest/transport_param_test.c
    picoquictest/datagram.c
    picoquictest/microbench.c
        picoquictest/util.c)

SET(PLUGINS_DATAGRAM
    plugins/datagram/parse_datagram_frame.c
    plugins/datagram/send_datagram.c
    plugins/datagram/process_datagram_frame.c
    plugins/datagram/write_datagram_frame.c
    plugins/datagram/get_datagram_socket.c
    plugins/datagram/cnx_state_changed.c
    plugins/datagram/process_datagram_buffer.c
)

SET(PLUGINS_MONITORING
    plugins/monitoring/cnx_state_changed.c
    plugins/monitoring/packet_received.c
    plugins/monitoring/packet_sent.c
    plugins/monitoring/rtt_updated.c
    plugins/monitoring/check_ooo_stream_frame.c
    plugins/monitoring/check_spurious_stream_frame.c
    plugins/monitoring/packet_lost.c
    plugins/monitoring/fast_retransmit.c
    plugins/monitoring/retransmission_timeout.c
    plugins/monitoring/tail_loss_probe.c
    plugins/monitoring/stream_opened.c
    plugins/monitoring/stream_closed.c
)

SET(PLUGINS_MULTIPATH
    plugins/multipath/connection_state_changed.c
    plugins/multipath/get_destination_connection_id.c
    plugins/multipath/get_incoming_path.c
    plugins/multipath/parse_add_address_frame.c
    plugins/multipath/parse_mp_ack_frame.c
    plugins/multipath/parse_mp_new_connection_id_frame.c
    plugins/multipath/predict_packet_header_length.c
    plugins/multipath/process_mp_ack_frame.c
    plugins/multipath/process_mp_new_connection_id_frame.c
    plugins/multipath/process_possible_ack_of_ack_frame.c
    plugins/multipath/set_next_wake_time.c
    plugins/multipath/write_add_address_frame.c
    plugins/multipath/write_mp_ack_frame.c
    plugins/multipath/write_mp_new_connection_id_frame.c
    plugins/multipath/write_rtt_probe.c
    plugins/multipath/path_schedulers/schedule_path_rr.c
    plugins/multipath/path_schedulers/schedule_path_rtt.c
    plugins/multipath/path_schedulers/schedule_frames.c
    plugins/multipath/qlog/mp_ack_frame_parsed.c
    plugins/multipath/qlog/mp_new_connection_id_frame_parsed.c
    plugins/multipath/qlog/add_address_parsed.c
    plugins/multipath/path_manager.c
    plugins/multipath/addresses_filters/no_v6_no_10.c
    plugins/multipath/update_rtt.c
    plugins/multipath/parse_uniflows_frame.c
    plugins/multipath/process_uniflows_frame.c
    plugins/multipath/write_uniflows_frame.c
    plugins/multipath/notify_uniflows_frame.c
    plugins/multipath/peer_address_changed.c
        plugins/multipath/qlog/uniflows_parsed.c)

SET(PLUGINS_STREAM_SCHEDULER
    plugins/stream_scheduling_rr/stream_scheduling_rr.c
)

SET(PLUGINS_QLOG
    plugins/qlog/cnx_state_changed.c
    plugins/qlog/set_output_file.c
    plugins/qlog/log_event.c
    plugins/qlog/frames/stream_opened.c
    plugins/qlog/frames/stream_flags_changed.c
    plugins/qlog/frames/reset_stream_frame_parsed.c
    plugins/qlog/frames/new_connection_id_parsed.c
    plugins/qlog/frames/new_token_frame_parsed.c
    plugins/qlog/frames/stop_sending_parsed.c
    plugins/qlog/frames/crypto_frame_parsed.c
    plugins/qlog/frames/ack_frame_parsed.c
    plugins/qlog/frames/is_ack_needed.c
    plugins/qlog/frames/connection_close_parsed.c
    plugins/qlog/frames/application_close_parsed.c
    plugins/qlog/frames/max_data_parsed.c
    plugins/qlog/frames/max_stream_data_parsed.c
    plugins/qlog/frames/path_challenge_created.c
    plugins/qlog/frames/path_response_parsed.c
    plugins/qlog/frames/blocked_frame_parsed.c
    plugins/qlog/frames/stream_blocked_frame_parsed.c
    plugins/qlog/frames/stream_id_blocked_frame_parsed.c
    plugins/qlog/frames/padding_or_ping_parsed.c
    plugins/qlog/frames/frame_prepared.c
    plugins/qlog/sender/retransmit_needed_by_packet.c
    plugins/qlog/sender/retransmit_needed.c
    plugins/qlog/sender/congestion_algorithm_notified.c
    plugins/qlog/sender/ready_stream.c
    plugins/qlog/sender/next_stream_scheduled.c
    plugins/qlog/sender/next_wake_time.c
    plugins/qlog/sender/header_prepared.c
        plugins/qlog/sender/segment_prepared.c
    plugins/qlog/push_log_context.c
    plugins/qlog/pop_log_context.c
    plugins/qlog/log_frame.c
    plugins/qlog/receiver/header_parsed.c
        plugins/qlog/receiver/segment_received.c
        plugins/qlog/frames/stream_frame_parsed.c
        plugins/qlog/frames/handshake_done_parsed.c
        plugins/qlog/sender/path_bandwidth_estimated.c)

SET(PLUGINS_ECN
    plugins/ecn/header_parsed.c
    plugins/ecn/before_sending_packet.c
    plugins/ecn/received_packet.c
    plugins/ecn/write_ecn_block.c
    plugins/ecn/parse_ecn_block.c
    plugins/ecn/process_ecn_block.c
        plugins/ecn/pre_skip_frame.c plugins/ecn/post_skip_frame.c)

FIND_LIBRARY(PTLS_CORE picotls-core PATH ../../../picotls)
MESSAGE(STATUS "Found picotls-core at : ${PTLS_CORE} " )

FIND_LIBRARY(PTLS_MINICRYPTO picotls-minicrypto PATH ../../../picotls)
MESSAGE(STATUS "Found picotls-crypto at : ${PTLS_MINICRYPTO} " )

FIND_LIBRARY(PTLS_OPENSSL picotls-openssl PATH ../../../picotls)
MESSAGE(STATUS "Found picotls-openssl at : ${PTLS_OPENSSL} " )

FIND_LIBRARY(UBPF ubpf PATH ubpf/vm)
MESSAGE(STATUS "Found ubpf at : ${UBPF} " )

FIND_PACKAGE(OpenSSL )
MESSAGE("root: ${OPENSSL_ROOT_DIR}")
MESSAGE("OpenSSL_VERSION: ${OPENSSL_VERSION}")
MESSAGE("OpenSSL_INCLUDE_DIR: ${OPENSSL_INCLUDE_DIR}")
MESSAGE("OpenSSL_LIBRARIES: ${OPENSSL_LIBRARIES}")
INCLUDE_DIRECTORIES(${OPENSSL_INCLUDE_DIR})

FIND_PACKAGE(LibArchive)
MESSAGE("root: ${LibArchive_FOUND}")
MESSAGE("libarchive_version: ${LibArchive_VERSION}")
MESSAGE("libarchive include dirs: ${LibArchive_INCLUDE_DIRS}")
MESSAGE("libarchive_LIBRARIES: ${LibArchive_LIBRARIES}")
INCLUDE_DIRECTORIES(${LibArchive_INCLUDE_DIRS})

ADD_LIBRARY(picohttp-core
    ${PICOHTTP_LIBRARY_FILES}
 )

ADD_LIBRARY(picoquic-core
    ${PICOQUIC_LIBRARY_FILES}
 )



 target_link_libraries(picoquic-core PUBLIC
    ${PTLS_CORE}
    ${PTLS_OPENSSL}
    ${PTLS_MINICRYPTO}
    ${OPENSSL_LIBRARIES}
    ${UBPF}
    ${CMAKE_DL_LIBS}
    ${LibArchive_LIBRARIES}
    ${MICHELFRALLOC_STATIC_LIBS}
)
target_link_libraries(picohttp-core PUBLIC 
    ${PTLS_CORE}
    ${PTLS_OPENSSL}
    ${PTLS_MINICRYPTO}
    ${OPENSSL_LIBRARIES}
    ${UBPF}
    ${CMAKE_DL_LIBS}
    ${LibArchive_LIBRARIES}
    ${MICHELFRALLOC_STATIC_LIBS}  
)

# They add lot of noise at compile time without actually compiling them...
if($ENV{COMPILE_CLION})
    ADD_LIBRARY(plugins-monitoring
        ${PLUGINS_MONITORING}
    )

    ADD_LIBRARY(plugins-multipath
        ${PLUGINS_MULTIPATH}
    )
    ADD_LIBRARY(plugins-datagram
        ${PLUGINS_DATAGRAM}
    )
    ADD_LIBRARY(plugins-stream-scheduler
        ${PLUGINS_STREAM_SCHEDULER}
    )
    ADD_LIBRARY(plugins-qlog
        ${PLUGINS_QLOG}
    )
    ADD_LIBRARY(plugins-ecn
        ${PLUGINS_ECN}
    )
endif()

if(NOT DEFINED ONLY_LIB)
    ADD_EXECUTABLE(picoquicdemo picoquicfirst/picoquicdemo.c
                                picoquicfirst/getopt.c  )
    TARGET_LINK_LIBRARIES(picoquicdemo
        picohttp-core
        picoquic-core      
    )

    target_include_directories(picoquicdemo PRIVATE picohttp)

    #ADD_EXECUTABLE(psquicdemo picoquicfirst/psquicdemo.c
    #                            picoquicfirst/getopt.c  )
    #TARGET_LINK_LIBRARIES(psquicdemo
    #    picohttp-core
    #    picoquic-core      
    #)

    #target_include_directories(psquicdemo PRIVATE picohttp)

    #ADD_EXECUTABLE(picoquicvpn picoquicfirst/picoquicvpn.c
    #                            picoquicfirst/getopt.c  )
    #TARGET_LINK_LIBRARIES(picoquicvpn picoquic-core
    #    ${PTLS_CORE}
    #    ${PTLS_OPENSSL}
    #    ${PTLS_MINICRYPTO}
    #    ${OPENSSL_LIBRARIES}
    #    ${UBPF}
    #    ${CMAKE_DL_LIBS}
    #    ${LibArchive_LIBRARIES}
    #    ${MICHELFRALLOC_STATIC_LIBS}
    #)

    #ADD_EXECUTABLE(picoquicdemobench picoquicfirst/picoquicdemobench.c
    #                            picoquicfirst/getopt.c  )
    #TARGET_LINK_LIBRARIES(picoquicdemobench picoquic-core
    #    ${PTLS_CORE}
    #    ${PTLS_OPENSSL}
   #     ${PTLS_MINICRYPTO}
    #    ${OPENSSL_LIBRARIES}
   #     ${UBPF}
   #     ${CMAKE_DL_LIBS}
  #      ${LibArchive_LIBRARIES}
   #     ${MICHELFRALLOC_STATIC_LIBS}
   # )

   # ADD_EXECUTABLE(picoquic_ct picoquic_t/picoquic_t.c
   #  ${PICOQUIC_TEST_LIBRARY_FILES}  )
   # TARGET_LINK_LIBRARIES(picoquic_ct picoquic-core
  #      ${PTLS_CORE}
   #     ${PTLS_OPENSSL}
   #     ${PTLS_MINICRYPTO}
   #     ${PTLS_OPENSSL}
   #     ${PTLS_CORE}
  #      ${OPENSSL_LIBRARIES}
   #     ${UBPF}
   #     ${CMAKE_DL_LIBS}
   #     ${LibArchive_LIBRARIES}
   #     ${MICHELFRALLOC_STATIC_LIBS}
 #   )

  #  SET(TEST_EXES picoquic_ct)
endif()

# get all project files for formatting
file(GLOB_RECURSE CLANG_FORMAT_SOURCE_FILES *.c *.h)

# Adds clangformat as target that formats all source files
add_custom_target(
    clangformat
    COMMAND clang-format
    -style=Webkit
    -i
    ${CLANG_FORMAT_SOURCE_FILES}
)
