/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "picoquic_internal.h"
#include "h3zero.h"
#include "democlient.h"

/* List of supported protocols 
 */

typedef struct st_picoquic_alpn_list_t {
    picoquic_alpn_enum alpn_code;
    char const* alpn_val;
} picoquic_alpn_list_t;

static picoquic_alpn_list_t alpn_list[] = {
    { picoquic_alpn_http_3, "h3-29" },
    { picoquic_alpn_http_0_9, "hq-29"},
    { picoquic_alpn_http_3, "h3-28" },
    { picoquic_alpn_http_0_9, "hq-28"},
    { picoquic_alpn_http_3, "h3-27" },
    { picoquic_alpn_http_0_9, "hq-27"},
    { picoquic_alpn_http_3, "h3" },
    { picoquic_alpn_http_0_9, "hq"}
};

static size_t nb_alpn_list = sizeof(alpn_list) / sizeof(picoquic_alpn_list_t);

void picoquic_demo_client_set_alpn_list(void* tls_context)
{
    int ret = 0;

    for (size_t i = 0; i < nb_alpn_list; i++) {
        if (alpn_list[i].alpn_code == picoquic_alpn_http_3 ||
            alpn_list[i].alpn_code == picoquic_alpn_http_0_9) {
            ret = picoquic_add_proposed_alpn(tls_context, alpn_list[i].alpn_val);
            if (ret != 0) {
                DBG_PRINTF("Could not propose ALPN=%s, ret=0x%x", alpn_list[i].alpn_val, ret);
                break;
            }
        }
    }
}

picoquic_alpn_enum picoquic_parse_alpn(char const* alpn)
{
    picoquic_alpn_enum code = picoquic_alpn_undef;

    if (alpn != NULL) {
        for (size_t i = 0; i < nb_alpn_list; i++) {
            if (strcmp(alpn_list[i].alpn_val, alpn) == 0) {
                code = alpn_list[i].alpn_code;
                break;
            }
        }
    }

    return code;
}

picoquic_alpn_enum picoquic_parse_alpn_nz(char const* alpn, size_t len)
{
    picoquic_alpn_enum code = picoquic_alpn_undef;

    if (alpn != NULL) {
        for (size_t i = 0; i < nb_alpn_list; i++) {
            if (memcmp(alpn, alpn_list[i].alpn_val, len) == 0 &&
                alpn_list[i].alpn_val[len] == 0) {
                code = alpn_list[i].alpn_code;
                break;
            }
        }
    }

    return code;
}

void picoquic_demo_client_set_alpn_from_tickets(picoquic_cnx_t* cnx, picoquic_demo_callback_ctx_t* ctx, uint64_t current_time)
{
    const char* sni = cnx->sni;
    if (sni != NULL) {
        uint16_t sni_len = (uint16_t) strlen(sni);

        for (size_t i = 0; i < nb_alpn_list; i++) {
            if ((alpn_list[i].alpn_code == picoquic_alpn_http_3 ||
                alpn_list[i].alpn_code == picoquic_alpn_http_0_9) &&
                alpn_list[i].alpn_val != NULL) {
                uint8_t* ticket;
                uint16_t ticket_length;

                if (picoquic_get_ticket(cnx->quic->p_first_ticket, current_time, sni, sni_len,
                    alpn_list[i].alpn_val, (uint16_t) strlen(alpn_list[i].alpn_val), &ticket, &ticket_length) == 0) {
                    ctx->alpn = alpn_list[i].alpn_code;
                    cnx->alpn = picoquic_string_duplicate(alpn_list[i].alpn_val);
                    break;
                }
            }
        }
    }
}

/*
 * Code common to H3 and H09 clients
 */

static picoquic_demo_client_stream_ctx_t* picoquic_demo_client_find_stream(
    picoquic_demo_callback_ctx_t* ctx, uint64_t stream_id)
{
    picoquic_demo_client_stream_ctx_t * stream_ctx = ctx->first_stream;

    while (stream_ctx != NULL && stream_ctx->stream_id != stream_id) {
        stream_ctx = stream_ctx->next_stream;
    }

    return stream_ctx;
}

/*将需要的文件数据全部写入到context的可用位置中，同时更新echo_sent,这一步也是判断是否为最后一个包的部分，针对上传而言*/
int demo_client_prepare_to_send(void * context, size_t space, size_t echo_length, size_t * echo_sent, FILE * F, char** msg)
{
    int ret = 0;

    if (*echo_sent < echo_length) {//检查文件是否发送完毕
        uint8_t * buffer;
        size_t available = echo_length - *echo_sent;//这里的available是指需要传输的文件的剩余数据，并非是指本次传输的数据
        int is_fin = 1;//结束标志位

        if (available > space) {//这里的space指的是一个数据包1536范围内的最大数据量，所以这里有个判定，如果available大于限制范围
            available = space;//就将本次能最大的发送数据量赋值给available，所以这里的available和上面的不同，这里是指本次传输的可用数据量
            is_fin = 0;
        }

        buffer = picoquic_provide_stream_data_buffer(context, available, is_fin, !is_fin);//这之后buffer是指向context的数据索引位置
        if (buffer != NULL) {
            //DBG_PRINTF("Preparing file");
            if (F) {//这是指定了传输文件的情况
                size_t nb_read = fread(buffer, 1, available, F);//从F中读取avaliable个字节到buffer中，
                //依据上面的分析，可知这里写入到buffer里的是一个1536数据包除去包头部分所能容纳的最大限额，通过这种方式做了切分

                if (nb_read != available) {
                    ret = -1;
                }
                else {
                    *echo_sent += (uint32_t)available;//更新已发送数据
                    ret = 0;
                }
            }
            else if(msg && *msg) {//传输message字节流信息
                //DBG_PRINTF("Preparing msg");
                memcpy(buffer, *msg, available);
                *msg += available;
                *echo_sent += (uint32_t)available;
                ret = 0;
            }        
            else {//以下是未指定传输文件的情况，因此是生成一些数据到buffer中
                //DBG_PRINTF("Preparing random data");
                int r = (74 - (*echo_sent % 74)) - 2;

                // TODO: fill buffer with some text 
                memset(buffer, 0x5A, available);

                while (r < (int)available) {
                    if (r >= 0) {
                        buffer[r] = '\r';
                    }
                    r++;
                    if (r >= 0 && (unsigned int)r < available) {
                        buffer[r] = '\n';
                    }
                    r += 73;
                }
                *echo_sent += (uint32_t)available;
                ret = 0;
            }
           
        }
        else {
            ret = -1;
        }
    }

    return ret;
}

