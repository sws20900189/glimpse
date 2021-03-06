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

#ifdef __ANDROID__
#include <jni.h>
#endif

#include "glimpse_context.h"

enum gm_device_event_type
{
    /* The device itself is ready to be used */
    GM_DEV_EVENT_READY,

    /* A new frame has been captured by the device */
    GM_DEV_EVENT_FRAME_READY,
};

struct gm_device_event
{
    struct gm_device *device;
    enum gm_device_event_type type;

    union {
        struct {
            uint64_t buffers_mask;
        } frame_ready;
        struct {
            struct gm_ui_property *prop;
        } prop_changed;
    };
};


enum gm_device_type {
    GM_DEVICE_KINECT,
    GM_DEVICE_RECORDING,
    GM_DEVICE_TANGO,
    GM_DEVICE_AVF,
};

struct gm_device_config {
    enum gm_device_type type;
    union {
        struct {
            int device_number;
        } kinect;
        struct {
            const char *path;
        } recording;
    };
};

#ifdef __cplusplus
extern "C" {
#endif

struct gm_device *
gm_device_open(struct gm_logger *log,
               struct gm_device_config *config,
               char **err);

enum gm_device_type
gm_device_get_type(struct gm_device *dev);

void
gm_device_set_event_callback(struct gm_device *dev,
                             void (*event_callback)(struct gm_device_event *event,
                                                    void *user_data),
                             void *user_data);

bool
gm_device_commit_config(struct gm_device *dev, char **err);

struct gm_ui_properties *
gm_device_get_ui_properties(struct gm_device *dev);

int
gm_device_get_max_depth_pixels(struct gm_device *dev);

int
gm_device_get_max_video_pixels(struct gm_device *dev);

struct gm_extrinsics *
gm_device_get_depth_to_video_extrinsics(struct gm_device *dev);

/* Based on the device's natural orientation the camera module might be
 * physically rotated such that the 'top' of a camera frame might be sideways
 * compared to the top of the display.
 */
enum gm_rotation
gm_device_get_camera_rotation(struct gm_device *dev);

/* It's expected that events aren't synchronously handled within the above
 * event callback considering that it's undefined what thread the callback
 * is invoked on and it's undefined what locks might be held during the
 * invocation whereby the device api may not be reentrant at that point.
 *
 * An event will likely be queued for processing later but when processing
 * is finished then the event structure needs to be freed with this api:
 */
void gm_device_event_free(struct gm_device_event *event);

void gm_device_start(struct gm_device *dev);
void gm_device_stop(struct gm_device *dev);

void gm_device_close(struct gm_device *dev);

void
gm_device_request_frame(struct gm_device *dev, uint64_t buffers_mask);

struct gm_frame *
gm_device_get_latest_frame(struct gm_device *dev);

struct gm_frame *
gm_device_combine_frames(struct gm_device *dev, struct gm_frame *master,
                         struct gm_frame *depth, struct gm_frame *video);

#ifdef __ANDROID__
void
gm_device_attach_jvm(struct gm_device *dev, JavaVM *jvm);
#endif

#ifdef __cplusplus
}
#endif
