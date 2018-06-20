/*
 * Copyright (C) 2017 Glimp IP Ltd
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include <glimpse_log.h>

#include "parson.h"

#define vector(type,size) type __attribute__ ((vector_size(sizeof(type)*(size))))

#define RDT_VERSION 6

typedef struct {
    /* XXX: Note that (at least with gcc) then uv will have a 16 byte
     * aligment resulting in a total struct size of 32 bytes with 4 bytes
     * alignment padding at the end
     */
    vector(float,4) uv;     // U in [0:2] and V in [2:4]
    float t;                // Threshold
    uint32_t label_pr_idx;  // Index into label probability table (1-based)
} Node;

typedef struct {
    char    tag[3];
    uint8_t version;
    uint8_t depth;
    uint8_t n_labels;
    uint8_t bg_label;
    // 1 byte padding
    float   fov;
    float   bg_depth; // v5+
    uint8_t sample_uv_offsets_nearest; // v6+
    uint8_t sample_uv_z_in_mm; // v6+
} RDTHeader;

typedef struct {
    RDTHeader header;
    Node* nodes;
    uint32_t n_pr_tables;
    float* label_pr_tables;
} RDTree;

#ifdef __cplusplus
extern "C" {
#endif

RDTree*
rdt_tree_load_from_json(struct gm_logger* log,
                        JSON_Value* json_tree_value,
                        bool allow_incomplete_leaves,
                        char** err);
RDTree*
rdt_tree_load_from_json_file(struct gm_logger* log,
                             const char* filename,
                             bool allow_incomplete_leaves,
                             char** err);

RDTree*
rdt_tree_load_from_buf(struct gm_logger* log,
                       uint8_t* tree,
                       int len,
                       char** err);
RDTree*
rdt_tree_load_from_file(struct gm_logger* log,
                        const char* filename,
                        char** err);

void
rdt_tree_destroy(RDTree* tree);

bool
rdt_tree_save(RDTree* tree, const char* filename);

RDTree**
rdt_forest_load_from_files(struct gm_logger* log,
                           const char** files,
                           int n_files,
                           char **err);

void
rdt_forest_destroy(RDTree** forest, int n_trees);

#ifdef __cplusplus
};
#endif