/*
 * H3Zero client. This is a simple client that conforms to HTTP 3.0,
 * but the client implementation is barebone.
 */

int h3zero_client_create_stream_request(
    uint8_t * buffer, size_t max_bytes, uint8_t const * path, size_t path_len, size_t post_size, const char * host, size_t * consumed)
{
    int ret = 0;
    uint8_t * o_bytes = buffer;
    uint8_t * o_bytes_max = o_bytes + max_bytes;

    *consumed = 0;

    if (max_bytes < 3) {
        o_bytes = NULL;
    }
    else {
        /* Create the request frame for the specified document */
        *o_bytes++ = h3zero_frame_header;
        o_bytes += 2; /* reserve two bytes for frame length */
        if (post_size == 0) {//果不其然，通过post_size的有无判定当前的行为
            //DBG_PRINTF("create get request");
            o_bytes = h3zero_create_request_header_frame(o_bytes, o_bytes_max,
                (const uint8_t *)path, path_len, host);
        }
        else {
            //DBG_PRINTF("create post request");
            o_bytes = h3zero_create_post_header_frame(o_bytes, o_bytes_max,
                (const uint8_t *)path, path_len, host, h3zero_content_type_text_plain);
        }
    }

    if (o_bytes == NULL) {
        DBG_PRINTF("create post request failed");
        ret = -1;
    }
    else {
        size_t header_length = o_bytes - &buffer[3];
        if (header_length < 64) {
            buffer[1] = (uint8_t)(header_length);
            memmove(&buffer[2], &buffer[3], header_length);
            o_bytes--;
        }
        else {
            buffer[1] = (uint8_t)((header_length >> 8) | 0x40);
            buffer[2] = (uint8_t)(header_length & 0xFF);
        }

        if (post_size > 0) {
            /* Add initial DATA frame for POST */
            size_t ll = 0;

            if (o_bytes < o_bytes_max) {
                *o_bytes++ = h3zero_frame_data;
                ll = picoquic_varint_encode(o_bytes, o_bytes_max - o_bytes, post_size);
                o_bytes += ll;
            }
            if (ll == 0) {
                ret = -1;
            }
            else {
                *consumed = o_bytes - buffer;
            }
        }
        else {
            *consumed = o_bytes - buffer;
        }
    }

    return ret;
}

int h3zero_client_init(picoquic_cnx_t* cnx)
{
    uint8_t decoder_stream_head = 0x03;
    uint8_t encoder_stream_head = 0x02;
    int ret = picoquic_add_to_stream(cnx, 2, h3zero_default_setting_frame, h3zero_default_setting_frame_size, 0);

    /*if (ret == 0) {
		// set the stream #2 to be the next stream to write!
        ret = picoquic_mark_high_priority_stream(cnx, 2, 1);
    }*/

    if (ret == 0) {
        /* set the stream 6 as the encoder stream, although we do not actually create dynamic codes. */
        ret = picoquic_add_to_stream(cnx, 6, &encoder_stream_head, 1, 0);
    }

    if (ret == 0) {
        /* set the stream 10 as the decoder stream, although we do not actually create dynamic codes. */
        ret = picoquic_add_to_stream(cnx, 10, &decoder_stream_head, 1, 0);
    }


    return ret;
}

/* HTTP 0.9 client. 
 * This is the client that was used for QUIC interop testing prior
 * to availability of HTTP 3.0. It allows for testing transport
 * functions without dependencies on the HTTP layer. Instead, it
 * uses the simplistic HTTP 0.9 definition, in which a command
 * would simply be "GET /document.html\n\r\n\r".
 */

