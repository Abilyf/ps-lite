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

/*
 * Context hash.
 * Retrieve an object based on a hash of a context ID, or alternatively based on
 * source address and port number.
 */
#ifndef PICOHASH_H
#define PICOHASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _picohash_item {
    uint64_t hash;
    struct _picohash_item* next_in_bin;
    void* key;
} picohash_item;

typedef struct picohash_table {
    /* TODO: lock ! */
    picohash_item** hash_bin;
    size_t nb_bin;
    size_t count;
    uint64_t (*picohash_hash)(void*);
    int (*picohash_compare)(void*, void*);
} picohash_table;

picohash_table* picohash_create(size_t nb_bin,
    uint64_t (*picohash_hash)(void*),
    int (*picohash_compute)(void*, void*));

picohash_item* picohash_retrieve(picohash_table* hash_table, void* key);

int picohash_insert(picohash_table* hash_table, void* key);

void picohash_item_delete(picohash_table* hash_table, picohash_item* item, int delete_key_too);

void picohash_delete(picohash_table* hash_table, int delete_key_too);

uint64_t picohash_bytes(uint8_t* key, uint32_t length);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* PICOHASH_H */
