/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef PICOQUIC_H
#define PICOQUIC_H

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "protoop.h"
#include "queue.h"
#ifdef _WINDOWS
#include <WS2tcpip.h>
#include <Ws2def.h>
#include <winsock2.h>
#else
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PICOQUIC_ERROR_CLASS 0x400
#define PICOQUIC_ERROR_DUPLICATE (PICOQUIC_ERROR_CLASS + 1)
#define PICOQUIC_ERROR_FNV1A_CHECK (PICOQUIC_ERROR_CLASS + 2)
#define PICOQUIC_ERROR_AEAD_CHECK (PICOQUIC_ERROR_CLASS + 3)
#define PICOQUIC_ERROR_UNEXPECTED_PACKET (PICOQUIC_ERROR_CLASS + 4)
#define PICOQUIC_ERROR_MEMORY (PICOQUIC_ERROR_CLASS + 5)
#define PICOQUIC_ERROR_SPURIOUS_REPEAT (PICOQUIC_ERROR_CLASS + 6)
#define PICOQUIC_ERROR_CNXID_CHECK (PICOQUIC_ERROR_CLASS + 7)
#define PICOQUIC_ERROR_INITIAL_TOO_SHORT (PICOQUIC_ERROR_CLASS + 8)
#define PICOQUIC_ERROR_VERSION_NEGOTIATION_SPOOFED (PICOQUIC_ERROR_CLASS + 9)
#define PICOQUIC_ERROR_MALFORMED_TRANSPORT_EXTENSION (PICOQUIC_ERROR_CLASS + 10)
#define PICOQUIC_ERROR_EXTENSION_BUFFER_TOO_SMALL (PICOQUIC_ERROR_CLASS + 11)
#define PICOQUIC_ERROR_ILLEGAL_TRANSPORT_EXTENSION (PICOQUIC_ERROR_CLASS + 12)
#define PICOQUIC_ERROR_CANNOT_RESET_STREAM_ZERO (PICOQUIC_ERROR_CLASS + 13)
#define PICOQUIC_ERROR_INVALID_STREAM_ID (PICOQUIC_ERROR_CLASS + 14)
#define PICOQUIC_ERROR_STREAM_ALREADY_CLOSED (PICOQUIC_ERROR_CLASS + 15)
#define PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL (PICOQUIC_ERROR_CLASS + 16)
#define PICOQUIC_ERROR_INVALID_FRAME (PICOQUIC_ERROR_CLASS + 17)
#define PICOQUIC_ERROR_CANNOT_CONTROL_STREAM_ZERO (PICOQUIC_ERROR_CLASS + 18)
#define PICOQUIC_ERROR_RETRY (PICOQUIC_ERROR_CLASS + 19)
#define PICOQUIC_ERROR_DISCONNECTED (PICOQUIC_ERROR_CLASS + 20)
#define PICOQUIC_ERROR_DETECTED (PICOQUIC_ERROR_CLASS + 21)
#define PICOQUIC_ERROR_INVALID_TICKET (PICOQUIC_ERROR_CLASS + 23)
#define PICOQUIC_ERROR_INVALID_FILE (PICOQUIC_ERROR_CLASS + 24)
#define PICOQUIC_ERROR_SEND_BUFFER_TOO_SMALL (PICOQUIC_ERROR_CLASS + 25)
#define PICOQUIC_ERROR_UNEXPECTED_STATE (PICOQUIC_ERROR_CLASS + 26)
#define PICOQUIC_ERROR_UNEXPECTED_ERROR (PICOQUIC_ERROR_CLASS + 27)
#define PICOQUIC_ERROR_TLS_SERVER_CON_WITHOUT_CERT (PICOQUIC_ERROR_CLASS + 28)
#define PICOQUIC_ERROR_NO_SUCH_FILE (PICOQUIC_ERROR_CLASS + 29)
#define PICOQUIC_ERROR_STATELESS_RESET (PICOQUIC_ERROR_CLASS + 30)
#define PICOQUIC_ERROR_CONNECTION_DELETED (PICOQUIC_ERROR_CLASS + 31)
#define PICOQUIC_ERROR_CNXID_SEGMENT (PICOQUIC_ERROR_CLASS + 32)
#define PICOQUIC_ERROR_CNXID_NOT_AVAILABLE (PICOQUIC_ERROR_CLASS + 33)
#define PICOQUIC_ERROR_MIGRATION_DISABLED (PICOQUIC_ERROR_CLASS + 34)
#define PICOQUIC_ERROR_CANNOT_COMPUTE_KEY (PICOQUIC_ERROR_CLASS + 35)
#define PICOQUIC_ERROR_CANNOT_SET_ACTIVE_STREAM (PICOQUIC_ERROR_CLASS + 36)
#define PICOQUIC_ERROR_PROTOCOL_OPERATION_TOO_MANY_ARGUMENTS (PICOQUIC_ERROR_CLASS + 40)
#define PICOQUIC_ERROR_PROTOCOL_OPERATION_UNEXEPECTED_ARGC (PICOQUIC_ERROR_CLASS + 41)
#define PICOQUIC_ERROR_INVALID_PLUGIN_STREAM_ID (PICOQUIC_ERROR_CLASS + 42)
#define PICOQUIC_ERROR_NO_ALPN_PROVIDED (PICOQUIC_ERROR_CLASS + 43)

#define PICOQUIC_MISCCODE_CLASS 0x800
#define PICOQUIC_MISCCODE_RETRY_NXT_PKT (PICOQUIC_MISCCODE_CLASS + 1)
/*
 * Protocol errors defined in the QUIC spec
 */