int h09_demo_client_prepare_stream_open_command(
    uint8_t * command, size_t max_size, uint8_t const* path, size_t path_len, size_t post_size, char const * host, size_t * consumed)
{

    if (post_size == 0) {
        if (path_len + 6 >= max_size) {
            return -1;
        }

        command[0] = 'G';
        command[1] = 'E';
        command[2] = 'T';
        command[3] = ' ';
        if (path_len > 0) {
            memcpy(&command[4], path, path_len);
        }
        command[path_len + 4] = '\r';
        command[path_len + 5] = '\n';
        command[path_len + 6] = 0;

        *consumed = path_len + 6;
    }
    else {
        size_t byte_index = 0;
        char const * post_head = "POST ";
        char const * post_middle = " HTTP/1.0\r\nHost: ";
        char const * post_trail = "\r\nContent-Type: text/plain\r\n\r\n";
        size_t host_len = (host == NULL) ? 0 : strlen(host);
        if (path_len + host_len + strlen(post_head) + strlen(post_middle) + strlen(post_trail) >= max_size) {
            return -1;
        }
        memcpy(command, post_head, strlen(post_head));
        byte_index = strlen(post_head);
        memcpy(command + byte_index, path, path_len);
        byte_index += path_len;
        memcpy(command + byte_index, post_middle, strlen(post_middle));
        byte_index += strlen(post_middle);
        if (host != NULL) {
            memcpy(command + byte_index, host, host_len);
        }
        byte_index += host_len;
        memcpy(command + byte_index, post_trail, strlen(post_trail));
        byte_index += strlen(post_trail);
        command[byte_index] = 0;
        *consumed = byte_index;
    }

    return 0;
}

/*
 * Unified procedures used for H3 and H09 clients
 */

static int picoquic_demo_client_open_stream(picoquic_cnx_t* cnx,//这一步将ctx写入到stream里
    picoquic_demo_callback_ctx_t* ctx,
    uint64_t stream_id, char const* doc_name, char const* fname, size_t post_size, size_t* post_sent, char** msg, uint64_t nb_repeat)
{
    int ret = 0;
    uint8_t buffer[1024];
    picoquic_demo_client_stream_ctx_t* stream_ctx = (picoquic_demo_client_stream_ctx_t*)
        malloc(sizeof(picoquic_demo_client_stream_ctx_t));//这一步创建了当前流的上下文，分配了动态内存

    if (stream_ctx == NULL) {
		fprintf(stdout, "Memory Error, cannot create stream context %d\n", (int)stream_id);
        ret = -1;
    }
    else {
        ctx->nb_open_streams++;
        ctx->nb_client_streams++;
        memset(stream_ctx, 0, sizeof(picoquic_demo_client_stream_ctx_t));//初始化
        stream_ctx->next_stream = ctx->first_stream;
        ctx->first_stream = stream_ctx;
        stream_ctx->stream_id = stream_id + nb_repeat*4u;
        stream_ctx->post_size = post_size;
        stream_ctx->post_sent = post_sent;
        stream_ctx->msg = msg;
        gettimeofday(&stream_ctx->tv_start, NULL);

        if (ctx->no_disk) {
            stream_ctx->F = NULL;
        }
        else {
#ifdef _WINDOWS
            char const* sep = "\\";
#else
            char const* sep = "/";
#endif
            char const * x_name = fname;
            char path_name[1024];

            if (ctx->out_dir != NULL && (x_name[0] == '/' || x_name[0] == '_')) {
                /* If writing in the specified directory, remove the initial "/",
                 * or '_' if it was sanitized to that before. */
                x_name++;
            }

            if (nb_repeat > 0) {
                ret = picoquic_sprintf(path_name, sizeof(path_name), NULL, "%s%sr%dx%s",
                    (ctx->out_dir == NULL)?".": ctx->out_dir, sep, (int)nb_repeat, x_name);//这一步就是如果未指定存储位置
                //则默认保存在当前目录，这里是格式化输出要保存的位置的路径名，存储在path_name中
            }
            else {
                ret = picoquic_sprintf(path_name, sizeof(path_name), NULL, "%s%s%s",
                    (ctx->out_dir == NULL) ? "." : ctx->out_dir, sep, x_name);
            }

            if (ret == 0) {
                stream_ctx->f_name = picoquic_string_duplicate(path_name);
                /* In order to reduce the number of open files, we only open the file when we start receiving data.*/
            }
            else {
                stream_ctx->F = NULL;
            }
        }
        if (ret == 0) {
            stream_ctx->is_open = 1;
        }else if (cnx->quic->F_log) {
            fprintf(cnx->quic->F_log,   "Cannot create file name: %s\n", fname);
        }
    }

    if (ret == 0) {
        size_t request_length = 0;
        uint8_t name_buffer[514];
        uint8_t * path;
        size_t path_len;

		/* make sure that the doc name is properly formated */
        path = (uint8_t *)doc_name;
        path_len = strlen(doc_name) ;
        if (doc_name[0] != '/' && path_len + 2 <= sizeof(name_buffer)) {//将文件路径开头加上“/”，并更新路径长度
            name_buffer[0] = '/';
            if (path_len > 0) {
                memcpy(&name_buffer[1], doc_name, path_len);
            }
            path = name_buffer;
            path_len++;
            name_buffer[path_len] = 0;
        }

        /* Format the protocol specific request */
        switch (ctx->alpn) {
        case picoquic_alpn_http_3:
            //DBG_PRINTF("ret: %d", ret);
            ret = h3zero_client_create_stream_request(
                buffer, sizeof(buffer), path, path_len, post_size, cnx->sni, &request_length);
            //DBG_PRINTF("ret: %d", ret);
            break;
        case picoquic_alpn_http_0_9:
        default:
            ret = h09_demo_client_prepare_stream_open_command(
                buffer, sizeof(buffer), path, path_len, post_size, cnx->sni, &request_length);
            break;
        }

		/* Send the request */

        if (ret == 0) {
            //DBG_PRINTF("ret: %d", ret);
            ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, buffer, request_length,
                (post_size > 0 || (ctx->delay_fin && stream_id == 0))?0:1, stream_ctx);
            //DBG_PRINTF("ret: %d", ret);
            if (post_size > 0) {
                //DBG_PRINTF("ret: %d", ret);
                ret = picoquic_mark_active_stream(cnx, stream_id, 1, stream_ctx);
            }
            //DBG_PRINTF("ret: %d", ret);
        }

        if (cnx->quic->F_log) {
            if (ret != 0) {
                fprintf(cnx->quic->F_log, "Cannot send %s command for stream(%d): %s\n", (post_size == 0) ? "GET" : "POST", (int)stream_ctx->stream_id, path);
            }
            else if (nb_repeat == 0) {
                fprintf(cnx->quic->F_log, "Opening stream %d to %s %s\n", (int)stream_ctx->stream_id, (post_size == 0) ? "GET" : "POST", path);
            }
        }
    }

    return ret;
}

