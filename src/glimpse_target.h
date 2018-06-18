/*
 * Copyright (C) 2018 Glimp IP Ltd
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

#include "glimpse_log.h"
#include "glimpse_context.h"

struct gm_target;

#ifdef __cplusplus
extern "C" {
#endif

struct gm_target *
gm_target_new(struct gm_logger *logger,
              char **err,
              const char *metadata_prefix);

unsigned int
gm_target_get_n_frames(struct gm_target *target);

void
gm_target_set_frame(struct gm_target *target,
                    unsigned int frame);

void
gm_target_update(struct gm_target *target,
                 const struct gm_skeleton *skeleton);

float
gm_target_get_cumulative_error(struct gm_target *target);

float
gm_target_get_error(struct gm_target *target,
                    int bone_head,
                    int bone_tail);

void
gm_target_free(struct gm_target *target);

#ifdef __cplusplus
}
#endif