#define PICOQUIC_TRANSPORT_INTERNAL_ERROR (0x1)
#define PICOQUIC_TRANSPORT_SERVER_BUSY (0x2)
#define PICOQUIC_TRANSPORT_FLOW_CONTROL_ERROR (0x3)
#define PICOQUIC_TRANSPORT_STREAM_LIMIT_ERROR (0x4)
#define PICOQUIC_TRANSPORT_STREAM_STATE_ERROR (0x5)
#define PICOQUIC_TRANSPORT_FINAL_SIZE_ERROR (0x6)
#define PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR (0x7)
#define PICOQUIC_TRANSPORT_PARAMETER_ERROR (0x8)
#define PICOQUIC_TRANSPORT_CONNECTION_ID_LIMIT_ERROR (0x9)
#define PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION (0xA)
#define PICOQUIC_TRANSPORT_INVALID_TOKEN (0xB)
#define PICOQUIC_TRANSPORT_CRYPTO_ERROR(Alert) (((uint64_t)0x100) | ((uint64_t)((Alert)&0xFF)))
#define PICOQUIC_TLS_HANDSHAKE_FAILED (0x201)
#define PICOQUIC_TLS_FATAL_ALERT_GENERATED (0x202)
#define PICOQUIC_TLS_FATAL_ALERT_RECEIVED (0x203)

#define PICOQUIC_MAX_PACKET_SIZE 1536
#define PICOQUIC_RESET_SECRET_SIZE 16
#define PICOQUIC_RESET_PACKET_MIN_SIZE (1 + 20 + 16)

/*
 * Efficient range operations that assume range containing bitfields.
 * Namely, it assumes max&min==min, min&bits==0, max&bits==bits.
 */
#define PICOQUIC_IN_RANGE(v, min, max)                  (((v) & ~((min)^(max))) == (min))
// Is v between min and max and has all given bits set/clear?
#define PICOQUIC_BITS_SET_IN_RANGE(  v, min, max, bits) (((v) & ~((min)^(max)^(bits))) == ((min)^(bits)))
#define PICOQUIC_BITS_CLEAR_IN_RANGE(v, min, max, bits) (((v) & ~((min)^(max)^(bits))) == (min))

/*
 * Types of frames
 */
typedef enum {
    picoquic_frame_type_padding = 0x00,
    picoquic_frame_type_ping = 0x01,
    picoquic_frame_type_ack = 0x02,
    picoquic_frame_type_ack_ecn = 0x03,
    picoquic_frame_type_reset_stream = 0x04,
    picoquic_frame_type_stop_sending = 0x05,
    picoquic_frame_type_crypto_hs = 0x06,
    picoquic_frame_type_new_token = 0x07,
    picoquic_frame_type_stream_range_min = 0x08,
    picoquic_frame_type_stream_range_max = 0x0f,
    picoquic_frame_type_max_data = 0x10,
    picoquic_frame_type_max_stream_data = 0x11,
    picoquic_frame_type_max_streams_bidi = 0x12, // TODO send those frames
    picoquic_frame_type_max_streams_uni = 0x13, // TODO send those frames
    picoquic_frame_type_data_blocked = 0x14,
    picoquic_frame_type_stream_data_blocked = 0x15,
    picoquic_frame_type_bidi_streams_blocked = 0x16,
    picoquic_frame_type_uni_streams_blocked = 0x17,
    picoquic_frame_type_new_connection_id = 0x18, // TODO update
    picoquic_frame_type_retire_connection_id = 0x19, // TODO implement
    picoquic_frame_type_path_challenge = 0x1a,
    picoquic_frame_type_path_response = 0x1b,
    picoquic_frame_type_connection_close = 0x1c, // TODO merge
    picoquic_frame_type_application_close = 0x1d, // TODO merge
    picoquic_frame_type_handshake_done = 0x1e,
    picoquic_frame_type_plugin_validate = 0x30,
    picoquic_frame_type_plugin = 0x31
} picoquic_frame_type_enum_t;

/*
* Connection states, useful to expose the state to the application.
*/
typedef enum {
    picoquic_state_client_init,
    picoquic_state_client_init_sent,
    picoquic_state_client_renegotiate,
    picoquic_state_client_retry_received,
    picoquic_state_client_init_resent,
    picoquic_state_server_init,
    picoquic_state_server_handshake,
    picoquic_state_client_handshake_start,
    picoquic_state_client_handshake_progress,
    picoquic_state_handshake_failure,
    picoquic_state_client_almost_ready,
    picoquic_state_server_almost_ready,
    picoquic_state_client_ready,
    picoquic_state_server_ready,
    picoquic_state_disconnecting,
    picoquic_state_closing_received,
    picoquic_state_closing,
    picoquic_state_draining,
    picoquic_state_disconnected
} picoquic_state_enum;


/*
* Quic context flags
*/
typedef enum {
    picoquic_context_check_token = 1,
    picoquic_context_unconditional_cnx_id = 2,
    picoquic_context_client_zero_share = 4
} picoquic_context_flags;

/*
 * Provisional definition of the connection ID.
 */
#define PICOQUIC_CONNECTION_ID_MAX_SIZE 20

typedef struct st_picoquic_connection_id_t {
    uint8_t id[PICOQUIC_CONNECTION_ID_MAX_SIZE];
    uint8_t id_len;
} picoquic_connection_id_t;

/* Detect whether error occured in TLS
 */
int picoquic_is_handshake_error(uint64_t error_code);
/*
* The stateless packet structure is used to temporarily store
* stateless packets before they can be sent by servers.
*/

typedef struct st_picoquic_stateless_packet_t {
    struct st_picoquic_stateless_packet_t* next_packet;
    struct sockaddr_storage addr_to;
    struct sockaddr_storage addr_local;
    unsigned long if_index_local;
    size_t length;

    uint8_t bytes[PICOQUIC_MAX_PACKET_SIZE];
} picoquic_stateless_packet_t;

/*
* Nominal packet types. These are the packet types used internally by the
* implementation. The wire encoding depends on the version.
*/
typedef enum {
    picoquic_packet_error = 0,
    picoquic_packet_version_negotiation,
    picoquic_packet_initial,
    picoquic_packet_retry,
    picoquic_packet_handshake,
    picoquic_packet_0rtt_protected,
    picoquic_packet_1rtt_protected_phi0,
    picoquic_packet_1rtt_protected_phi1,
    picoquic_packet_type_max
} picoquic_packet_type_enum;