static int picoquic_demo_client_close_stream(picoquic_cnx_t* cnx,
    picoquic_demo_callback_ctx_t* ctx, picoquic_demo_client_stream_ctx_t* stream_ctx)
{
    int ret = 0;
    if (stream_ctx != NULL && stream_ctx->is_open) {
        if (stream_ctx->f_name != NULL) {
            free(stream_ctx->f_name);
            stream_ctx->f_name = NULL;
        }
        stream_ctx->F = picoquic_file_close(stream_ctx->F);
        if (stream_ctx->is_file_open) {
            ctx->nb_open_files--;
            stream_ctx->is_file_open = 0;
        }
        stream_ctx->is_open = 0;
        gettimeofday(&stream_ctx->tv_end, NULL);
        ctx->nb_open_streams--; 
        ret = 1;
    }
    if (cnx->dml) {
        uint64_t stream_id = stream_ctx->stream_id;
        picoquic_stream_head** stream;
        if (cnx->first_stream) {
            stream = &(cnx->first_stream);
        }
        else {
            stream = NULL;
        }   
        while (stream && *stream) {
            if ((*stream)->stream_id == stream_id) {
                break;
            }
            else {
                *stream = (*stream)->next_stream;
            }
        };
        if (*stream != NULL) {
            DBG_PRINTF("prepare to free send stream:%p id:%d", *stream, (*stream)->stream_id);
            free(*stream);
            *stream = NULL;
        }
    }
    return ret;
}

int picoquic_demo_client_start_streams(picoquic_cnx_t* cnx,
    picoquic_demo_callback_ctx_t* ctx, uint64_t fin_stream_id)
{
    int ret = 0;

    /* First perform ALPN specific initialization.
	 * This will trigger sending the "settings" in H3 mode */
    if (fin_stream_id == PICOQUIC_DEMO_STREAM_ID_INITIAL) {
        switch (ctx->alpn) {
        case picoquic_alpn_http_3:
            ret = h3zero_client_init(cnx);
            break;
        default:
            break;
        }
    }

	/* Open all the streams scheduled after the stream that
	 * just finished */
    //DBG_PRINTF("nb_demo_stream: %d", ctx->nb_demo_streams);
    for (size_t i = 0; ret == 0 && i < ctx->nb_demo_streams; i++) {
        if (ctx->demo_stream[i].previous_stream_id == fin_stream_id) {
            uint64_t repeat_nb = 0;
            do {
                ret = picoquic_demo_client_open_stream(cnx, ctx, ctx->demo_stream[i].stream_id,
                    ctx->demo_stream[i].doc_name,
                    ctx->demo_stream[i].f_name,
                    (size_t)ctx->demo_stream[i].post_size, 
                    ctx->demo_stream[i].post_sent, 
                    ctx->demo_stream[i].msg_data, 
                    repeat_nb);
                repeat_nb++;
            } while (ret == 0 && repeat_nb < ctx->demo_stream[i].repeat_count);

            if (ret == 0 && repeat_nb > 1 && cnx->quic->F_log) {
                fprintf(cnx->quic->F_log, "Repeated stream opening %d times.\n", (int)repeat_nb);
            }
            
            if (repeat_nb < ctx->demo_stream[i].repeat_count && cnx->quic->F_log) {
                fprintf(cnx->quic->F_log, "Could only open %d streams out of %d, ret = %d.\n", (int)repeat_nb, (int)ctx->demo_stream[i].repeat_count, ret);
            }
        }
    }

    return ret;
}

int picoquic_demo_client_open_stream_file(picoquic_cnx_t* cnx, picoquic_demo_callback_ctx_t* ctx, picoquic_demo_client_stream_ctx_t* stream_ctx)
{
    int ret = 0;

    if (!stream_ctx->is_file_open && ctx->no_disk == 0) {
        int last_err = 0;
        stream_ctx->F = picoquic_file_open_ex(stream_ctx->f_name, "wb", &last_err);
        if (stream_ctx->F == NULL) {
            DBG_PRINTF("Could not open file <%s> for stream %" PRIu64 ", error %d (0x%x)", stream_ctx->f_name, stream_ctx->stream_id, last_err, last_err);
            ret = -1;
        }
        else {
            stream_ctx->is_file_open = 1;
            ctx->nb_open_files++;
        }
    }

    return ret;
}

int picoquic_demo_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    uint64_t fin_stream_id = PICOQUIC_DEMO_STREAM_ID_INITIAL;
    picoquic_demo_callback_ctx_t* ctx = (picoquic_demo_callback_ctx_t*)callback_ctx;
    picoquic_demo_client_stream_ctx_t* stream_ctx = (picoquic_demo_client_stream_ctx_t *)v_stream_ctx;

    ctx->last_interaction_time = picoquic_get_quic_time(cnx->quic);
    ctx->progress_observed = 1;
    //DBG_PRINTF("fin_or_event:%d", fin_or_event);
    switch (fin_or_event) {
    case picoquic_callback_no_event:
    case picoquic_callback_stream_fin:
        /* Data arrival on stream #x, maybe with fin mark */
        /* TODO: parse the frames. */
        /* TODO: check settings frame */
        if (stream_ctx == NULL) {
            stream_ctx = picoquic_demo_client_find_stream(ctx, stream_id);
        }
        if (stream_ctx != NULL && stream_ctx->is_open) {
            if (!stream_ctx->is_file_open && ctx->no_disk == 0) {
                ret = picoquic_demo_client_open_stream_file(cnx, ctx, stream_ctx);
                stream_ctx->is_file_open = 1;
            }
            if (ret == 0 && length > 0) {
                switch (ctx->alpn) {
                case picoquic_alpn_http_3: {
                    uint16_t error_found = 0;
                    size_t available_data = 0;
                    uint8_t * bytes_max = bytes + length;
                    while (bytes < bytes_max) {
                        //这个位置应该是流数据
                        bytes = h3zero_parse_data_stream(bytes, bytes_max, &stream_ctx->stream_state, &available_data, &error_found);
                        if (bytes == NULL) {
                            ret = picoquic_close(cnx, error_found);
                            if (ret != 0 && cnx->quic->F_log) {
                                fprintf(cnx->quic->F_log, "Could not parse incoming data from stream %" PRIu64 ", error 0x%x", stream_id, error_found);
                            }
                            break;
                        }
                        else if (available_data > 0) {
                            if (!stream_ctx->flow_opened){
                                if (stream_ctx->stream_state.current_frame_length < 0x100000) {
                                    stream_ctx->flow_opened = 1;
                                }
                                else if (cnx->cnx_state == picoquic_state_client_ready) {
                                    stream_ctx->flow_opened = 1;
                                    /* ret = picoquic_open_flow_control(cnx, stream_id, stream_ctx->stream_state.current_frame_length); */
                                }
                            }
                            if (ret == 0 && ctx->no_disk == 0) {
                                //从这里可以得出，bytes是指向数据的首地址，available_data是数据的大小，F则是存储位置为out_dir指定的一个标准输出流，追加写入
                                ret = (fwrite(bytes, 1, available_data, stream_ctx->F) > 0) ? 0 : -1;
                                if (ret != 0 && cnx->quic->F_log) {
                                    fprintf(cnx->quic->F_log, "Could not write data from stream %" PRIu64 ", error 0x%x", stream_id, ret);
                                }
                            }
                            stream_ctx->received_length += available_data;
                            bytes += available_data;
                        }
                    }
                    break;
                }
                case picoquic_alpn_http_0_9:
                    if (ctx->no_disk == 0) {
                        ret = (fwrite(bytes, 1, length, stream_ctx->F) > 0) ? 0 : -1;
                        if (ret != 0 && cnx->quic->F_log) {
                            fprintf(cnx->quic->F_log, "Could not write data from stream %" PRIu64 ", error 0x%x", stream_id, ret);
                        }
                    }
                    stream_ctx->received_length += length;
                    break;
                default:
                    DBG_PRINTF("%s", "ALPN not selected!");
                    ret = -1;
                    break;
                }
            }

            if (fin_or_event == picoquic_callback_stream_fin) {
                //DBG_PRINTF("prepare to close stream for continuing sending msg");
                if (picoquic_demo_client_close_stream(cnx, ctx, stream_ctx)) {
                    fin_stream_id = stream_id;
                    //DBG_PRINTF("fin_stream_id:%d", fin_stream_id);
                    if (stream_id <= 64 && cnx->quic->F_log) {
                        fprintf(cnx->quic->F_log, "Stream %d ended after %d bytes\n",
                        (int)stream_id, (int)stream_ctx->received_length);
                    }
                    if (stream_ctx->received_length == 0 && cnx->quic->F_log) {
                        fprintf(cnx->quic->F_log, "Stream %d ended after %d bytes, ret=0x%x\n",
                            (int)stream_id, (int)stream_ctx->received_length, ret);
                    }
                }
                /*                picoquic_stream_head* stream_tmp = picoquic_find_stream(cnx, stream_id, 0);
                if (stream_tmp) {
                    DBG_PRINTF("|is_active:%d|fin_requested:%d|fin_sent:%d|reuse:%d|",
                        stream_tmp->is_active, stream_tmp->fin_requested, stream_tmp->fin_sent, stream_tmp->reuse);
                    stream_tmp->reuse = 1;
                    stream_tmp->is_active = 1;
                    stream_tmp->fin_requested = 0;
                    stream_tmp->fin_sent = 0;
                    stream_tmp->fin_signalled = 0;
                    stream_tmp->fin_received = 0;
                    stream_tmp->fin_offset = 0;
                    stream_tmp->consumed_offset = 0;
                    stream_tmp->sending_offset = 0;
                    stream_tmp->sent_offset = 0;
                }
                */
            }
        }
        break;
    case picoquic_callback_stream_reset: /* Server reset stream #x */
    case picoquic_callback_stop_sending: /* Server asks client to reset stream #x */
        /* TODO: special case for uni streams. */
        if (stream_ctx == NULL) {
            stream_ctx = picoquic_demo_client_find_stream(ctx, stream_id);
        }
        if (picoquic_demo_client_close_stream(cnx, ctx, stream_ctx)) {
            fin_stream_id = stream_id;
            if (cnx->quic->F_log) {
                fprintf(cnx->quic->F_log,  "Stream %d reset after %d bytes\n",
                    (int)stream_id, (int)stream_ctx->received_length);
            }
        }
        picoquic_reset_stream(cnx, stream_id, 0);
        /* TODO: higher level notify? */
        break;
    case picoquic_callback_stateless_reset:
        if (cnx->quic->F_log) {
            fprintf(cnx->quic->F_log,  "Received a stateless reset.\n");
        }
        break;
    case picoquic_callback_close: /* Received connection close */
        if (cnx->quic->F_log) {
            fprintf(cnx->quic->F_log,  "Received a request to close the connection.\n");
        }
        ctx->connection_closed = 1;
        break;
    case picoquic_callback_application_close: /* Received application close */
        if (cnx->quic->F_log) {
            fprintf(cnx->quic->F_log,  "Received a request to close the application.\n");
        }
        ctx->connection_closed = 1;
        break;
    /*case picoquic_callback_version_negotiation:
        if (!ctx->no_print) {
            fprintf(stdout, "Received a version negotiation request:");
            for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
                uint32_t vn = PICOPARSE_32(bytes + byte_index);
                fprintf(stdout, "%s%08x", (byte_index == 0) ? " " : ", ", vn);
            }
            fprintf(stdout, "\n");
        }
        break;
    case picoquic_callback_stream_gap:*/
        /* Gap indication, when unreliable streams are supported *//*
        fprintf(stdout, "Received a gap indication.\n");
        if (stream_ctx == NULL) {
            stream_ctx = picoquic_demo_client_find_stream(ctx, stream_id);
        }
        if (picoquic_demo_client_close_stream(ctx, stream_ctx)) {
            fin_stream_id = stream_id;
            fprintf(stdout, "Stream %d reset after %d bytes\n",
                (int)stream_id, (int)stream_ctx->received_length);
        }
        /* TODO: Define what error. Stop sending? *//*
        picoquic_reset_stream(cnx, stream_id, H3ZERO_INTERNAL_ERROR);
        break;*/
    case picoquic_callback_prepare_to_send:
        /* Used on client when posting data */
            /* Used for active streams */
        if (stream_ctx == NULL) {
            /* Unexpected */
            picoquic_reset_stream(cnx, stream_id, 0);
            return 0;
        }
        else {
            //DBG_PRINTF("prepare to send msg");
            //return demo_client_prepare_to_send((void*)bytes, length, stream_ctx->post_size, &stream_ctx->post_sent, NULL, NULL);
            /*Write down message from psquic*/
            return demo_client_prepare_to_send((void*)bytes, length, stream_ctx->post_size, stream_ctx->post_sent, NULL, stream_ctx->msg);
        }
    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
        ctx->connection_ready = 1;
        break;
    case picoquic_callback_request_alpn_list:
        picoquic_demo_client_set_alpn_list((void*)bytes);
        break;
    case picoquic_callback_set_alpn:
        ctx->alpn = picoquic_parse_alpn((const char*)bytes);
        break;
    default:
        /* unexpected */
        break;
    }

    if (ret == 0 && fin_stream_id != PICOQUIC_DEMO_STREAM_ID_INITIAL) {
         /* start next batch of streams! */
        //DBG_PRINTF("Start another stream");
        //DBG_PRINTF("check post size:%d", ctx->demo_stream->post_size);
        //DBG_PRINTF("before start another stream nb of stream:%d", ctx->nb_open_streams);
		ret = picoquic_demo_client_start_streams(cnx, ctx, fin_stream_id);
        //DBG_PRINTF("after start another stream ret:%d", ret);
        //DBG_PRINTF("after start another stream nb of stream:%d", ctx->nb_open_streams);
    }

    /* that's it */
    return ret;
}