typedef enum {
    picoquic_long_packet_type_initial = 0x0,
    picoquic_long_packet_type_0rtt = 0x1,
    picoquic_long_packet_type_handshake = 0x2,
    picoquic_long_packet_type_retry = 0x3,
} picoquic_long_packet_type;

typedef enum {
    picoquic_packet_context_application = 0,
    picoquic_packet_context_handshake = 1,
    picoquic_packet_context_initial = 2,
    picoquic_nb_packet_context = 3
} picoquic_packet_context_enum;

typedef struct protoop_plugin protoop_plugin_t;
typedef struct st_plugin_struct_metadata plugin_struct_metadata_t;

/* This structure is used for sending booking purposes */
typedef struct reserve_frame_slot {
    size_t nb_bytes;
    uint8_t is_congestion_controlled:1;
    bool low_priority:1;
    uint64_t frame_type;
    protoop_plugin_t *p; /* Whathever you place here, it will be overwritten */
    /* TODO FIXME position */
    void *frame_ctx;
} reserve_frame_slot_t;

typedef struct reserve_frames_block {
    size_t total_bytes;
    uint8_t nb_frames;
    uint8_t is_congestion_controlled:1;
    bool low_priority:1; // if false, picoquic will wake as soon as it is reserved
    /* The following pointer is an array! */
    reserve_frame_slot_t *frames;
} reserve_frames_block_t;

typedef struct st_picoquic_packet_plugin_frame_t {
    struct st_picoquic_packet_plugin_frame_t* next;
    protoop_plugin_t* plugin;
    size_t frame_offset; /* Offset of the start of the frame in the packet payload */
    uint64_t bytes;
    reserve_frame_slot_t *rfs; /* Memory held by the plugin, responsible for its my_free */
} picoquic_packet_plugin_frame_t;

/*
 * The simple packet structure is used to store packets that
 * have been sent but are not yet acknowledged.
 * Packets are stored in unencrypted format.
 * The checksum length is the difference between encrypted and unencrypted.
 */

typedef struct st_picoquic_packet_t {
    struct st_picoquic_packet_t* previous_packet;
    struct st_picoquic_packet_t* next_packet;
    struct st_picoquic_path_t * send_path;
    uint64_t sequence_number;
    uint64_t send_time;
    uint64_t delivered_prior;
    uint64_t delivered_time_prior;
    uint64_t delivered_sent_prior;
    uint32_t length;
    uint32_t send_length;
    uint32_t checksum_overhead;
    uint32_t offset;
    picoquic_packet_type_enum ptype;
    picoquic_packet_context_enum pc;
    unsigned int is_pure_ack : 1;
    unsigned int contains_crypto : 1;
    unsigned int is_congestion_controlled : 1;  // This flag can be set independently of the is_evaluated flag, but either before or at the same time.
    unsigned int has_plugin_frames : 1;
    unsigned int is_mtu_probe : 1;
    unsigned int delivered_app_limited : 1;
    unsigned int has_handshake_done : 1;

    picoquic_packet_plugin_frame_t *plugin_frames; /* Track plugin bytes */

    plugin_struct_metadata_t *metadata;

    uint8_t bytes[PICOQUIC_MAX_PACKET_SIZE];
} picoquic_packet_t;

typedef struct st_picoquic_quic_t picoquic_quic_t;
typedef struct st_picoquic_cnx_t picoquic_cnx_t;
typedef struct st_picoquic_path_t picoquic_path_t;
typedef struct st_picoquic_packet_context_t picoquic_packet_context_t;
typedef struct st_picoquic_sack_item_t picoquic_sack_item_t;
typedef struct _picoquic_stream_head picoquic_stream_head;
typedef struct st_picoquic_crypto_context_t picoquic_crypto_context_t;
typedef struct _picoquic_packet_header picoquic_packet_header;
typedef struct st_picoquic_tp_t picoquic_tp_t;
typedef struct _picoquic_stream_data picoquic_stream_data;

typedef struct st_plugin_req_pid_t plugin_req_pid_t;

typedef struct st_protocol_operation_struct_t protocol_operation_struct_t;

typedef uint64_t protoop_arg_t;
typedef struct protoop_plugin protoop_plugin_t;
typedef uint16_t param_id_t;
typedef uint16_t opaque_id_t;

typedef struct {
    protoop_id_t *pid;//协议操作的id，比如PROTOOP_NOPARAM_PREPARE_PACKET_READY等，可认为是标明需要干啥的或者当前状态的标志
    param_id_t param;//参数id
    bool caller_is_intern;
    int inputc;//传入的可变参数数目
    protoop_arg_t *inputv;//指向存放可变参数数组的第一个元素
    protoop_arg_t *outputv;//可变参数数组，暂定
} protoop_params_t;

#define NO_PARAM (param_id_t) -1
#define PROTOOPARGS_MAX 16 /* Minimum required value... */

/**
 * Frame structures
 */
#define REASONPHRASELENGTH_MAX 200

typedef struct padding_or_ping_frame {
    int is_ping;
    int num_block; /** How many consecutive frames? */
} padding_or_ping_frame_t;

typedef struct reset_stream_frame {
    uint64_t stream_id;
    uint64_t app_error_code;
    uint64_t final_offset;
} reset_stream_frame_t;

typedef struct connection_close_frame {
    uint64_t error_code;
    uint64_t frame_type;
    uint64_t reason_phrase_length;
    /** \todo Remove fix-length char */
    char reason_phrase[REASONPHRASELENGTH_MAX];
} connection_close_frame_t;

typedef struct application_close_frame {
    uint64_t error_code;
    uint64_t reason_phrase_length;
    /** \todo Remove fix-length char */
    char reason_phrase[REASONPHRASELENGTH_MAX];
} application_close_frame_t;

typedef struct max_data_frame {
    uint64_t maximum_data;
} max_data_frame_t;