int picoquic_demo_client_initialize_context(
    picoquic_demo_callback_ctx_t* ctx,
    picoquic_demo_stream_desc_t const * demo_stream,
	size_t nb_demo_streams,
	char const * alpn,
    int no_disk, int delay_fin)
{
    //memset(ctx, 0, sizeof(picoquic_demo_callback_ctx_t));
    ctx->demo_stream = demo_stream;
    ctx->nb_demo_streams = nb_demo_streams;
    ctx->alpn = picoquic_parse_alpn(alpn);
    ctx->no_disk = no_disk;
    ctx->delay_fin = delay_fin;

    return 0;
}


static void picoquic_demo_client_delete_stream_context(picoquic_demo_callback_ctx_t* ctx,
    picoquic_demo_client_stream_ctx_t * stream_ctx)
{
    int removed_from_context = 0;

    h3zero_delete_data_stream_state(&stream_ctx->stream_state);

    if (stream_ctx->f_name != NULL) {
        free(stream_ctx->f_name);
        stream_ctx->f_name = NULL;
    }

    if (stream_ctx->F != NULL) {
        DBG_PRINTF("Stream %d, file open after %d bytes\n", stream_ctx->stream_id, stream_ctx->received_length);
        stream_ctx->F = picoquic_file_close(stream_ctx->F);
    }

    if (stream_ctx == ctx->first_stream) {
        ctx->first_stream = stream_ctx->next_stream;
        removed_from_context = 1;
    }
    else {
        picoquic_demo_client_stream_ctx_t * previous = ctx->first_stream;

        while (previous != NULL) {
            if (previous->next_stream == stream_ctx) {
                previous->next_stream = stream_ctx->next_stream;
                removed_from_context = 1;
                break;
            }
            else {
                previous = previous->next_stream;
            }
        }
    }

    if (removed_from_context) {
        ctx->nb_client_streams--;
    }

    free(stream_ctx);
}

void picoquic_demo_client_delete_context(picoquic_demo_callback_ctx_t* ctx)
{
    picoquic_demo_client_stream_ctx_t * stream_ctx;

    while ((stream_ctx = ctx->first_stream) != NULL) {
        picoquic_demo_client_delete_stream_context(ctx, stream_ctx);
    }
}

/*返回text中第一个非空格、制表符或换行符的字符的地址*/
char const * demo_client_parse_stream_spaces(char const * text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }
    return text;
}

/*解析text中符号‘*’后的数字，将该数字视作这段请求文件需要重复的次数，之后返回除去“*数字：”后的去除空格的text地址，比如
“*12:Hello, *3:World!”，那么在调用这个函数时，就会将第一个*后的数字12作为该段请求的重复次数，之后返回“Hello, *3:World!”中H的地址，
用作后续的继续解析*/
char const * demo_client_parse_stream_repeat(char const * text, int * number)
{
    int rep = 0;

    if (*text == '*') {
        text++;
        while (text[0] >= '0' && text[0] <= '9') {
            rep *= 10;
            rep += *text++ - '0';
        }

        text = demo_client_parse_stream_spaces(text);

        if (*text == ':') {
            text++;
        }
        else {
            text = NULL;
        }
    }
    *number = rep;

    return text;
}

/*这一段用于解析除去重复次数之后的stream number，并将其存储在第三个传入参数中，比如输入为"*3: 24:hello.txt"，那么再经过重复次数的解析后，
指针将指向24的2，此时本函数将解析从这个指针开始（如果确实是数字）的连续数字，并将其存入到第三个传入参数中*/
char const * demo_client_parse_stream_number(char const * text, uint64_t default_number, uint64_t * number)
{
    if (text[0] < '0' || text[0] > '9') {
        *number = default_number;
    }
    else {
        *number = 0;
        do {
            int delta = *text++ - '0';
            *number *= 10;
            *number += delta;
        } while (text[0] >= '0' && text[0] <= '9');

        text = demo_client_parse_stream_spaces(text);

        if (*text == ':') {
            text++;
        }
        else {
            text = NULL;
        }
    }

    return text;
}

char const* demo_client_parse_stream_previous(char const* text, uint64_t default_number, uint64_t* number)
{

    if (text[0] != '-') {
        text = demo_client_parse_stream_number(text, default_number, number);
    }
    else {
        *number = PICOQUIC_DEMO_STREAM_ID_INITIAL;
        text++;

        text = demo_client_parse_stream_spaces(text);

        if (*text == ':') {
            text++;
        }
        else {
            text = NULL;
        }
    }

    return text;
}

/*这一段代码用于解析剩下的路径和文件名，分别存储到对应的传入参数中，其中path是包含文件名在内的完成路径，而f_name则是将路径中的“\”转为下划线“_”
之后得到的新文件名*/
char const * demo_client_parse_stream_path(char const * text, 
    char ** path, char ** f_name)
{
    size_t l_path = 0;
    int is_complete = 0;
    int need_dup = 0;

    while (text != NULL) {
        char c = text[l_path];

        if (c == 0 || c == ';' || c == ':') {
            is_complete = 1;
            break;
        }
        
        if (c == '/') {
            need_dup = 1;
        }

        l_path++;
    }

    if (is_complete) {
        *path = (char *)malloc(l_path + 1);
        if (*path == NULL) {
            is_complete = 0;
        }
        else {
            if (need_dup) {
                *f_name = (char *)malloc(l_path + 1);
                if (*f_name == NULL) {
                    is_complete = 0;
                    free(*path);
                    *path = NULL;
                }
            }
        }
    }

    if (is_complete) {
        memcpy(*path, text, l_path);
        (*path)[l_path] = 0;
        if (need_dup) {
            for (size_t i = 0; i < l_path; i++) {
                (*f_name)[i] = (text[i] == '/') ? '_' : text[i];
            }
            (*f_name)[l_path] = 0;
        }
        else {
            *f_name = *path;
        }

        text += l_path;

        if (*text == ':') {
            text++;
        }
    }
    else {
        text = NULL;
    }
    
    return text;
}

/*这一段解析文件名之后的数字，将其视为文件大小，存储至第三个传入参数中*/
char const * demo_client_parse_post_size(char const * text, uint64_t * post_size)
{
    if (text[0] < '0' || text[0] > '9') {
        *post_size = 0;
    }
    else {
        *post_size = 0;
        do {
            int delta = *text++ - '0';
            *post_size *= 10;
            *post_size += delta;
        } while (text[0] >= '0' && text[0] <= '9');

        text = demo_client_parse_stream_spaces(text);

        if (*text == ':') {
            text++;
        }
        else if (*text != 0 && *text != ';'){
            text = NULL;
        }
    }

    return text;
}