typedef struct max_stream_data_frame {
    uint64_t stream_id;
    uint64_t maximum_stream_data;
} max_stream_data_frame_t;

typedef struct max_streams_frame {
    bool uni;
    uint64_t maximum_streams;
} max_streams_frame_t;

typedef struct blocked_frame {
    uint64_t offset;
} blocked_frame_t;

typedef struct stream_blocked_frame {
    uint64_t stream_id;
    uint64_t offset;
} stream_blocked_frame_t;

typedef struct stream_id_blocked_frame {
    bool uni;
    uint64_t stream_limit;
} streams_blocked_frame_t;

typedef struct new_connection_id_frame {
    uint64_t sequence;
    uint64_t retire_prior_to;
    picoquic_connection_id_t connection_id;
    uint8_t stateless_reset_token[16];
} new_connection_id_frame_t;

typedef struct retire_connection_id_frame {
    uint64_t sequence;
} retire_connection_id_frame_t;

typedef struct stop_sending_frame {
    uint64_t stream_id;
    uint64_t application_error_code;
} stop_sending_frame_t;

typedef struct ack_block {
    uint64_t gap;
    uint64_t additional_ack_block;
} ack_block_t;

typedef struct ack_frame {
    uint8_t is_ack_ecn;
    uint64_t largest_acknowledged;
    uint64_t ack_delay;
    uint64_t ecnx3[3];
    void *ecn_block;
    /** \todo Fixme we do not support ACK frames with more than 63 ack blocks */
    uint64_t ack_block_count;
    uint64_t first_ack_block;
    ack_block_t ack_blocks[63];
} ack_frame_t;

typedef struct path_challenge_frame {
    uint8_t data[8];
} path_challenge_frame_t;

typedef struct path_response_frame {
    uint64_t data;
} path_response_frame_t;

typedef struct stream_frame {
    uint64_t stream_id;
    size_t   data_length;
    uint64_t offset;
    int      fin;
    size_t   consumed;
    uint8_t  *data_ptr;
} stream_frame_t;

typedef struct crypto_frame {
    uint64_t offset;
    uint64_t length;
    uint8_t* crypto_data_ptr; /* Start of the data, not contained in the structure */
} crypto_frame_t;

typedef struct new_token_frame {
    uint64_t token_length;
    uint8_t* token_ptr; /* Start of the data, not contained in the structure */
} new_token_frame_t;

typedef uint8_t hanshake_done_frame_t;

typedef struct plugin_validate_frame {
    uint64_t pid_id;
    uint64_t pid_len;
    char *pid;
} plugin_validate_frame_t;

typedef struct plugin_frame {
    uint8_t fin;
    uint64_t pid_id;
    uint64_t offset;
    uint64_t length;
    uint8_t data[1500];
} plugin_frame_t;


typedef struct st_plugin_fname_t {
    char* plugin_name;
    char* plugin_path;
    bool require_negotiation;
} plugin_fname_t;


typedef enum {
    picoquic_callback_no_event = 0, /* Data received from peer on stream N */
    picoquic_callback_stream_fin, /* Fin received from peer on stream N; data is optional */
    picoquic_callback_stream_reset, /* Reset Stream received from peer on stream N; bytes=NULL, len = 0  */
    picoquic_callback_stop_sending, /* Stop sending received from peer on stream N; bytes=NULL, len = 0 */
    picoquic_callback_stateless_reset, /* Stateless reset received from peer. Stream=0, bytes=NULL, len=0 */
    picoquic_callback_close, /* Connection close. Stream=0, bytes=NULL, len=0 */
    picoquic_callback_application_close, /* Application closed by peer. Stream=0, bytes=NULL, len=0 */
    picoquic_callback_stream_gap,  /* bytes=NULL, len = length-of-gap or 0 (if unknown) */
    picoquic_callback_challenge_response,
    picoquic_callback_prepare_to_send, /* Ask application to send data in frame, see picoquic_provide_stream_data_buffer for details */
    picoquic_callback_almost_ready, /* Data can be sent, but the connection is not fully established */
    picoquic_callback_ready, /* Data can be sent and received, connection migration can be initiated */
    picoquic_callback_request_alpn_list, /* Provide the list of supported ALPN */
    picoquic_callback_set_alpn, /* Set ALPN to negotiated value */
} picoquic_call_back_event_t;

typedef struct plugin_stat {
    char *protoop_name;
    char *pluglet_name;
    bool pre, replace, post, is_param;
    param_id_t param;
    uint64_t count;
    uint64_t total_execution_time;
} plugin_stat_t;
#define PICOQUIC_STREAM_ID_TYPE_MASK 3
#define PICOQUIC_STREAM_ID_CLIENT_INITIATED 0
#define PICOQUIC_STREAM_ID_SERVER_INITIATED 1
#define PICOQUIC_STREAM_ID_BIDIR 0
#define PICOQUIC_STREAM_ID_UNIDIR 2
#define PICOQUIC_STREAM_ID_CLIENT_INITIATED_BIDIR (PICOQUIC_STREAM_ID_CLIENT_INITIATED|PICOQUIC_STREAM_ID_BIDIR)
#define PICOQUIC_STREAM_ID_SERVER_INITIATED_BIDIR (PICOQUIC_STREAM_ID_SERVER_INITIATED|PICOQUIC_STREAM_ID_BIDIR)
#define PICOQUIC_STREAM_ID_CLIENT_INITIATED_UNIDIR (PICOQUIC_STREAM_ID_CLIENT_INITIATED|PICOQUIC_STREAM_ID_UNIDIR)
#define PICOQUIC_STREAM_ID_SERVER_INITIATED_UNIDIR (PICOQUIC_STREAM_ID_SERVER_INITIATED|PICOQUIC_STREAM_ID_UNIDIR)