/*这一段代码用于解析text中不同数字代表的用于，其中用冒号":"分隔开不同内容，注意text是有顺序的，
现在以"*3: 24: flower/zll.png: 100"为例，将其输入，其中第一段*号后的数字被解析3为重复次数，24被解析为流id，flower/zll.png被解析为路径，更换
/为下划线_解析成文件名，100则被解析为post_size，由于输入参数中不存在"-"，因此函数demo_client_parse_stream_previous()中第一个输入字符将会是文件
路径名，所以previous_stream_id会被解析为default_stream，也就是0，具体结果如下
Input string: "*3: 24: flower/zll.png: 100"
Parsed repeat count: 3
Parsed stream id: 24
Paesed previous stream id: 0
Parsed path: "flower/zll.png"
Parsed file name: "flower_zll.png"
Parsed post size: 100
*/
char const * demo_client_parse_stream_desc(char const * text, uint64_t default_stream, uint64_t default_previous,
    picoquic_demo_stream_desc_t * desc)
{
    text = demo_client_parse_stream_repeat(text, &desc->repeat_count);

    if (text != NULL) {
        text = demo_client_parse_stream_number(text, default_stream, &desc->stream_id);
    }

    if (text != NULL) {
        text = demo_client_parse_stream_previous(
            demo_client_parse_stream_spaces(text), default_previous, &desc->previous_stream_id);
    }
    
    if (text != NULL){
        text = demo_client_parse_stream_path(
            demo_client_parse_stream_spaces(text), (char **)&desc->doc_name, (char **)&desc->f_name);
    }

    if (text != NULL) {
        text = demo_client_parse_post_size(demo_client_parse_stream_spaces(text), &desc->post_size);
    }

    /* Skip the final ';' */
    if (text != NULL && *text == ';') {
        text++;
    }

    return text;
}

void demo_client_delete_scenario_desc(size_t nb_streams, picoquic_demo_stream_desc_t * desc)
{
    for (size_t i = 0; i < nb_streams; i++) {
        if (desc[i].f_name != desc[i].doc_name && desc[i].f_name != NULL) {
            free((char*)desc[i].f_name);
            *(char**)(&desc[i].f_name) = NULL;
        }
        if (desc[i].doc_name != NULL) {
            free((char*)desc[i].doc_name);
            *(char**)(&desc[i].doc_name) = NULL;
        }
    }
    free(desc);
}

/*这里是解析text中有几个请求，请求由";"分割*/
size_t demo_client_parse_nb_stream(char const * text) {
    size_t n = 0;
    int after_semi = 1;

    while (*text != 0) {
        if (*text++ == ';') {
            n++;
            after_semi = 0;
        }
        else {
            after_semi = 1;
        }
    }

    n += after_semi;

    return n;
}

int demo_client_parse_scenario_desc(char const * text, size_t * nb_streams, picoquic_demo_stream_desc_t ** desc)
{
    //注意这里传入的desc是双重指针，是为了能够改变传入参数的值，如果只是传入指针，那么就是像变量一样该变的是副本值而非原本值；
    int ret = 0;
    /* first count the number of streams and allocate memory */
    size_t nb_desc = demo_client_parse_nb_stream(text);//请求的文件个数，代表了流的数量，这里传递的指针，指针值本身不会变
    size_t i = 0;
    uint64_t previous = PICOQUIC_DEMO_STREAM_ID_INITIAL;
    uint64_t stream_id = 0;

    //为这个指针变量(也就调用函数里的client_sc)所指向的内容分配nb_desc个picoquic_demo_stream_desc_t大小的连续空间，返回首地址到
    //client_sc，因为传递的是二级指针，所以改变的是原本的值；
    *desc = (picoquic_demo_stream_desc_t *)malloc(nb_desc*sizeof(picoquic_demo_stream_desc_t));

    if (*desc == NULL) {
        *nb_streams = 0;
        ret = -1;
    }
    else {
        while (text != NULL ) {
            //虽然是这里传递的也是一级指针，并不会改变指针本身，指针指向的依然是原来的内容
            //但是要注意，函数返回的是一个指针，而且又重新赋值给了这个指针，实际上他就是改变了；
            //只不过不是在函数内部改变的，而是在返回值，在这里赋值改变了
            //这个函数是去除了text开头的空格或者制表或者换行
            text = demo_client_parse_stream_spaces(text);
            if (*text == 0) {
                break;
            }
            if (i >= nb_desc) {
                /* count was wrong! */
                break;
            }
            else {
                //以下是根据text中请求的任务数量nb_desc来逐一初始化并赋值
                picoquic_demo_stream_desc_t* stream_desc = &(*desc)[i];//由于*desc已经被分配了一连串的区域，所以【i】则是针对不同的这个类型的区域块
                text = demo_client_parse_stream_desc(text, stream_id, previous, stream_desc);
                if (text != NULL) {
                    stream_id = stream_desc->stream_id + 4;
                    previous = stream_desc->stream_id;
                    i++;
                }
            }
        }

        *nb_streams = i;

        if (text == NULL) {
            ret = -1;
        }
    }

    return ret;
}