#define PICOQUIC_STREAM_ID_CLIENT_MAX_INITIAL_BIDIR (PICOQUIC_STREAM_ID_CLIENT_INITIATED_BIDIR + ((65535-1)*4))
#define PICOQUIC_STREAM_ID_SERVER_MAX_INITIAL_BIDIR (PICOQUIC_STREAM_ID_SERVER_INITIATED_BIDIR + ((65535-1)*4))
#define PICOQUIC_STREAM_ID_CLIENT_MAX_INITIAL_UNIDIR (PICOQUIC_STREAM_ID_CLIENT_INITIATED_UNIDIR + ((65535-1)*4))
#define PICOQUIC_STREAM_ID_SERVER_MAX_INITIAL_UNIDIR (PICOQUIC_STREAM_ID_SERVER_INITIATED_UNIDIR + ((65535-1)*4))

/*
* Time management. Internally, picoquic works in "virtual time", updated via the "current time" parameter
* passed through picoquic_create(), picoquic_create_cnx(), picoquic_incoming_packet(), and picoquic_prepare_packet().
*
* There are two supported modes of operation, "wall time" synchronized with the system's current time function,
* and "simulated time". Production services are expected to use wall time, tests and simulation use the
* simulated time. The simulated time is held in a 64 bit counter, the address of which is passed as
* the "p_simulated_time" parameter to picoquic_create().
*
* The time management needs to be consistent with the functions used internally by the TLS package "picotls".
* If the argument "p_simulated_time" is NULL, picotls will use "wall time", accessed through system API.
* If the argument is set, the default time function of picotls will be overridden by a function that
* reads the value of *p_simulated_time.
*
* The function "picoquic_current_time()" reads the wall time in microseconds, using the same system calls
* as picotls. The default socket code in "picosock.[ch]" uses that time function, and returns the time
* at which messages arrived.
*
* The function "picoquic_get_quic_time()" returns the "virtual time" used by the specified quic
* context, which can be either the current wall time or the simulated time, depending on how the
* quic context was initialized.
*/

uint64_t picoquic_current_time(); /* wall time */
uint64_t picoquic_get_quic_time(picoquic_quic_t* quic); /* connection time, compatible with simulations */


/* Callback function for providing stream data to the application.
     * If stream_id is zero, this delivers changes in
     * connection state.
     */
typedef int (*picoquic_stream_data_cb_fn)(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx);

typedef void (*cnx_id_cb_fn)(picoquic_connection_id_t cnx_id_local,
    picoquic_connection_id_t cnx_id_remote, void* cnx_id_cb_data, picoquic_connection_id_t * cnx_id_returned);

#ifndef picotls_h
/* Including picotls.h here complicates the plugins compilation, so better define this simple structure here */
typedef struct st_ptls_iovec_t {
    uint8_t *base;
    size_t len;
} ptls_iovec_t;
#endif
/* Callback from the TLS stack upon receiving a list of proposed ALPN in the Client Hello
 * The stack passes a <list> of io <count> vectors (base, len) each containing a proposed
 * ALPN. The implementation returns the index of the selected ALPN, or a value >= count
 * if none of the proposed ALPN is supported.
 *
 * The callback is only called if no default ALPN is specified in the Quic context.
 */
typedef size_t (*picoquic_alpn_select_fn)(picoquic_quic_t* quic, ptls_iovec_t* list, size_t count);

/*如果quic context结构体中的default alpn不为空，则释放这个参数的内存，并将其置为NULL，同时将alp_select_fn(这是一个用于回调的函数指针)
设置为传入的函数，进行回调*/
void picoquic_set_alpn_select_fn(picoquic_quic_t* quic, picoquic_alpn_select_fn alpn_select_fn);

/* Function used during callback to provision an ALPN context. The stack
 * issues a callback of type
 */
int picoquic_add_proposed_alpn(void* tls_context, const char* alpn);

/* The fuzzer function is used to inject error in packets randomly.
 * It is called just prior to sending a packet, and can randomly
 * change the content or length of the packet.
 */
typedef uint32_t(*picoquic_fuzz_fn)(void * fuzz_ctx, picoquic_cnx_t* cnx, uint8_t * bytes,
    size_t bytes_max, size_t length, uint32_t header_length);
void picoquic_set_fuzz(picoquic_quic_t* quic, picoquic_fuzz_fn fuzz_fn, void * fuzz_ctx);

/* Will be called to verify that the given data corresponds to the given signature.
 * This callback and the `verify_ctx` will be set by the `verify_certificate_cb_fn`.
 * If `data` and `sign` are empty buffers, an error occurred and `verify_ctx` should be freed.
 * Expect `0` as return value, when the data matches the signature.
 */
typedef struct st_ptls_iovec_t ptls_iovec_t; /* forward definition to avoid full dependency on picotls.h */
typedef int (*picoquic_verify_sign_cb_fn)(void* verify_ctx, ptls_iovec_t data, ptls_iovec_t sign);
/* Will be called to verify a certificate of a connection.
 * The arguments `verify_sign` and `verify_sign_ctx` are expected to be set, when the function returns `0`.
 * See `verify_sign_cb_fn` for more information about these arguments.
 */
typedef int (*picoquic_verify_certificate_cb_fn)(void* ctx, picoquic_cnx_t* cnx, ptls_iovec_t* certs, size_t num_certs,
                                                 picoquic_verify_sign_cb_fn* verify_sign, void** verify_sign_ctx);

/* Is called to free the verify certificate ctx */
typedef void (*picoquic_free_verify_certificate_ctx)(void* ctx);

/* QUIC context create and dispose */
picoquic_quic_t* picoquic_create(uint32_t nb_connections,
    char const* cert_file_name, char const* key_file_name, char const * cert_root_file_name,
    char const* default_alpn,
    picoquic_stream_data_cb_fn default_callback_fn,
    void* default_callback_ctx,
    cnx_id_cb_fn cnx_id_callback,
    void* cnx_id_callback_ctx,
    uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE],
    uint64_t current_time,
    uint64_t* p_simulated_time,
    char const* ticket_file_name,
    const uint8_t* ticket_encryption_key,
    size_t ticket_encryption_key_length,
    const char* plugin_store_path);

void picoquic_free(picoquic_quic_t* quic);

/* Set the plugins we want to inject */
int picoquic_set_plugins_to_inject(picoquic_quic_t* quic, const char** plugin_fnames, int plugins);

/* Set the local plugins we want to forcefully inject */
int picoquic_set_local_plugins(picoquic_quic_t* quic, const char** plugin_fnames, int plugins);

/* Set the filename where the logging will be printed.
 * If log_fname is NULL, print to stdout.
 * If log_fname is "/dev/null", does not print at all. */
int picoquic_set_log(picoquic_quic_t* quic, const char *log_fname);

/* If the application required plugin insertion, handle the negotiation */
int picoquic_handle_plugin_negotiation(picoquic_cnx_t* cnx);

/* Set cookie mode on QUIC context when under stress */
void picoquic_set_cookie_mode(picoquic_quic_t* quic, int cookie_mode);

/* Set the TLS certificate chain(DER format) for the QUIC context. The context will take ownership over the certs pointer. */
void picoquic_set_tls_certificate_chain(picoquic_quic_t* quic, ptls_iovec_t* certs, size_t count);

/* Set the TLS root certificates (DER format) for the QUIC context. The context will take ownership over the certs pointer.
 * The root certificates will be used to verify the certificate chain of the server and client (with client authentication activated).
 * Returns `0` on success, `-1` on error while loading X509 certificate or `-2` on error while adding a cert to the certificate store.
 */
int picoquic_set_tls_root_certificates(picoquic_quic_t* quic, ptls_iovec_t* certs, size_t count);

/* Tell the TLS stack to not attempt verifying certificates */
void picoquic_set_null_verifier(picoquic_quic_t* quic);

/* Set the TLS private key(DER format) for the QUIC context. The caller is responsible for cleaning up the pointer. */
int picoquic_set_tls_key(picoquic_quic_t* quic, const uint8_t* data, size_t len);

/* Set the verify certificate callback and context. */
int picoquic_set_verify_certificate_callback(picoquic_quic_t* quic, picoquic_verify_certificate_cb_fn cb, void* ctx,
                                             picoquic_free_verify_certificate_ctx free_fn);

/* Set client authentication in TLS (if enabled, client is required to send certificates). */
void picoquic_set_client_authentication(picoquic_quic_t* quic, int client_authentication);

/* Connection context creation and registration */
picoquic_cnx_t* picoquic_create_cnx(picoquic_quic_t* quic,
    picoquic_connection_id_t initial_cnx_id, picoquic_connection_id_t remote_cnx_id,
    struct sockaddr* addr, uint64_t start_time, uint32_t preferred_version,
    char const* sni, char const* alpn, char client_mode);

picoquic_cnx_t* picoquic_create_client_cnx(picoquic_quic_t* quic,
    struct sockaddr* addr, uint64_t start_time, uint32_t preferred_version,
    char const* sni, char const* alpn,
    picoquic_stream_data_cb_fn callback_fn, void* callback_ctx);

int picoquic_start_client_cnx(picoquic_cnx_t* cnx);

/*
 * pre: stats != NULL
 * populates an array containing statistics for each (protoop, pluglet) pair used in this connection
 * When *stats is NULL, the result array will be allocated using malloc(3).
 * When *stats is not NULL, it points to an already existing array allocated with malloc(3) with nmemb slots of
 *  sizeof(plugin_stat_t) bytes. The address stored in *stats before the call to the function must not be used after the
 *  call if it is different from the value stored in *stats after the call to the function.
 * When the array is too small to store all the statistics, it will be reallocated using realloc(3) and the address of
 *  the new array will be stored in *stats
 * RETURN VALUE:
 *  -1 if an error occurred
 *  the number of added statistics elements otherwise
 * In any case, *stats will either be NULL or point to a valid memory address allocated with malloc(3) after a call to
 * this function.
 */
int picoquic_get_plugin_stats(picoquic_cnx_t *cnx, plugin_stat_t **stats, int nmemb);

void picoquic_delete_cnx(picoquic_cnx_t* cnx);

int picoquic_close(picoquic_cnx_t* cnx, uint64_t reason_code);

picoquic_cnx_t* picoquic_get_first_cnx(picoquic_quic_t* quic);
picoquic_cnx_t* picoquic_get_next_cnx(picoquic_cnx_t* cnx);
int64_t picoquic_get_next_wake_delay(picoquic_quic_t* quic,
    uint64_t current_time,
    int64_t delay_max);
picoquic_cnx_t* picoquic_get_earliest_cnx_to_wake(picoquic_quic_t* quic, picoquic_cnx_t** cnx, uint64_t max_wake_time);

picoquic_state_enum picoquic_get_cnx_state(picoquic_cnx_t* cnx);
int picoquic_get_msg_state(picoquic_cnx_t* cnx);
void picoquic_set_msg_state(picoquic_cnx_t* cnx, int done);
void picoquic_reset_msg_size(picoquic_cnx_t* cnx);
void picoquic_set_cnx_state(picoquic_cnx_t* cnx, picoquic_state_enum state);

int picoquic_tls_is_psk_handshake(picoquic_cnx_t* cnx);

/* Needed for bridging */
picoquic_path_t* picoquic_get_connection_path(picoquic_cnx_t* cnx);

void picoquic_get_peer_addr(picoquic_path_t* path_x, struct sockaddr** addr, int* addr_len);
void picoquic_get_local_addr(picoquic_path_t* path_x, struct sockaddr** addr, int* addr_len);
unsigned long picoquic_get_local_if_index(picoquic_path_t* path_x);

picoquic_connection_id_t picoquic_get_local_cnxid(picoquic_cnx_t* cnx);
picoquic_connection_id_t picoquic_get_remote_cnxid(picoquic_cnx_t* cnx);
picoquic_connection_id_t picoquic_get_initial_cnxid(picoquic_cnx_t* cnx);
picoquic_connection_id_t picoquic_get_client_cnxid(picoquic_cnx_t* cnx);
picoquic_connection_id_t picoquic_get_server_cnxid(picoquic_cnx_t* cnx);
picoquic_connection_id_t picoquic_get_logging_cnxid(picoquic_cnx_t* cnx);

uint64_t picoquic_get_cnx_start_time(picoquic_cnx_t* cnx);
uint64_t picoquic_is_0rtt_available(picoquic_cnx_t* cnx);

int picoquic_is_cnx_backlog_empty(picoquic_cnx_t* cnx);

void picoquic_set_callback(picoquic_cnx_t* cnx,
    picoquic_stream_data_cb_fn callback_fn, void* callback_ctx);

void * picoquic_get_callback_context(picoquic_cnx_t* cnx);

uint8_t *picoquic_parse_ecn_block(picoquic_cnx_t* cnx, uint8_t *bytes, const uint8_t *bytes_max, void **ecn_block);
int picoquic_process_ecn_block(picoquic_cnx_t* cnx, void *ecn_block, picoquic_packet_context_t *pkt_ctx, picoquic_path_t *path);
int picoquic_write_ecn_block(picoquic_cnx_t* cnx, uint8_t *bytes, size_t bytes_max, picoquic_packet_context_t *pkt_ctx, size_t *consumed);
/* Send and receive network packets */

picoquic_stateless_packet_t* picoquic_dequeue_stateless_packet(picoquic_quic_t* quic);
void picoquic_delete_stateless_packet(picoquic_stateless_packet_t* sp);

int picoquic_incoming_packet(
    picoquic_quic_t* quic,
    uint8_t* bytes,
    uint32_t length,
    struct sockaddr* addr_from,
    struct sockaddr* addr_to,
    int if_index_to,
    uint64_t current_time,
    int* new_context_created,
    picoquic_connection_id_t* cnx_id);

picoquic_packet_t* picoquic_create_packet(picoquic_cnx_t *cnx);

void picoquic_destroy_packet(picoquic_packet_t *p);

int picoquic_prepare_packet(picoquic_cnx_t* cnx,
    uint64_t current_time, uint8_t* send_buffer, size_t send_buffer_max, size_t* send_length, picoquic_path_t** path);

/* Associate stream with app context */
int picoquic_set_app_stream_ctx(picoquic_cnx_t* cnx,
                                uint64_t stream_id, void* app_stream_ctx);

/* Mark stream as active, or not.
 * If a stream is active, it will be polled for data when the transport
 * is ready to send.
 * Returns an error if data was previously queued on the stream using
 * "picoquic_add_to_stream" and the data has not been sent yet.
 */
int picoquic_mark_active_stream(picoquic_cnx_t* cnx,
    uint64_t stream_id, int is_active, void *app_stream_ctx);

/* If a stream is marked active, the application will receive a callback with
 * event type "picoquic_callback_prepare_to_send" when the transport is ready to
 * send data on a stream. The "length" argument in the call back indicates the
 * largest amount of data that can be sent, and the "bytes" argument points
 * to an opaque context structure. In order to prepare data, the application
 * needs to call "picoquic_provide_stream_data_buffer" with that context
 * pointer, with the number of bytes that it wants to write, with an indication
 * of whether or not the fin of the stream was reached, and also an indication
 * of whether or not the stream is still active. The function
 * returns the pointer to a memory address where to write the byte -- or
 * a NULL pointer in case of error. The application then copies the specified 
 * number of bytes at the provided address, and provide a return code 0 from
 * the callback in case of success, or non zero in case of error.
 */

uint8_t* picoquic_provide_stream_data_buffer(void* context, size_t nb_bytes, int is_fin, int is_still_active);

/* Queue data on a stream, so the transport can send it immediately
 * when ready. The data is copied in an intermediate buffer managed by
 * the transport. Calling this API automatically erases the "active
 * mark" that might have been set by using "picoquic_mark_active_stream".
 */
int picoquic_add_to_stream(picoquic_cnx_t* cnx,
    uint64_t stream_id, const uint8_t* data, size_t length, int set_fin);

/* Same as "picoquic_add_to_stream", but also sets the application stream context.
 * The context is used in call backs, so the application can directly process responses.
 */
int picoquic_add_to_stream_with_ctx(picoquic_cnx_t * cnx, uint64_t stream_id, const uint8_t * data, size_t length, int set_fin, void * app_stream_ctx);

/* Reset a stream, indicating that no more data will be sent on 
 * that stream and that any data currently queued can be abandoned. */
int picoquic_reset_stream(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint64_t local_stream_error);

/* Ask the peer to stop sending on a stream. The peer is expected
 * to reset that stream when receiving the "stop sending" signal. */
int picoquic_stop_sending(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint64_t local_stream_error);

/* send and receive data on plugin frames */
int picoquic_add_to_plugin_stream(picoquic_cnx_t* cnx,
    uint64_t pid_id, const uint8_t* data, size_t length, int set_fin);

/* Congestion algorithm definition */
typedef enum {
    picoquic_congestion_notification_acknowledgement,
    picoquic_congestion_notification_repeat,
    picoquic_congestion_notification_timeout,
    picoquic_congestion_notification_spurious_repeat,
    picoquic_congestion_notification_rtt_measurement,
    picoquic_congestion_notification_cwin_blocked,
    picoquic_congestion_notification_bw_measurement,
    picoquic_congestion_notification_congestion_experienced
} picoquic_congestion_notification_t;

typedef void (*picoquic_congestion_algorithm_init)(picoquic_cnx_t* cnx, picoquic_path_t* path_x);
typedef void (*picoquic_congestion_algorithm_notify)(picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t rtt_measurement,
    uint64_t nb_bytes_acknowledged,
    uint64_t lost_packet_number,
    uint64_t current_time);
typedef void (*picoquic_congestion_algorithm_delete)(picoquic_cnx_t* cnx, picoquic_path_t* path_x);

typedef struct st_picoquic_congestion_algorithm_t {
    uint32_t congestion_algorithm_id;
    picoquic_congestion_algorithm_init alg_init;
    picoquic_congestion_algorithm_notify alg_notify;
    picoquic_congestion_algorithm_delete alg_delete;
} picoquic_congestion_algorithm_t;

extern picoquic_congestion_algorithm_t* picoquic_newreno_algorithm;
extern picoquic_congestion_algorithm_t* picoquic_cubic_algorithm;
extern picoquic_congestion_algorithm_t* picoquic_bbr_algorithm;
extern picoquic_congestion_algorithm_t* picoquic_ibbr_algorithm;

#define PICOQUIC_DEFAULT_CONGESTION_ALGORITHM picoquic_bbr_algorithm;//这一部分指定默认的cc算法

void picoquic_set_default_congestion_algorithm(picoquic_quic_t* quic, picoquic_congestion_algorithm_t const* algo);

void picoquic_set_congestion_algorithm(picoquic_cnx_t* cnx, picoquic_congestion_algorithm_t const* alg);

void picoquic_congestion_algorithm_notify_func(picoquic_cnx_t *cnx, picoquic_path_t* path_x, picoquic_congestion_notification_t notification, uint64_t rtt_measurement,
                                                uint64_t nb_bytes_acknowledged, uint64_t lost_packet_number, uint64_t current_time);

/**
 * Book an occasion to send the frame whose details are given in \p slot.
 * \param[in] cnx The context of the connection
 * \param[in] nb_frames The number of frames concerned by the booking
 * \param[in] slots Information about the frames' booking
 *
 * \return The number of bytes reserved, or 0 if an error occurred
 */
size_t reserve_frames(picoquic_cnx_t* cnx, uint8_t nb_frames, reserve_frame_slot_t* slots);

/**
 * Cancels the reservation at the head of the plugin queue
 * and returns its slots.
 *
 * \param[in] cnx The context of the connection
 * \param[in] nb_frames A pointer to return the number of slots
 * \param[in] congestion_controlled \b iny Do we consider the congestion controlled queue or the non one?
 * \return The slots in the reservation
 */
reserve_frame_slot_t* cancel_head_reservation(picoquic_cnx_t* cnx, uint8_t *nb_frames, int congestion_controlled);

/* For building a basic HTTP 0.9 test server */
int http0dot9_get(uint8_t* command, size_t command_length,
    uint8_t* response, size_t response_max, size_t* response_length);

/* Enables keep alive for a connection.
 * If `interval` is `0`, it is set to `max_idle_timeout / 2`.
 */
void picoquic_enable_keep_alive(picoquic_cnx_t* cnx, uint64_t interval);
/* Disables keep alive for a connection. */
void picoquic_disable_keep_alive(picoquic_cnx_t* cnx);

/* Returns if the given connection is the client. */
int picoquic_is_client(picoquic_cnx_t* cnx);

/* Returns the local error of the given connection context. */
uint64_t picoquic_get_local_error(picoquic_cnx_t* cnx);

/* Returns the remote error of the given connection context. */
uint64_t picoquic_get_remote_error(picoquic_cnx_t* cnx);

/* Create a path */
int picoquic_create_path(picoquic_cnx_t* cnx, uint64_t start_time, struct sockaddr* addr);

/* Check pacing to see whether the next transmission is authorized. If it is not, update the next wait time to reflect pacing. */
int picoquic_is_sending_authorized_by_pacing(picoquic_path_t * path_x, uint64_t current_time, uint64_t * next_time);

/* Reset the pacing data after CWIN is updated */
void picoquic_update_pacing_data(picoquic_path_t * path_x);
void picoquic_update_pacing_rate(picoquic_path_t* path_x, double pacing_rate, uint64_t quantum);

void picoquic_estimate_path_bandwidth(picoquic_cnx_t *cnx, picoquic_path_t* path_x, uint64_t send_time, uint64_t delivered_prior, uint64_t delivered_time_prior, uint64_t delivered_sent_prior,
                                      uint64_t delivery_time, uint64_t current_time, int rs_is_path_limited);

/* Integer formatting functions */
size_t picoquic_varint_decode(const uint8_t* bytes, size_t max_bytes, uint64_t* n64);
size_t picoquic_varint_encode(uint8_t* bytes, size_t max_bytes, uint64_t n64);
size_t picoquic_varint_skip(const uint8_t* bytes);

/* Stream management */
picoquic_stream_head* picoquic_find_stream(picoquic_cnx_t* cnx, uint64_t stream_id, int create);

/* Plugin stream management */
picoquic_stream_head* picoquic_find_plugin_stream(picoquic_cnx_t* cnx, uint64_t pid_id, int create);
picoquic_stream_head* picoquic_find_or_create_plugin_stream(picoquic_cnx_t* cnx, uint64_t pid_id, int is_remote);

/* Utilities */
int picoquic_getaddrs(struct sockaddr_storage *sas, uint32_t *if_indexes, int sas_length);
int picoquic_compare_connection_id(picoquic_connection_id_t * cnx_id1, picoquic_connection_id_t * cnx_id2);
uint8_t* picoquic_frames_varint_decode(uint8_t* bytes, const uint8_t* bytes_max, uint64_t* n64);

void picoquic_reinsert_cnx_by_wake_time(picoquic_cnx_t* cnx, uint64_t next_time);

bool picoquic_has_booked_plugin_frames(picoquic_cnx_t *cnx);

size_t picoquic_frame_fair_reserve(picoquic_cnx_t *cnx, picoquic_path_t *path_x, picoquic_stream_head* stream, uint64_t frame_mss);


int picoquic_decode_frames_without_current_time(picoquic_cnx_t* cnx, uint8_t* bytes,
                                                size_t bytes_max_size, int epoch, picoquic_path_t* path_x);

#ifdef __cplusplus
}
#endif

#endif /* PICOQUIC_H */
