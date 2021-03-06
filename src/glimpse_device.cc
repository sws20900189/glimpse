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


#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>

#include <vector>

#ifdef USE_FREENECT
#include <libfreenect.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#include <android/log.h>
#endif

#ifdef USE_TANGO
#include <tango_client_api.h>
#include <tango_support_api.h>
#include <glm/gtx/quaternion.hpp>
#endif

#ifdef USE_AVF
#include "ios_utils.h"
#endif

#include "parson.h"
#include "half.hpp"
#include "xalloc.h"

#include "image_utils.h"

#include "glimpse_log.h"
#include "glimpse_mem_pool.h"
#include "glimpse_device.h"

#undef GM_LOG_CONTEXT
#ifdef __ANDROID__
#define GM_LOG_CONTEXT "Glimpse Device"
#else
#define GM_LOG_CONTEXT "device"
#endif

#define xsnprintf(dest, n, fmt, ...) do { \
        if (snprintf(dest, n, fmt,  __VA_ARGS__) >= (int)(n)) \
            exit(1); \
    } while(0)

#define ARRAY_LEN(X) (sizeof(X)/sizeof(X[0]))

using half_float::half;

struct trail_crumb
{
    char tag[32];
    int n_frames;
    void *backtrace_frame_pointers[10];
};

struct gm_device_buffer
{
    struct gm_buffer base;

    struct gm_buffer_vtable vtable;

    struct gm_device *dev;
    struct gm_mem_pool *pool;

    /* Lets us debug when we've failed to release frame resources when
     * we come to destroy our resource pools
     */
    pthread_mutex_t trail_lock;
    std::vector<struct trail_crumb> trail;
};

struct gm_device_frame
{
    struct gm_frame base;

    struct gm_frame_vtable vtable;

    struct gm_device *dev;
    struct gm_mem_pool *pool;

    //TODO
#if 0
    enum gm_rotation rotation;
    float down[3];
#endif

    /* Lets us debug when we've failed to release frame resources when
     * we come to destroy our resource pools
     */
    pthread_mutex_t trail_lock;
    std::vector<struct trail_crumb> trail;
};

struct gm_device
{
    enum gm_device_type type;

    struct gm_logger *log;

    /* When a device is first opened it is not considered to be fully
     * configured until gm_device_commit_config() returns successfully.
     *
     * This allows for an extensible configuration API, e.g. for
     * setting callbacks before using the device.
     *
     * NB: Not all of the device API is ready to use while a device is
     * unconfigured. E.g. you shouldn't try and query camera intrinsics
     * and start/stop the device, until the device is configured *and*
     * a _READY event has been delivered.
     */
    bool configured;

    /* Between gm_device_start/stop boundaries the device is 'running' */
    bool running;

    union {
        struct {
            char *path;
            JSON_Value *json;

            /* properties (so careful about changing types) */
            int frame;
            bool loop;
            int max_frame;

            /* Used to break out of pause for skipping frames back and forth */
            bool ignore_loop;

            /* State in case playback is paused: */
            enum gm_rotation last_camera_rotation;
            struct gm_buffer *last_depth_buf;
            struct gm_buffer *last_video_buf;

            pthread_t io_thread;

            /* older recordings have intrinsics that apply to the entire recording
             * and more recent recordings attach intrinsics to each frame
             */
            bool fixed_intrinsics;
        } recording;

#ifdef USE_FREENECT
        struct {
            freenect_context *fctx;
            freenect_device *fdev;

            int ir_brightness;
            float req_tilt; // tilt requested via UI
            float phys_tilt; // tilt currently reported by HW
            float accel[3];
            float mks_accel[3];
            pthread_t io_thread;
        } kinect;
#endif

#ifdef USE_TANGO
        struct {
            TangoConfig tango_config;
            enum gm_rotation display_rotation;
            enum gm_rotation display_to_camera_rotation;
        } tango;
#endif

#ifdef USE_AVF
        struct {
            struct ios_av_session *session;
        } avf;
#endif
    };

    int camera_rotation; // enum gm_rotation
    int camera_rotation_prop_id;

    int max_depth_pixels;
    int max_video_pixels;
    struct gm_intrinsics video_intrinsics;
    struct gm_intrinsics depth_intrinsics;
    struct gm_extrinsics depth_to_video_extrinsics;

    void (*frame_callback)(struct gm_device *dev,
                           struct gm_frame *frame,
                           void *user_data);
    void *frame_callback_data;

    /* What data is required for the next frame?
     * E.g. _DEPTH | _VIDEO
     */
    pthread_mutex_t request_buffers_mask_lock;
    uint64_t frame_request_buffers_mask;
    uint64_t frame_ready_buffers_mask;

    pthread_mutex_t swap_buffers_lock;

    enum gm_format depth_format;

    enum gm_format video_format;

    /* Here 'ready' buffers are one that are ready to be collected into a
     * frame if requested. The 'back' buffers are the ones that the hardware
     * is currently writing into.
     */
    struct gm_mem_pool *video_buf_pool;
    struct gm_device_buffer *video_buf_ready;
    struct gm_device_buffer *video_buf_back;

    struct gm_mem_pool *depth_buf_pool;
    struct gm_device_buffer *depth_buf_ready;
    struct gm_device_buffer *depth_buf_back;

    uint64_t frame_time;
    struct gm_pose frame_pose;

    struct gm_mem_pool *frame_pool;
    struct gm_frame *last_frame;

    std::vector<struct gm_ui_enumerant> rotation_enumerants;
    struct gm_ui_properties properties_state;
    std::vector<struct gm_ui_property> properties;

    void (*event_callback)(struct gm_device_event *event,
                           void *user_data);

    void *callback_data;

#ifdef __ANDROID__
    JavaVM *jvm;
#endif
};

static const char *rotation_names[] = {
    "None",
    "90 degrees",
    "180 degrees",
    "270 degrees",
};

#ifdef USE_TANGO
static pthread_mutex_t jni_lock = PTHREAD_MUTEX_INITIALIZER;
static jobject early_tango_service_binder;

/* For our JNI callbacks we assume there can only be a single device...
 */
static struct gm_device *tango_singleton_dev;

/* We only care about the display rotation if we're running with Tango
 * and in other cases we're playing back a recording which my have
 * synthetic rotations
 */
static enum gm_rotation tango_display_rotation;
#endif

static uint64_t
get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec) * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct gm_device_event *
device_event_alloc(struct gm_device *device, enum gm_device_event_type type)
{
    struct gm_device_event *event =
        (struct gm_device_event *)xcalloc(sizeof(struct gm_device_event), 1);

    event->device = device;
    event->type = type;

    return event;
}

void
gm_device_event_free(struct gm_device_event *event)
{
    free(event);
}

static struct gm_device_frame *
mem_pool_acquire_frame(struct gm_mem_pool *pool, const char *bread_crumb)
{
    struct gm_device_frame *frame = (struct gm_device_frame *)
        mem_pool_acquire_resource(pool);

    gm_assert(frame->dev->log, frame->base.ref == 0,
              "frame was used after last free");

    frame->base.ref = 1;
    frame->base.depth = NULL;
    frame->base.depth_format = GM_FORMAT_UNKNOWN;
    frame->base.video = NULL;
    frame->base.video_format = GM_FORMAT_UNKNOWN;

    gm_frame_add_breadcrumb(&frame->base, bread_crumb);

    return frame;
}

static struct gm_device_buffer *
mem_pool_acquire_buffer(struct gm_mem_pool *pool, const char *bread_crumb)
{
    struct gm_device_buffer *buffer = (struct gm_device_buffer *)
        mem_pool_acquire_resource(pool);

    gm_assert(buffer->dev->log, buffer->base.ref == 0,
              "%s buffer was used after last free", mem_pool_get_name(pool));

    buffer->base.ref = 1;

    gm_buffer_add_breadcrumb(&buffer->base, bread_crumb);

    return buffer;
}

static void
device_frame_recycle(struct gm_frame *self)
{
    struct gm_device_frame *frame = (struct gm_device_frame *)self;
    struct gm_mem_pool *pool = frame->pool;

    gm_assert(frame->dev->log, frame->base.ref == 0, "Unbalanced frame unref");

    if (self->video) {
        gm_buffer_unref(self->video);
        self->video = NULL;
    }

    if (self->depth) {
        gm_buffer_unref(self->depth);
        self->depth = NULL;
    }

    frame->trail.clear();

    mem_pool_recycle_resource(pool, frame);
}

static void
device_frame_free(struct gm_mem_pool *pool, void *resource, void *user_data)
{
    //struct gm_device *dev = user_data;
    struct gm_device_frame *frame = (struct gm_device_frame *)resource;
    gm_debug(frame->dev->log, "freeing frame %p", frame);

    delete frame;
}

static void
device_frame_add_breadcrumb(struct gm_frame *self, const char *tag)
{
    struct gm_device_frame *frame = (struct gm_device_frame *)self;
    struct trail_crumb crumb;

    gm_assert(frame->dev->log, frame->base.ref >= 0,
              "Use of frame after free");

    snprintf(crumb.tag, sizeof(crumb.tag), "%s", tag);

    crumb.n_frames = gm_backtrace(crumb.backtrace_frame_pointers,
                                  1, // skip top stack frame
                                  10);

    pthread_mutex_lock(&frame->trail_lock);
    frame->trail.push_back(crumb);
    pthread_mutex_unlock(&frame->trail_lock);
}

static void *
device_frame_alloc(struct gm_mem_pool *pool, void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    struct gm_device_frame *frame = new gm_device_frame();

    frame->vtable.free = device_frame_recycle;
    frame->vtable.add_breadcrumb = device_frame_add_breadcrumb;
    frame->dev = dev;
    frame->pool = pool;

    frame->base.api = &frame->vtable;

    pthread_mutex_init(&frame->trail_lock, NULL);

    return frame;
}

static void
device_buffer_recycle(struct gm_buffer *self)
{
    struct gm_device_buffer *buf = (struct gm_device_buffer *)self;
    struct gm_mem_pool *pool = buf->pool;

    buf->trail.clear();

    mem_pool_recycle_resource(pool, buf);

    gm_assert(buf->dev->log, buf->base.ref == 0, "Unbalanced buffer unref");
}

static void
device_buffer_add_breadcrumb(struct gm_buffer *self, const char *tag)
{
    struct gm_device_buffer *buffer = (struct gm_device_buffer *)self;
    struct trail_crumb crumb;

    gm_assert(buffer->dev->log, buffer->base.ref >= 0,
              "Use of buffer after free");

    snprintf(crumb.tag, sizeof(crumb.tag), "%s", tag);

    crumb.n_frames = gm_backtrace(crumb.backtrace_frame_pointers,
                                  1, // skip top stack frame
                                  10);

    pthread_mutex_lock(&buffer->trail_lock);
    buffer->trail.push_back(crumb);
    pthread_mutex_unlock(&buffer->trail_lock);
}

static void *
device_video_buf_alloc(struct gm_mem_pool *pool, void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    struct gm_device_buffer *buf = new gm_device_buffer();

    buf->vtable.free = device_buffer_recycle;
    buf->vtable.add_breadcrumb = device_buffer_add_breadcrumb;
    buf->dev = dev;
    buf->pool = pool;

    buf->base.api = &buf->vtable;

    switch (dev->type) {
    case GM_DEVICE_TANGO:
    case GM_DEVICE_KINECT:
        /* Allocated large enough for RGB data */
        buf->base.len = dev->max_video_pixels * 3;
        break;
    case GM_DEVICE_AVF:
    case GM_DEVICE_RECORDING:
        /* Allocated large enough for any data format */
        buf->base.len = dev->max_video_pixels * 4;
        break;
    }
    buf->base.data = xmalloc(buf->base.len);

    pthread_mutex_init(&buf->trail_lock, NULL);

    return buf;
}

static void
device_buffer_free(struct gm_mem_pool *pool,
                   void *resource,
                   void *user_data)
{
    //struct gm_device *dev = user_data
    struct gm_device_buffer *buf = (struct gm_device_buffer *)resource;
    xfree(buf->base.data);

    delete buf;
}

static void *
device_depth_buf_alloc(struct gm_mem_pool *pool, void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    struct gm_device_buffer *buf = new gm_device_buffer();

    buf->vtable.free = device_buffer_recycle;
    buf->vtable.add_breadcrumb = device_buffer_add_breadcrumb;
    buf->dev = dev;
    buf->pool = pool;

    buf->base.api = &buf->vtable;

    switch (dev->type) {
    case GM_DEVICE_TANGO:
        /* Allocated large enough for _XYZC_F32_M data */
        buf->base.len = dev->max_depth_pixels * 16;
        break;
    case GM_DEVICE_AVF:
        /* Allocated large enough for any data */
        buf->base.len = dev->max_depth_pixels * 16;
        break;
    case GM_DEVICE_RECORDING:
        /* Allocated large enough for any data */
        buf->base.len = dev->max_depth_pixels * 16;
        break;
    case GM_DEVICE_KINECT:
        /* Allocated large enough for _U16_MM data */
        buf->base.len = dev->max_depth_pixels * 2;
        break;
    }
    buf->base.data = xmalloc(buf->base.len);

    pthread_mutex_init(&buf->trail_lock, NULL);

    return buf;
}

/* XXX: the request_buffers_mask_lock must be held while calling this.
 *
 * Note: this implies that it's not currently safe for the reciever of the
 * event to synchronously request a new frame or call any device api that
 * might affect this buffers_mask (needing the same lock)
 */
static void
notify_frame_locked(struct gm_device *dev)
{
    struct gm_device_event *event =
        device_event_alloc(dev, GM_DEV_EVENT_FRAME_READY);

    gm_debug(dev->log, "notify_frame_locked (buffers_mask = 0x%" PRIx64,
             dev->frame_request_buffers_mask);

    event->frame_ready.buffers_mask = dev->frame_ready_buffers_mask;
    dev->frame_request_buffers_mask &= ~dev->frame_ready_buffers_mask;

    dev->event_callback(event, dev->callback_data);
}

static void
maybe_notify_frame_locked(struct gm_device *dev)
{
    if (dev->frame_request_buffers_mask & dev->frame_ready_buffers_mask)
    {
        notify_frame_locked(dev);
    }
}

#ifdef USE_FREENECT
static void
kinect_depth_frame_cb(freenect_device *fdev, void *depth, uint32_t timestamp)
{
    struct gm_device *dev = (struct gm_device *)freenect_get_user(fdev);

    if (!(dev->frame_request_buffers_mask & GM_REQUEST_FRAME_DEPTH))
        return;

    pthread_mutex_lock(&dev->swap_buffers_lock);

    struct gm_device_buffer *old = dev->depth_buf_ready;
    dev->depth_buf_ready = dev->depth_buf_back;
    dev->depth_buf_back = mem_pool_acquire_buffer(dev->depth_buf_pool, "kinect depth");
    // TODO: Figure out the Kinect timestamp format to translate it into
    //       nanoseconds
    //dev->frame_time = (uint64_t)timestamp;
    dev->frame_time = get_time();
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_DEPTH;

    freenect_set_depth_buffer(fdev, dev->depth_buf_back->base.data);
    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static void
kinect_rgb_frame_cb(freenect_device *fdev, void *video, uint32_t timestamp)
{
    struct gm_device *dev = (struct gm_device *)freenect_get_user(fdev);

    if (!(dev->frame_request_buffers_mask & GM_REQUEST_FRAME_VIDEO))
        return;

    pthread_mutex_lock(&dev->swap_buffers_lock);

    struct gm_device_buffer *old = dev->video_buf_ready;
    dev->video_buf_ready = dev->video_buf_back;
    dev->video_buf_back = mem_pool_acquire_buffer(dev->video_buf_pool, "kinect rgb");
    //dev->frame_time = (uint64_t)timestamp;
    dev->frame_time = get_time();
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_VIDEO;

    freenect_set_video_buffer(fdev, dev->video_buf_back->base.data);
    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static bool
kinect_open(struct gm_device *dev, struct gm_device_config *config, char **err)
{
    if (freenect_init(&dev->kinect.fctx, NULL) < 0) {
        gm_throw(dev->log, err, "Failed to init libfreenect\n");
        dev->kinect.fctx = NULL;
        return false;
    }

    /* We get loads of 'errors' from the kinect but it seems to vaguely
     * be working :)
     */
    freenect_set_log_level(dev->kinect.fctx, FREENECT_LOG_FATAL);
    freenect_select_subdevices(dev->kinect.fctx,
                               (freenect_device_flags)(FREENECT_DEVICE_MOTOR |
                                                       FREENECT_DEVICE_CAMERA));

    if (!freenect_num_devices(dev->kinect.fctx)) {
        gm_throw(dev->log, err, "Failed to find a Kinect device\n");
        freenect_shutdown(dev->kinect.fctx);
        dev->kinect.fctx = NULL;
        return false;
    }

    if (freenect_open_device(dev->kinect.fctx, &dev->kinect.fdev, 0) < 0) {
        gm_throw(dev->log, err, "Could not open Kinect device\n");
        freenect_shutdown(dev->kinect.fctx);
        dev->kinect.fctx = NULL;
        return false;
    }

    freenect_set_user(dev->kinect.fdev, dev);

    dev->kinect.ir_brightness = freenect_get_ir_brightness(dev->kinect.fdev);

    freenect_raw_tilt_state *tilt_state;
    freenect_update_tilt_state(dev->kinect.fdev);
    tilt_state = freenect_get_tilt_state(dev->kinect.fdev);

    dev->kinect.phys_tilt = freenect_get_tilt_degs(tilt_state);
    dev->kinect.req_tilt = dev->kinect.phys_tilt;

    /* libfreenect doesn't give us a way to query camera intrinsics so just
     * using these random/plausible intrinsics found on the internet to avoid
     * manually calibrating for now :)
     */
    dev->depth_format = GM_FORMAT_Z_U16_MM;
    dev->max_depth_pixels = 640 * 480;
    dev->depth_intrinsics.width = 640;
    dev->depth_intrinsics.height = 480;
    dev->depth_intrinsics.cx = 339.30780975300314;
    dev->depth_intrinsics.cy = 242.73913761751615;
    dev->depth_intrinsics.fx = 594.21434211923247;
    dev->depth_intrinsics.fy = 591.04053696870778;
    dev->depth_intrinsics.distortion_model = GM_DISTORTION_NONE;

    /* We're going to use Freenect's registered depth mode, which transforms
     * depth to video space, so we don't need video intrinsics/extrinsics.
     */
    dev->video_format = GM_FORMAT_RGB_U8;
    dev->max_video_pixels = 640 * 480;
    dev->video_intrinsics = dev->depth_intrinsics;

    dev->depth_to_video_extrinsics.rotation[0] = 1.f;
    dev->depth_to_video_extrinsics.rotation[1] = 0.f;
    dev->depth_to_video_extrinsics.rotation[2] = 0.f;
    dev->depth_to_video_extrinsics.rotation[3] = 0.f;
    dev->depth_to_video_extrinsics.rotation[4] = 1.f;
    dev->depth_to_video_extrinsics.rotation[5] = 0.f;
    dev->depth_to_video_extrinsics.rotation[6] = 0.f;
    dev->depth_to_video_extrinsics.rotation[7] = 0.f;
    dev->depth_to_video_extrinsics.rotation[8] = 1.f;

    dev->depth_to_video_extrinsics.translation[0] = 0.f;
    dev->depth_to_video_extrinsics.translation[1] = 0.f;
    dev->depth_to_video_extrinsics.translation[2] = 0.f;

    /* Alternative video intrinsics/extrinsics when not using registered mode.
     * Note, these unfortunately don't actually work.
     */
#if 0
    dev->video_intrinsics.width = 640;
    dev->video_intrinsics.height = 480;
    dev->video_intrinsics.cx = 328.94272028759258;
    dev->video_intrinsics.cy = 267.48068171871557;
    dev->video_intrinsics.fx = 529.21508098293293;
    dev->video_intrinsics.fy = 525.56393630057437;

    dev->depth_to_video_extrinsics.rotation[0] = 0.99984628826577793;
    dev->depth_to_video_extrinsics.rotation[1] = 0.0012635359098409581;
    dev->depth_to_video_extrinsics.rotation[2] = -0.017487233004436643;
    dev->depth_to_video_extrinsics.rotation[3] = -0.0014779096108364480;
    dev->depth_to_video_extrinsics.rotation[4] = 0.99992385683542895;
    dev->depth_to_video_extrinsics.rotation[5] = -0.012251380107679535;
    dev->depth_to_video_extrinsics.rotation[6] = 0.017470421412464927;
    dev->depth_to_video_extrinsics.rotation[7] = 0.012275341476520762;
    dev->depth_to_video_extrinsics.rotation[8] = 0.99977202419716948;

    dev->depth_to_video_extrinsics.translation[0] = 0.019985242312092553;
    dev->depth_to_video_extrinsics.translation[1] = -0.00074423738761617583;
    dev->depth_to_video_extrinsics.translation[2] = -0.010916736334336222;
#endif

    /* Some alternative intrinsics
     *
     * TODO: we should allow explicit calibrarion and loading these at runtime
     */
#if 0
    dev->depth_intrinsics.cx = 322.515987;
    dev->depth_intrinsics.cy = 259.055966;
    dev->depth_intrinsics.fx = 521.179233;
    dev->depth_intrinsics.fy = 493.033034;

#endif

    freenect_set_video_callback(dev->kinect.fdev, kinect_rgb_frame_cb);
    freenect_set_video_mode(dev->kinect.fdev,
                            freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM,
                                                     FREENECT_VIDEO_RGB));
    dev->video_buf_back = mem_pool_acquire_buffer(dev->video_buf_pool, "kinect rgb");
    freenect_set_video_buffer(dev->kinect.fdev, dev->video_buf_back->base.data);

    freenect_set_depth_callback(dev->kinect.fdev, kinect_depth_frame_cb);
    freenect_set_depth_mode(dev->kinect.fdev,
                            freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM,
                            FREENECT_DEPTH_REGISTERED)); // MM, aligned to RGB
    /*freenect_set_depth_mode(dev->kinect.fdev,
                            freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM,
                                                     FREENECT_DEPTH_MM));*/
    dev->depth_buf_back = mem_pool_acquire_buffer(dev->depth_buf_pool, "kinect depth");
    freenect_set_depth_buffer(dev->kinect.fdev, dev->depth_buf_back->base.data);


    struct gm_ui_property prop;

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "ir_brightness";
    prop.desc = "IR Brightness";
    prop.type = GM_PROPERTY_INT;
    prop.int_state.ptr = &dev->kinect.ir_brightness;
    prop.int_state.min = 0;
    prop.int_state.max = 50;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "request_tilt";
    prop.desc = "Requested Tilt";
    prop.type = GM_PROPERTY_FLOAT;
    prop.float_state.ptr = &dev->kinect.req_tilt;
    prop.float_state.min = -30;
    prop.float_state.max = 30;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "physical tilt";
    prop.desc = "Current Physical Tilt";
    prop.type = GM_PROPERTY_FLOAT;
    prop.float_state.ptr = &dev->kinect.phys_tilt;
    prop.read_only = true;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "accel";
    prop.desc = "Accel";
    prop.type = GM_PROPERTY_FLOAT_VEC3;
    prop.vec3_state.ptr = dev->kinect.accel;
    prop.vec3_state.components[0] = "x";
    prop.vec3_state.components[1] = "y";
    prop.vec3_state.components[2] = "z";
    prop.read_only = true;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "mks_accel";
    prop.desc = "MKS Accel";
    prop.type = GM_PROPERTY_FLOAT_VEC3;
    prop.vec3_state.ptr = dev->kinect.mks_accel;
    prop.vec3_state.components[0] = "x";
    prop.vec3_state.components[1] = "y";
    prop.vec3_state.components[2] = "z";
    prop.read_only = true;
    dev->properties.push_back(prop);

    return true;
}

static void
kinect_close(struct gm_device *dev)
{
    /* XXX: can assume the device has been stopped */

    if (dev->kinect.fdev)
        freenect_close_device(dev->kinect.fdev);
    if (dev->kinect.fctx)
        freenect_shutdown(dev->kinect.fctx);
}

static void *
kinect_io_thread_cb(void *data)
{
    struct gm_device *dev = (struct gm_device *)data;
    int state_check_throttle = 0;

    freenect_set_tilt_degs(dev->kinect.fdev, 0);
    freenect_set_led(dev->kinect.fdev, LED_RED);

    freenect_start_depth(dev->kinect.fdev);
    freenect_start_video(dev->kinect.fdev);

    while (dev->running &&
           freenect_process_events(dev->kinect.fctx) >= 0)
    {
        if (state_check_throttle++ >= 2000) {
            freenect_raw_tilt_state* state;
            freenect_update_tilt_state(dev->kinect.fdev);
            state = freenect_get_tilt_state(dev->kinect.fdev);

            dev->kinect.accel[0] = state->accelerometer_x;
            dev->kinect.accel[1] = state->accelerometer_y;
            dev->kinect.accel[2] = state->accelerometer_z;

            double mks_dx, mks_dy, mks_dz;
            freenect_get_mks_accel(state, &mks_dx, &mks_dy, &mks_dz);

            dev->kinect.mks_accel[0] = mks_dx;
            dev->kinect.mks_accel[1] = mks_dy;
            dev->kinect.mks_accel[2] = mks_dz;

            dev->kinect.phys_tilt = freenect_get_tilt_degs(state);
            if (dev->kinect.phys_tilt != dev->kinect.req_tilt) {
                freenect_set_tilt_degs(dev->kinect.fdev, dev->kinect.req_tilt);
            }

            int brightness = freenect_get_ir_brightness(dev->kinect.fdev);
            if (brightness != dev->kinect.ir_brightness) {
                freenect_set_ir_brightness(dev->kinect.fdev,
                                           dev->kinect.ir_brightness);
            }

            state_check_throttle = 0;
        }
    }

    freenect_stop_depth(dev->kinect.fdev);
    freenect_stop_video(dev->kinect.fdev);

    return NULL;
}

static void
kinect_start(struct gm_device *dev)
{
    /* Set running before starting thread, otherwise it would exit immediately */
    dev->running = true;
    pthread_create(&dev->kinect.io_thread,
                   NULL, //attributes
                   kinect_io_thread_cb,
                   dev); //data
#ifdef __linux__
    pthread_setname_np(dev->kinect.io_thread, "Kinect IO");
#endif
}

static void
kinect_stop(struct gm_device *dev)
{
    void *retval = NULL;

    /* After setting running = false we expect the thread to exit within a
     * finite amount of time */
    dev->running = false;

    int ret = pthread_join(dev->kinect.io_thread, &retval);
    if (ret < 0) {
        gm_error(dev->log, "Failed to wait for Kinect IO thread to exit: %s",
                 strerror(ret));
        return;
    }

    if (retval != NULL) {
        gm_error(dev->log, "Kinect IO thread exited with error: %d",
                 (int)(intptr_t)retval);
    }
}
#endif // USE_FREENECT

static void
read_json_intrinsics(JSON_Object *json_intrinsics,
                     struct gm_intrinsics *intrinsics)
{
    /* E.g. ensures a default distortion model in case it wasn't
     * included in json... */
    memset(intrinsics, 0, sizeof(*intrinsics));

    intrinsics->width = (uint32_t)round(
        json_object_get_number(json_intrinsics, "width"));
    intrinsics->height = (uint32_t)round(
        json_object_get_number(json_intrinsics, "height"));
    intrinsics->fx = json_object_get_number(json_intrinsics, "fx");
    intrinsics->fy = json_object_get_number(json_intrinsics, "fy");
    intrinsics->cx = json_object_get_number(json_intrinsics, "cx");
    intrinsics->cy = json_object_get_number(json_intrinsics, "cy");

    intrinsics->distortion_model = (enum gm_distortion_model)
        json_object_get_number(json_intrinsics, "distortion_model");
    JSON_Array *coeffs =
        json_object_get_array(json_intrinsics, "distortion_coefficients");
    if (coeffs) {
        int n_coeffs = std::min(json_array_get_count(coeffs),
                                ARRAY_LEN(intrinsics->distortion));
        for (int i = 0; i < n_coeffs; i++) {
            intrinsics->distortion[i] = json_array_get_number(coeffs, i);
        }
    }
}

static void
recording_playpause(struct gm_ui_property *prop)
{
    struct gm_device *dev = (struct gm_device *)prop->object;

    if (!dev->running) {
        return;
    }

    if (dev->recording.loop) {
        dev->recording.max_frame = dev->recording.frame;
        dev->recording.loop = false;
    } else if (dev->recording.max_frame >= dev->recording.frame) {
        dev->recording.max_frame = -1;
        dev->recording.loop = true;
    }
}

static void
recording_step_back(struct gm_ui_property *prop)
{
    struct gm_device *dev = (struct gm_device *)prop->object;

    if (!dev->running || dev->recording.frame < 1) {
        return;
    }

    --dev->recording.frame;
    dev->recording.max_frame = dev->recording.frame;
    if (dev->recording.frame) {
        --dev->recording.frame;
    }
    dev->recording.loop = false;
    dev->recording.ignore_loop = true;
}

static void
recording_step_forward(struct gm_ui_property *prop)
{
    struct gm_device *dev = (struct gm_device *)prop->object;

    if (!dev->running) {
        return;
    }

    dev->recording.max_frame = dev->recording.frame + 1;
    dev->recording.loop = false;
    dev->recording.ignore_loop = true;
}

static bool
recording_open(struct gm_device *dev,
               struct gm_device_config *config, char **err)
{
    const char *recording_name = "glimpse_recording.json";
    size_t json_path_size = strlen(config->recording.path) +
                                   strlen(recording_name) + 2;
    char *json_path = (char *)alloca(json_path_size);
    snprintf(json_path, json_path_size, "%s/%s",
             config->recording.path, recording_name);

    dev->recording.path = strdup(config->recording.path);
    dev->recording.json = json_parse_file(json_path);
    if (!dev->recording.json) {
        gm_throw(dev->log, err, "Failed to open recording metadata");
        return false;
    }

    JSON_Object *meta = json_object(dev->recording.json);


    /* Since recordings now associate intrinsics with every frame we won't
     * necessarily find intrinsics here...
     */
    JSON_Object *depth_intrinsics =
        json_object_get_object(meta, "depth_intrinsics");
    if (depth_intrinsics) {
        read_json_intrinsics(depth_intrinsics, &dev->depth_intrinsics);
        dev->max_depth_pixels = dev->depth_intrinsics.width *
            dev->depth_intrinsics.height;
        dev->recording.fixed_intrinsics = true;
    } else {
        dev->max_depth_pixels =
            json_object_get_number(meta, "max_depth_pixels");
    }

    JSON_Object *video_intrinsics =
        json_object_get_object(meta, "video_intrinsics");
    if (video_intrinsics) {
        read_json_intrinsics(video_intrinsics, &dev->video_intrinsics);
        dev->max_video_pixels = dev->video_intrinsics.width *
            dev->video_intrinsics.height;
        gm_assert(dev->log, dev->recording.fixed_intrinsics,
                  "Inconsistently fixed depth/video intrinsics");
    } else {
        dev->max_video_pixels =
            json_object_get_number(meta, "max_video_pixels");
    }

    JSON_Object *extrinsics =
        json_object_get_object(meta, "depth_to_video_extrinsics");
    JSON_Array *rotation = json_object_get_array(extrinsics, "rotation");
    for (int i = 0; i < 9; ++i) {
        dev->depth_to_video_extrinsics.rotation[i] =
            (float)json_array_get_number(rotation, i);
    }
    JSON_Array *translation = json_object_get_array(extrinsics, "translation");
    for (int i = 0; i < 3; ++i) {
        dev->depth_to_video_extrinsics.translation[i] =
            (float)json_array_get_number(translation, i);
    }

    dev->depth_format = (enum gm_format)
        round(json_object_get_number(meta, "depth_format"));
    dev->video_format = (enum gm_format)
        round(json_object_get_number(meta, "video_format"));

    dev->recording.frame = 0;

    JSON_Array *frames =
        json_object_get_array(json_object(dev->recording.json), "frames");
    int n_recorded_frames = json_array_get_count(frames);

    struct gm_ui_property prop;

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "frame";
    prop.desc = "Frame";
    prop.type = GM_PROPERTY_INT;
    prop.int_state.ptr = &dev->recording.frame;
    prop.int_state.min = 0;
    prop.int_state.max = n_recorded_frames - 1;
    prop.read_only = true;
    dev->properties.push_back(prop);

    dev->recording.loop = true;
    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "loop";
    prop.desc = "Loop Playback";
    prop.type = GM_PROPERTY_BOOL;
    prop.bool_state.ptr = &dev->recording.loop;
    dev->properties.push_back(prop);

    dev->recording.max_frame = -1;
    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "max frame";
    prop.desc = "Maximum frame number to replay";
    prop.type = GM_PROPERTY_INT;
    prop.int_state.ptr = &dev->recording.max_frame;
    prop.int_state.min = -1;
    prop.int_state.max = n_recorded_frames - 1;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "<<";
    prop.desc = "Step back a frame";
    prop.type = GM_PROPERTY_SWITCH;
    prop.switch_state.set = recording_step_back;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "||>";
    prop.desc = "Toggle playing state";
    prop.type = GM_PROPERTY_SWITCH;
    prop.switch_state.set = recording_playpause;
    dev->properties.push_back(prop);

    prop = gm_ui_property();
    prop.object = dev;
    prop.name = ">>";
    prop.desc = "Step forward a frame";
    prop.type = GM_PROPERTY_SWITCH;
    prop.switch_state.set = recording_step_forward;
    dev->properties.push_back(prop);

    return true;
}

static void
recording_close(struct gm_device *dev)
{
    if (dev->recording.last_depth_buf) {
        gm_buffer_unref(dev->recording.last_depth_buf);
        dev->recording.last_depth_buf = nullptr;
    }
    if (dev->recording.last_video_buf) {
        gm_buffer_unref(dev->recording.last_video_buf);
        dev->recording.last_video_buf = nullptr;
    }
    if (dev->recording.path) {
        free(dev->recording.path);
        dev->recording.path = nullptr;
    }
    if (dev->recording.json) {
        json_value_free(dev->recording.json);
        dev->recording.json = nullptr;
    }
}

static struct gm_buffer *
read_frame_buffer(struct gm_device *dev,
                  JSON_Object *frame,
                  const char *filename_prop,
                  const char *len_prop,
                  const char *intrinsics_prop,
                  struct gm_intrinsics *intrinsics_out,
                  struct gm_mem_pool *buf_pool)
{
    size_t base_path_len = strlen(dev->recording.path);
    const char *filename = json_object_get_string(frame, filename_prop);
    if (!filename)
        return NULL;

    size_t abs_filename_size = strlen(filename) + base_path_len + 2;
    char *abs_filename = (char *)alloca(abs_filename_size);
    snprintf(abs_filename, abs_filename_size, "%s/%s",
             dev->recording.path, filename);

    size_t len = (size_t)json_object_get_number(frame, len_prop);

    FILE *fp = fopen(abs_filename, "r");
    if (!fp) {
        gm_error(dev->log, "Failed to open recording frame '%s'\n",
                 abs_filename);
        return NULL;
    }

    struct gm_buffer *buf = (struct gm_buffer *)
        mem_pool_acquire_buffer(buf_pool, "recording buffer");

    if (fread(buf->data, 1, len, fp) != len) {
        gm_error(dev->log, "Failed to open recording frame '%s'\n",
                 abs_filename);
        mem_pool_recycle_resource(buf_pool, buf);
        fclose(fp);
        return NULL;
    }

    buf->len = len;

    fclose(fp);

    if (dev->recording.fixed_intrinsics) {
        /* XXX: Feels a bit kludgy... */
        if (strcmp(intrinsics_prop, "depth_intrinsics") == 0) {
            *intrinsics_out = dev->depth_intrinsics;
        } else {
            gm_assert(dev->log, strcmp(intrinsics_prop, "video_intrinsics") == 0,
                      "unknown intrinsics prop");
            *intrinsics_out = dev->video_intrinsics;
        }
    } else {
        JSON_Object *intrinsics =
            json_object_get_object(frame, intrinsics_prop);
        read_json_intrinsics(intrinsics, intrinsics_out);
    }

    return buf;
}

static struct gm_buffer *
copy_device_buffer(struct gm_device *dev,
                   struct gm_device_buffer *buffer)
{
    struct gm_mem_pool *pool = buffer->pool;

    struct gm_buffer *copy = (struct gm_buffer *)
        mem_pool_acquire_buffer(pool, "paused recording buffer");

    copy->len = buffer->base.len;
    memcpy(copy->data, buffer->base.data, buffer->base.len);

    return copy;
}

static void
swap_recorded_frame(struct gm_device *dev,
                    uint64_t timestamp,
                    struct gm_pose &pose,
                    enum gm_rotation camera_rotation,
                    struct gm_buffer *depth_buffer,
                    struct gm_intrinsics *depth_intrinsics,
                    struct gm_buffer *video_buffer,
                    struct gm_intrinsics *video_intrinsics)
{
        pthread_mutex_lock(&dev->swap_buffers_lock);

        dev->recording.last_camera_rotation = camera_rotation;
        dev->camera_rotation = camera_rotation;

        dev->frame_time = timestamp;
        dev->frame_pose = pose;

        if (depth_buffer) {
            dev->depth_intrinsics = *depth_intrinsics;

            gm_assert(dev->log,
                      depth_intrinsics->width > 0 &&
                      depth_intrinsics->height > 0,
                      "swapping recorded frame with invalid depth intrinsics");

            if (dev->recording.last_depth_buf)
                gm_buffer_unref(dev->recording.last_depth_buf);
            dev->recording.last_depth_buf = gm_buffer_ref(depth_buffer);

            if (dev->frame_request_buffers_mask & GM_REQUEST_FRAME_DEPTH) {
                struct gm_device_buffer *old = dev->depth_buf_ready;
                dev->depth_buf_ready = (struct gm_device_buffer *)gm_buffer_ref(depth_buffer);
                dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_DEPTH;
                if (old)
                    gm_buffer_unref(&old->base);
            }
        }

        if (video_buffer) {
            dev->video_intrinsics = *video_intrinsics;

            gm_assert(dev->log,
                      video_intrinsics->width > 0 &&
                      video_intrinsics->height > 0,
                      "swapping recorded frame with invalid video intrinsics");

            if (dev->recording.last_video_buf)
                gm_buffer_unref(dev->recording.last_video_buf);
            dev->recording.last_video_buf = gm_buffer_ref(video_buffer);

            if (dev->frame_request_buffers_mask & GM_REQUEST_FRAME_VIDEO) {
                struct gm_device_buffer *old = dev->video_buf_ready;
                dev->video_buf_ready = (struct gm_device_buffer *)gm_buffer_ref(video_buffer);
                dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_VIDEO;
                if (old)
                    gm_buffer_unref(&old->base);
            }
        }

        pthread_mutex_unlock(&dev->swap_buffers_lock);

        pthread_mutex_lock(&dev->request_buffers_mask_lock);
        maybe_notify_frame_locked(dev);
        pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static void *
recording_io_thread_cb(void *userdata)
{
    struct gm_device *dev = (struct gm_device *)userdata;

    gm_debug(dev->log, "Started recording IO thread");

    JSON_Array *frames =
        json_object_get_array(json_object(dev->recording.json), "frames");
    JSON_Object *frame0 = json_array_get_object(frames, 0);
    int n_recorded_frames = json_array_get_count(frames);

    uint64_t frame0_timestamp = (uint64_t)
            json_object_get_number(frame0, "timestamp");

    struct gm_pose pose;
    JSON_Object *json_pose = json_object_get_object(frame0, "pose");
    if (json_pose) {
        JSON_Array *orientation = json_object_get_array(json_pose,
                                                        "orientation");
        for (int i = 0; i < 4; ++i) {
            pose.orientation[i] = (float)json_array_get_number(orientation, i);
        }
        JSON_Array *translation = json_object_get_array(json_pose,
                                                        "translation");
        for (int i = 0; i < 3; ++i) {
            pose.translation[i] = (float)json_array_get_number(translation, i);
        }
        pose.valid = true;
    }

    /* Even though the recording loops and the playback can be paused
     * we still guarantee a monotonic increasing clock for each frame.
     *
     * This is the clock we maintain by adding frame deltas to it.
     */
    uint64_t monotonic_clock = get_time();

    /* We want to play back in real-time so at the start of playback
     * we update this reference point for the real wall clock time.
     */
    uint64_t loop_start = get_time();

    /* Our monotonic timestamps are derived by calculating the delta
     * between sequential frames. This tracks the previous frame's
     * timestamp for calculating a delta.
     *
     * This resets at the start of each loop and when we pause playback
     * (i.e. it's not monotonic)
     */
    uint64_t loop_prev_frame_timestamp = frame0_timestamp;

    while (dev->running) {
        int n_frames = dev->recording.max_frame >= 0 ?
            std::min(dev->recording.max_frame + 1, n_recorded_frames) :
            n_recorded_frames;

        if (dev->recording.frame >= (n_frames - 1)) {

            /* Enter paused state if looping has been disabled... */
            while (dev->running && dev->recording.loop == false &&
                   dev->recording.ignore_loop == false) {
                struct gm_buffer *depth_buffer = NULL;
                struct gm_buffer *video_buffer = NULL;

                /* We don't just keep 'swapping' in the same buffer since that
                 * will likely confuse downstream logic. For example with
                 * our bread crumb debugging for tracking the lifecycle of
                 * of frames and buffers we don't expect very long lived buffers
                 * with a never-ending debug trail to track.
                 */
                if (dev->recording.last_depth_buf) {
                    depth_buffer = copy_device_buffer(dev,
                                                      (struct gm_device_buffer *)
                                                      dev->recording.last_depth_buf);
                }

                if (dev->recording.last_video_buf) {
                    video_buffer = copy_device_buffer(dev,
                                                      (struct gm_device_buffer *)
                                                      dev->recording.last_video_buf);
                }

                monotonic_clock += 16000000;
                loop_start += 16000000;

                swap_recorded_frame(dev,
                                    monotonic_clock,
                                    pose,
                                    dev->recording.last_camera_rotation,
                                    depth_buffer,
                                    &dev->depth_intrinsics,
                                    video_buffer,
                                    &dev->video_intrinsics);

                if (depth_buffer)
                    gm_buffer_unref(depth_buffer);
                if (video_buffer)
                    gm_buffer_unref(video_buffer);

                usleep(16000);
            }

            /* Recalculate n_frames as it may have changed during the loop */
            n_frames = dev->recording.max_frame >= 0 ?
                std::min(dev->recording.max_frame + 1, n_recorded_frames) :
                n_recorded_frames;
            if (dev->recording.frame >= n_frames - 1) {
                /* Note: the subtraction makes it look like the loop closure
                 * from end to re-start takes some time.
                 */
                loop_start = get_time();
                loop_prev_frame_timestamp = frame0_timestamp - 16000000;
                dev->recording.frame = 0;
            }
        } else
            dev->recording.frame++;

        /* This is only used to break out of the while loop above when loop
         * is set to false. It's safe to reset it after that loop.
         */
        dev->recording.ignore_loop = false;

        uint64_t time = get_time();
        uint64_t real_progress = time - loop_start;

        JSON_Object *frame = json_array_get_object(frames, dev->recording.frame);
        uint64_t frame_timestamp = (uint64_t)
            json_object_get_number(frame, "timestamp");
        uint64_t recording_progress = frame_timestamp - frame0_timestamp;

        json_pose = json_object_get_object(frame, "pose");
        if (json_pose) {
            JSON_Array *orientation = json_object_get_array(json_pose,
                                                            "orientation");
            for (int i = 0; i < 4; ++i) {
                pose.orientation[i] = (float)
                    json_array_get_number(orientation, i);
            }
            JSON_Array *translation = json_object_get_array(json_pose,
                                                            "translation");
            for (int i = 0; i < 3; ++i) {
                pose.translation[i] = (float)
                    json_array_get_number(translation, i);
            }
            pose.valid = true;
        } else {
            pose.valid = false;
        }

        /* XXX: Skip frames if we're > 33ms behind */
        if (recording_progress < (real_progress - 33333333)) {
            gm_warn(dev->log, "slow playback, skipping recorded frames");

            int last_depth = -1;
            int i;
            for (i = dev->recording.frame + 1;
                 i < n_frames && (recording_progress < real_progress);
                 i++)
            {
                frame = json_array_get_object(frames, i);
                frame_timestamp = (uint64_t)
                    json_object_get_number(frame, "timestamp");
                recording_progress = frame_timestamp - frame0_timestamp;

                /* If we're skipping frames that's likely due to the size of
                 * video buffers we're loading.
                 *
                 * If depth has been requested then we prioritize the most
                 * recent frame with depth considering that depth is a hard
                 * requirement for tracking and there will be fewer depth
                 * frames than video frames typically so we would likely keep
                 * skipping over them unable to do any tracking.
                 */
                if (dev->frame_request_buffers_mask & GM_REQUEST_FRAME_DEPTH &&
                    json_object_get_string(frame, "depth_file"))
                {
                    last_depth = i;
                }
            }

            if (i >= n_frames) {
                /* If we've skipped to the end of the recording at least keep
                 * the last frame without immediately looping so we don't have
                 * more than one place to handle looping and don't have to
                 * consider a special case that can continue; before hitting
                 * the swap_buffers below.
                 */
                dev->recording.frame = n_frames - 1;
            } else if (last_depth > 0 && last_depth != i) {
                /* jump back if we need to prioritize an earlier depth frame */
                dev->recording.frame = last_depth;
                frame = json_array_get_object(frames, last_depth);
                frame_timestamp = (uint64_t)
                    json_object_get_number(frame, "timestamp");
                recording_progress = frame_timestamp - frame0_timestamp;
            } else
                dev->recording.frame = i;
        }

        gm_debug(dev->log, "replaying frame %d", dev->recording.frame);

        uint64_t frame_delta;
        if (frame_timestamp < loop_prev_frame_timestamp) {
            gm_error(dev->log, "Recorded frame timestamps went backwards");
            frame_delta = 16000000;
        } else {
            frame_delta = frame_timestamp - loop_prev_frame_timestamp;
        }
        monotonic_clock += frame_delta;
        loop_prev_frame_timestamp = frame_timestamp;

        /* Throttle playback according to the timestamps in the recorded frames */
        while (recording_progress > real_progress) {
            uint64_t delay_us = (recording_progress - real_progress) / 1000;
            usleep(delay_us);
            time = get_time();
            real_progress = time - loop_start;
        }

        struct gm_intrinsics depth_intrinsics;
        struct gm_buffer *depth_buffer = read_frame_buffer(dev,
                                                           frame,
                                                           "depth_file",
                                                           "depth_len",
                                                           "depth_intrinsics",
                                                           &depth_intrinsics,
                                                           dev->depth_buf_pool);
        struct gm_intrinsics video_intrinsics;
        struct gm_buffer *video_buffer = read_frame_buffer(dev,
                                                           frame,
                                                           "video_file",
                                                           "video_len",
                                                           "video_intrinsics",
                                                           &video_intrinsics,
                                                           dev->video_buf_pool);
        enum gm_rotation rotation = (enum gm_rotation)
            json_object_get_number(frame, "camera_rotation");

        swap_recorded_frame(dev,
                            monotonic_clock,
                            pose,
                            rotation,
                            depth_buffer,
                            &depth_intrinsics,
                            video_buffer,
                            &video_intrinsics);

        if (depth_buffer)
            gm_buffer_unref(depth_buffer);
        if (video_buffer)
            gm_buffer_unref(video_buffer);
    }

    return NULL;
}

static void
recording_start(struct gm_device *dev)
{
    dev->recording.frame = 0;

    /* Set running before starting thread, otherwise it would exit immediately */
    dev->running = true;
    pthread_create(&dev->recording.io_thread,
                   NULL,
                   recording_io_thread_cb,
                   dev);
#ifdef __linux__
    pthread_setname_np(dev->recording.io_thread, "Recording IO");
#endif
}

static void
recording_stop(struct gm_device *dev)
{
    void *retval = NULL;

    /* After setting running = false we expect the thread to exit within a
     * finite amount of time */
    dev->running = false;

    int ret = pthread_join(dev->recording.io_thread, &retval);
    if (ret < 0) {
        gm_error(dev->log, "Failed to wait for recording IO thread to exit: %s",
                 strerror(ret));
        return;
    }

    if (retval != NULL) {
        gm_error(dev->log, "Recording IO thread exited with error: %d",
                 (int)(intptr_t)retval);
    } else {
        gm_debug(dev->log, "Successfully joined recording io thread");
    }
}

static void
notify_device_ready(struct gm_device *dev)
{
    struct gm_device_event *event = device_event_alloc(dev, GM_DEV_EVENT_READY);

    dev->event_callback(event, dev->callback_data);
}

#ifdef USE_TANGO
static bool
tango_open(struct gm_device *dev, struct gm_device_config *config, char **err)
{
    gm_debug(dev->log, "Tango Device Open");

    /* We wait until _configure() time before doing much because we want to
     * allow the device to be configured with an event callback first
     * so we will be able to notify that the device is ready if the Tango
     * service has already been bound.
     */

    return true;
}

static void
tango_close(struct gm_device *dev)
{
    gm_debug(dev->log, "Tango Device Close");
}

static void
tango_point_cloud_cb(void *context, const TangoPointCloud *point_cloud)
{
    struct gm_device *dev = (struct gm_device *)context;

    gm_debug(dev->log, "tango_point_cloud_cb");

    if (!(dev->frame_request_buffers_mask & GM_REQUEST_FRAME_DEPTH)) {
        gm_debug(dev->log, "> tango_point_cloud_cb: depth not needed");
        return;
    }

    /* FIXME: explicitly enable/disable callbacks via Tango API somehow */
    if (!dev->running) {
        gm_debug(dev->log, "> tango_point_cloud_cb: not running");
        return;
    }

    struct gm_device_buffer *depth_buf_back =
        mem_pool_acquire_buffer(dev->depth_buf_pool, "tango depth");

    gm_assert(dev->log,
              point_cloud->num_points <= dev->max_depth_pixels,
              "Spurious Tango Point Cloud larger than sensor resolution (%d > max=%d)",
              point_cloud->num_points,
              dev->max_depth_pixels);

    memcpy(depth_buf_back->base.data,
           point_cloud->points,
           point_cloud->num_points * 4 * sizeof(float));
    depth_buf_back->base.len = point_cloud->num_points * 4 * sizeof(float);

    TangoPoseData pose;
    TangoErrorType error = TangoSupport_getPoseAtTime(point_cloud->timestamp,
        TANGO_COORDINATE_FRAME_CAMERA_DEPTH,
        TANGO_COORDINATE_FRAME_START_OF_SERVICE,
        TANGO_SUPPORT_ENGINE_OPENGL,
        TANGO_SUPPORT_ENGINE_OPENGL,
        (TangoSupportRotation)dev->tango.display_rotation,
        &pose);

    pthread_mutex_lock(&dev->swap_buffers_lock);

    struct gm_device_buffer *old = dev->depth_buf_ready;
    dev->depth_buf_ready = depth_buf_back;
    dev->frame_time = (uint64_t)(point_cloud->timestamp * 1e9);
    if (error == TANGO_SUCCESS) {
        dev->frame_pose = {
            true,
            { (float)-pose.orientation[0],
              (float)pose.orientation[1],
              (float)pose.orientation[2],
              (float)pose.orientation[3] },
            { (float)-pose.translation[0],
              (float)pose.translation[1],
              (float)pose.translation[2] }
        };
    } else {
        gm_debug(dev->log, "tango_point_cloud_cb invalid pose");
        dev->frame_pose.valid = false;
    }
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_DEPTH;
    gm_debug(dev->log, "tango_point_cloud_cb depth ready = %p", dev->depth_buf_ready);

    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

// This function does nothing. TangoService_connectOnTextureAvailable
// requires a callback function pointer, and it cannot be null.
static void
tango_texture_available_cb(void *context, TangoCameraId id)
{
    //struct gm_device *dev = (struct gm_device *)context;
}

#if 0
static int
tango_format_bits_per_pixel(struct gm_device *dev, TangoImageFormatType format)
{
    switch (format) {
    case TANGO_HAL_PIXEL_FORMAT_RGBA_8888:
        gm_assert(dev->log, 0, "Unhandled Tango Camera Format (RGBA_8888)");
        return 32;
    case TANGO_HAL_PIXEL_FORMAT_YV12:
        gm_assert(dev->log, 0, "Unhandled Tango Camera Format (YV12)");
        return 12;
    case TANGO_HAL_PIXEL_FORMAT_YCrCb_420_SP:
        return 12;
    }

    gm_assert(dev->log, 0, "Should not be reached");
}
#endif

/* FIXME: we should avoid needing to copy the video buffers here by only
 * sampling the video as a GL texture.
 */
static void
tango_frame_available_cb(void *context,
                         TangoCameraId id,
                         const TangoImageBuffer *buffer)
{
    struct gm_device *dev = (struct gm_device *)context;

    gm_debug(dev->log, "tango_frame_available_cb");

    if (!(dev->frame_request_buffers_mask & GM_REQUEST_FRAME_VIDEO)) {
        gm_debug(dev->log, "> tango_frame_available_cb: VIDEO not required");
        return;
    }

    /* FIXME: explicitly enable/disable callbacks via Tango API somehow */
    if (!dev->running) {
        gm_debug(dev->log, "> tango_frame_available_cb: not running");
        return;
    }

    gm_assert(dev->log, buffer->format == TANGO_HAL_PIXEL_FORMAT_YCrCb_420_SP,
              "FIXME: Unhandled Tango Camera Format (only supporting NV12 currently)");
    if (buffer->format != TANGO_HAL_PIXEL_FORMAT_YCrCb_420_SP)
        return;

    struct gm_device_buffer *video_buf_back =
        mem_pool_acquire_buffer(dev->video_buf_pool, "tango video");

    /* XXX: we're just keeping the luminance for now... */
    memcpy(video_buf_back->base.data,
           buffer->data,
           buffer->width * buffer->height);

    pthread_mutex_lock(&dev->swap_buffers_lock);

    struct gm_device_buffer *old = dev->video_buf_ready;
    dev->video_buf_ready = video_buf_back;
    dev->frame_time = (uint64_t)(buffer->timestamp * 1e9);
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_VIDEO;

    gm_debug(dev->log, "tango_frame_available_cb video ready = %p", dev->video_buf_ready);

    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static bool
tango_set_up_config(struct gm_device *dev, char **err)
{
    dev->depth_format = GM_FORMAT_POINTS_XYZC_F32_M;
    dev->video_format = GM_FORMAT_LUMINANCE_U8;

    /* FIXME */
    dev->depth_to_video_extrinsics.rotation[0] = 1.f;
    dev->depth_to_video_extrinsics.rotation[1] = 0.f;
    dev->depth_to_video_extrinsics.rotation[2] = 0.f;
    dev->depth_to_video_extrinsics.rotation[3] = 0.f;
    dev->depth_to_video_extrinsics.rotation[4] = 1.f;
    dev->depth_to_video_extrinsics.rotation[5] = 0.f;
    dev->depth_to_video_extrinsics.rotation[6] = 0.f;
    dev->depth_to_video_extrinsics.rotation[7] = 0.f;
    dev->depth_to_video_extrinsics.rotation[8] = 1.f;

    dev->depth_to_video_extrinsics.translation[0] = 0.f;
    dev->depth_to_video_extrinsics.translation[1] = 0.f;
    dev->depth_to_video_extrinsics.translation[2] = 0.f;

    // The TANGO_CONFIG_DEFAULT config enables basic motion tracking capabilities.
    // In addition to motion tracking, however, we want to run with depth...
    dev->tango.tango_config = TangoService_getConfig(TANGO_CONFIG_DEFAULT);
    if (dev->tango.tango_config == nullptr) {
        gm_throw(dev->log, err, "Unable to get tango config");
        return false;
    }

    TangoErrorType ret =
        TangoConfig_setBool(dev->tango.tango_config, "config_enable_depth", true);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to enable depth.");
        return false;
    }

    ret = TangoConfig_setInt32(dev->tango.tango_config, "config_depth_mode",
                               TANGO_POINTCLOUD_XYZC);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to configure to XYZC.");
        return false;
    }

    ret = TangoConfig_setBool(dev->tango.tango_config, "config_enable_color_camera", true);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to enable color camera.");
        return false;
    }

    // Enable low latency IMU integration so that we have pose information
    // available as quickly as possible. Without setting this flag, you will
    // often receive invalid poses when calling getPoseAtTime() for an image.
    ret = TangoConfig_setBool(dev->tango.tango_config,
                              "config_enable_low_latency_imu_integration", true);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to enable low latency imu integration.");
        return false;
    }

    // Drift correction allows motion tracking to recover after it loses tracking.
    //
    // The drift corrected pose is is available through the frame pair with
    // base frame AREA_DESCRIPTION and target frame DEVICE.
    ret = TangoConfig_setBool(dev->tango.tango_config,
                              "config_enable_drift_correction", true);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,
                 "enabling config_enable_drift_correction failed with error code: %d",
                 ret);
        return false;
    }

#if 0
    if (point_cloud_manager_ == nullptr) {
        int32_t max_point_cloud_elements;
        ret = TangoConfig_getInt32(dev->tango.tango_config, "max_point_cloud_elements",
                                   &max_point_cloud_elements);
        if (ret != TANGO_SUCCESS) {
            gm_throw(dev->log, err,
                     "Failed to query maximum number of point cloud elements.");
            return false;
        }

        ret = TangoSupport_createPointCloudManager(max_point_cloud_elements,
                                                   &point_cloud_manager_);
        if (ret != TANGO_SUCCESS) {
            gm_throw(dev->log, err, "Failed to create a point cloud manager.");
            return false;
        }
    }
#endif

    // Register for depth notification.
    ret = TangoService_connectOnPointCloudAvailable(tango_point_cloud_cb);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,"Failed to connected to depth callback.");
        return false;
    }

#if 0
    // The Tango service allows you to connect an OpenGL texture directly to its
    // RGB and fisheye cameras. This is the most efficient way of receiving
    // images from the service because it avoids copies. You get access to the
    // graphic buffer directly. As we are interested in rendering the color image
    // in our render loop, we will be polling for the color image as needed.
    ret = TangoService_connectOnTextureAvailable(TANGO_CAMERA_COLOR, dev,
                                                 tango_texture_available_cb);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,
                 "Failed to connect texture callback with error code: %d",
                 ret);
        return false;
    }

    ret = TangoService_connectOnFrameAvailable(TANGO_CAMERA_COLOR, dev,
                                               tango_frame_available_cb);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,"Failed to connect to color frame callback");
        return false;
    }
#endif

    return true;
}

static enum gm_rotation
tango_infer_camera_orientation(struct gm_device *dev)
{
    float texture_coords[8] = {
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f
    };
    float rotated_texture_coords[8];

    gm_debug(dev->log, "    default UVs: %f,%f %f,%f %f,%f, %f,%f",
             texture_coords[0],
             texture_coords[1],
             texture_coords[2],
             texture_coords[3],
             texture_coords[4],
             texture_coords[5],
             texture_coords[6],
             texture_coords[7]);

    for (int i = 0; i < 4; i++) {
        TangoErrorType r =
            TangoSupport_getVideoOverlayUVBasedOnDisplayRotation(texture_coords,
                                                                 (TangoSupportRotation)i,
                                                                 rotated_texture_coords);
        if (r == TANGO_SUCCESS) {
            gm_debug(dev->log, "%3d degrees UVs: %f,%f %f,%f %f,%f, %f,%f",
                     i*90,
                     rotated_texture_coords[0],
                     rotated_texture_coords[1],
                     rotated_texture_coords[2],
                     rotated_texture_coords[3],
                     rotated_texture_coords[4],
                     rotated_texture_coords[5],
                     rotated_texture_coords[6],
                     rotated_texture_coords[7]);
        } else {
            gm_error(dev->log, "Failed to query Tango UV coords for display rotation of %d degrees",
                     90 * i);
            return GM_ROTATION_0;
        }
    }
    TangoErrorType r =
        TangoSupport_getVideoOverlayUVBasedOnDisplayRotation(texture_coords,
                                                             ROTATION_0,
                                                             rotated_texture_coords);
    if (r == TANGO_SUCCESS) {
        int i = 0;
        for (i = 0; i < 4; i++) {
            if (rotated_texture_coords[2*i] == 0 && rotated_texture_coords[2*i+1] == 0)
                break;
        }
        if (i == 4) {
            gm_error(dev->log, "Couldn't infer the camera's rotation relative to the display");
            return GM_ROTATION_0;
        }
        gm_debug(dev->log, "Tango camera is rotated %d degrees, relative to the display",
                 i * 90);
        return (enum gm_rotation)i;
    } else {
        gm_error(dev->log, "Failed to query Tango UV coords for natural display orientation");
        return GM_ROTATION_0;
    }
}

static void
tango_set_display_rotation(struct gm_device *dev, enum gm_rotation display_rotation)
{
    dev->tango.display_rotation = display_rotation;

    int camera_rotation = (int)display_rotation;
    camera_rotation += dev->tango.display_to_camera_rotation;
    camera_rotation %= 4;

    gm_debug(dev->log, "Tango camera is rotated %d degrees, relative to current display orientation (rotated %d degrees)",
             90 * camera_rotation,
             ((int)display_rotation)*90);

    dev->camera_rotation = camera_rotation;
}

static void
print_basis_and_rotated_intrinsics(struct gm_device *dev,
                                   const char *name,
                                   TangoCameraId camera_id,
                                   const TangoCameraIntrinsics *intrinsics)
{
#define DEGREES(RAD) (RAD * (360.0 / (M_PI * 2.0)))
    float hfov = 2.0 * atan(0.5 * intrinsics->width /
                                  intrinsics->fx);
    float vfov = 2.0 * atan(0.5 * intrinsics->height /
                                  intrinsics->fy);
    gm_debug(dev->log,
             "%s: %dx%d fx=%f,fy=%f (hfov=%f,vfov=%f), cx=%f,cy=%f",
             name,
             (int)intrinsics->width,
             (int)intrinsics->height,
             intrinsics->fx,
             intrinsics->fy,
             DEGREES(hfov), DEGREES(vfov),
             intrinsics->cx,
             intrinsics->cy);

    switch (intrinsics->calibration_type) {
    case TANGO_CALIBRATION_UNKNOWN:
        gm_debug(dev->log, "%s: distortion 'unknown'", name);
        break;
    case TANGO_CALIBRATION_EQUIDISTANT:
        gm_debug(dev->log, "%s: distortion 'fov-model', w=%f",
                 name, intrinsics->distortion[0]);
        break;
    case TANGO_CALIBRATION_POLYNOMIAL_2_PARAMETERS:
        gm_debug(dev->log, "%s: distortion 'brown2', k1=%f, k2=%f",
                 name,
                 intrinsics->distortion[0],
                 intrinsics->distortion[1]);
        break;
    case TANGO_CALIBRATION_POLYNOMIAL_3_PARAMETERS:
        gm_debug(dev->log, "%s: distortion 'brown3', k1=%f, k2=%f, k3=%f",
                 name,
                 intrinsics->distortion[0],
                 intrinsics->distortion[1],
                 intrinsics->distortion[2]);

        break;
    case TANGO_CALIBRATION_POLYNOMIAL_5_PARAMETERS:
        gm_debug(dev->log, "%s: distortion 'brown5', k1=%f, k2=%f, p1=%f, p2=%f, k3=%f",
                 name,
                 intrinsics->distortion[0],
                 intrinsics->distortion[1],
                 intrinsics->distortion[2],
                 intrinsics->distortion[3],
                 intrinsics->distortion[4]);
        break;
    }

    /* We print the rotated intrinsics just so we can double check they are
     * consistent with our gm_context_rotate_intrinsics() implementation
     */
    for (int i = 0; i < 4; i++) {
        TangoCameraIntrinsics rot_intrinsics;
        TangoErrorType err = TangoSupport_getCameraIntrinsicsBasedOnDisplayRotation(
            camera_id,
            (TangoSupportRotation)i,
            &rot_intrinsics);

        if (err == TANGO_SUCCESS) {
            hfov = 2.0 * atan(0.5 * intrinsics->width / intrinsics->fx);
            vfov = 2.0 * atan(0.5 * intrinsics->height / intrinsics->fy);
            gm_debug(dev->log,
                     "> @ %3d deg: %dx%d fx=%f,fy=%f (hfov=%f,vfov=%f), cx=%f,cy=%f",
                     90 * i,
                     (int)rot_intrinsics.width,
                     (int)rot_intrinsics.height,
                     rot_intrinsics.fx,
                     rot_intrinsics.fy,
                     DEGREES(hfov), DEGREES(vfov),
                     rot_intrinsics.cx,
                     rot_intrinsics.cy);
        }
    }
#undef DEGREES
}

static void
init_intrinsics_from_tango(struct gm_intrinsics *intrinsics,
                           TangoCameraIntrinsics *tango_intrinsics)
{
    intrinsics->width = tango_intrinsics->width;
    intrinsics->height = tango_intrinsics->height;
    intrinsics->fx = tango_intrinsics->fx;
    intrinsics->fy = tango_intrinsics->fy;
    intrinsics->cx = tango_intrinsics->cx;
    intrinsics->cy = tango_intrinsics->cy;

    int n_params = 0;

    switch (tango_intrinsics->calibration_type) {
    case TANGO_CALIBRATION_UNKNOWN:
        intrinsics->distortion_model = GM_DISTORTION_NONE;
        break;
    case TANGO_CALIBRATION_EQUIDISTANT:
        intrinsics->distortion_model = GM_DISTORTION_FOV_MODEL;
        n_params = 1;
        break;
    case TANGO_CALIBRATION_POLYNOMIAL_2_PARAMETERS:
        intrinsics->distortion_model = GM_DISTORTION_BROWN_K1_K2;
        n_params = 2;
        break;
    case TANGO_CALIBRATION_POLYNOMIAL_3_PARAMETERS:
        intrinsics->distortion_model = GM_DISTORTION_BROWN_K1_K2_K3;
        n_params = 3;
        break;
    case TANGO_CALIBRATION_POLYNOMIAL_5_PARAMETERS:
        intrinsics->distortion_model = GM_DISTORTION_BROWN_K1_K2_P1_P2_K3;
        n_params = 5;
        break;
    }

    for (int i = 0; i < n_params; i++)
        intrinsics->distortion[i] = tango_intrinsics->distortion[i];
}

static bool
tango_connect(struct gm_device *dev, char **err)
{
    TangoCameraIntrinsics color_camera_intrinsics;
    //TangoCameraIntrinsics rgbir_camera_intrinsics;
    TangoCameraIntrinsics depth_camera_intrinsics;

    // Here, we will connect to the TangoService and set up to run. Note that
    // we are passing in a pointer to ourselves as the context which will be
    // passed back in our callbacks.
    TangoErrorType ret = TangoService_connect(dev, dev->tango.tango_config);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to connect to the Tango service.");
        return false;
    }

    // Initialize TangoSupport context.
    TangoSupport_initialize(TangoService_getPoseAtTime,
                            TangoService_getCameraIntrinsics);

    // Get the intrinsics for the color camera and pass them on to the depth
    // image. We need these to know how to project the point cloud into the color
    // camera frame.
    ret = TangoService_getCameraIntrinsics(TANGO_CAMERA_COLOR,
                                           &color_camera_intrinsics);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,
                 "Failed to get the intrinsics for the color camera.");
        return false;
    }

    ret = TangoService_connectOnFrameAvailable(TANGO_CAMERA_COLOR, dev,
                                               tango_frame_available_cb);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,"Failed to connect to color frame callback");
        return false;
    }

    init_intrinsics_from_tango(&dev->video_intrinsics,
                               &color_camera_intrinsics);
    dev->max_video_pixels = color_camera_intrinsics.width *
        color_camera_intrinsics.height;

    print_basis_and_rotated_intrinsics(dev, "ColorCamera",
                                       TANGO_CAMERA_COLOR,
                                       &color_camera_intrinsics);

    ret = TangoService_getCameraIntrinsics(TANGO_CAMERA_DEPTH,
                                           &depth_camera_intrinsics);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err,
                 "Failed to get the intrinsics for the depth camera.");
        return false;
    }

    init_intrinsics_from_tango(&dev->depth_intrinsics,
                               &depth_camera_intrinsics);
    dev->max_depth_pixels = color_camera_intrinsics.width *
        color_camera_intrinsics.height;

    print_basis_and_rotated_intrinsics(dev, "DepthCamera",
                                       TANGO_CAMERA_DEPTH,
                                       &depth_camera_intrinsics);

    TangoCoordinateFramePair pair = { TANGO_COORDINATE_FRAME_CAMERA_COLOR,
        TANGO_COORDINATE_FRAME_CAMERA_DEPTH };
    TangoPoseData color_camera_T_depth_camera;

    TangoService_getPoseAtTime(0, //get latest
                               pair,
                               &color_camera_T_depth_camera);
    gm_debug(dev->log,
             "depth -> color camera: dx=%f,dy=%f,dz=%f, [w=%f (x=%f, y=%f, z=%f)]",
             color_camera_T_depth_camera.translation[0],
             color_camera_T_depth_camera.translation[1],
             color_camera_T_depth_camera.translation[2],
             color_camera_T_depth_camera.orientation[0],
             color_camera_T_depth_camera.orientation[1],
             color_camera_T_depth_camera.orientation[2],
             color_camera_T_depth_camera.orientation[3]);

#if 0
    /* Hitting an abort in the Tango SDK if we attempt to query intrinsics
     * with the TANGO_CAMERA_RGBIR enum
     */
    ret = TangoService_getCameraIntrinsics(TANGO_CAMERA_RGBIR,
                                           &rgbir_camera_intrinsics_);
    if (ret != TANGO_SUCCESS) {
        gm_throw(dev->log, err, "Failed to get the intrinsics for the RGB-IR "
             "camera.");
        std::exit(EXIT_SUCCESS);
    }

    dev->rgbir_intrinsics.width = rgbir_camera_intrinsics.width;
    dev->rgbir_intrinsics.height = rgbir_camera_intrinsics.height;
    dev->rgbir_intrinsics.fx = rgbir_camera_intrinsics.fx;
    dev->rgbir_intrinsics.fy = rgbir_camera_intrinsics.fy;
    dev->rgbir_intrinsics.cx = rgbir_camera_intrinsics.cx;
    dev->rgbir_intrinsics.cy = rgbir_camera_intrinsics.cy;

    float ir_hfov = 2.0 * atan(0.5 * rgbir_camera_intrinsics_.width /
                            rgbir_camera_intrinsics_.fx);
    float ir_vfov = 2.0 * atan(0.5 * rgbir_camera_intrinsics_.height /
                            rgbir_camera_intrinsics_.fy);
    LOGI("RGB-IR-Camera: %dx%d H-FOV: %f, V-FOV: %f",
         (int)rgbir_camera_intrinsics_.width,
         (int)rgbir_camera_intrinsics_.height,
         DEGREES(ir_hfov), DEGREES(ir_vfov));
#endif

    dev->tango.display_to_camera_rotation = tango_infer_camera_orientation(dev);
    tango_set_display_rotation(dev, dev->tango.display_rotation);

    return true;
}

static void
tango_set_service_binder(struct gm_device *dev, JNIEnv *jni_env, jobject binder)
{
    TangoErrorType ret = TangoService_setBinder(jni_env, binder);
    if (ret != TANGO_SUCCESS) {
        gm_debug(dev->log, "TangoService_setBinder failed");
        return;
    }

    char *err = NULL;
    if (tango_set_up_config(dev, &err) && tango_connect(dev, &err))
    {
        notify_device_ready(dev);
    } else {
        gm_error(dev->log, "Failed to configure Tango: %s", err);
        free(err);
    }
}

static bool
tango_configure(struct gm_device *dev, char **err)
{
    /* Some JNI calls set global state that will get checked while configuring
     * the device. We don't want a race between setting that global state and
     * checking that state during configuration.
     *
     * Some JNI calls will check for a non-null tango_singleton_dev and
     * notify changes via that device instead (or in addition) to setting the
     * global state.
     */
    pthread_mutex_lock(&jni_lock);

    dev->configured = true;

    gm_debug(dev->log, "Tango Device Configure");

    if (early_tango_service_binder) {
        JNIEnv *jni_env = 0;
        dev->jvm->AttachCurrentThread(&jni_env, 0);

        tango_set_service_binder(dev, jni_env, early_tango_service_binder);
    }

    /* Now that the device is configured we set the global tango_singleton_dev
     * pointer making the device visible to JNI callbacks...
     */
    gm_assert(dev->log, tango_singleton_dev == NULL,
              "Attempted to open multiple Tango devices");
    tango_singleton_dev = dev;

    pthread_mutex_unlock(&jni_lock);

    return true;
}

static void
tango_start(struct gm_device *dev)
{
    dev->running = true;
}

static void
tango_stop(struct gm_device *dev)
{
    dev->running = false;
}
#endif // USE_TANGO

#ifdef USE_AVF

static void
on_avf_configure_finished_cb(struct ios_av_session *session,
                             void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    gm_debug(dev->log, "glimpse_device: on_avf_configure_finished_cb");
    notify_device_ready(dev);
}

static void
on_avf_video_cb(struct ios_av_session *session,
                struct gm_intrinsics *intrinsics,
                int stride,
                uint8_t *video,
                void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    int width = intrinsics->width;
    int height = intrinsics->height;
    gm_debug(dev->log, "glimpse_device: on_avf_video_cb");

    if (!(dev->frame_request_buffers_mask & GM_REQUEST_FRAME_VIDEO)) {
        gm_debug(dev->log, "> on_avf_video_cb: VIDEO not required");
        return;
    }

    if (!dev->running) {
        gm_debug(dev->log, "> on_avf_video_cb: not running");
        return;
    }

    gm_assert(dev->log,
              width == 640 && height == 480 && stride == width * 4,
              "Unexpected AVF video frame size/format");

    struct gm_device_buffer *video_buf_back =
        mem_pool_acquire_buffer(dev->video_buf_pool, "avf video");

    memcpy(video_buf_back->base.data, video, stride * height);

    pthread_mutex_lock(&dev->swap_buffers_lock);

    dev->video_intrinsics = *intrinsics;

    struct gm_device_buffer *old = dev->video_buf_ready;
    dev->video_buf_ready = video_buf_back;
    // FIXME: get time from AVF
    dev->frame_time = get_time();
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_VIDEO;

    gm_debug(dev->log, "on_avf_video_cb video ready = %p", dev->video_buf_ready);

    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static void
on_avf_depth_cb(struct ios_av_session *session,
                struct gm_intrinsics *intrinsics,
                int stride,
                float *disparity,
                void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    int width = intrinsics->width;
    int height = intrinsics->height;
    gm_debug(dev->log, "glimpse_device: on_avf_depth_cb");

    struct gm_device_buffer *depth_buf_back =
        mem_pool_acquire_buffer(dev->depth_buf_pool, "avf depth");

    float *depth = (float *)depth_buf_back->base.data;
    gm_assert(dev->log,
              depth_buf_back->base.len >= width * height * 4,
              "depth buffer too small");

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int off = width * y + x;
            depth[off] = 1.0f / disparity[off];
        }
    }

    pthread_mutex_lock(&dev->swap_buffers_lock);

    dev->depth_intrinsics = *intrinsics;

    struct gm_device_buffer *old = dev->depth_buf_ready;
    dev->depth_buf_ready = depth_buf_back;
    // TODO: get timestamp from avf
    dev->frame_time = get_time();
    dev->frame_ready_buffers_mask |= GM_REQUEST_FRAME_DEPTH;
    gm_debug(dev->log, "avf depth ready = %p", dev->depth_buf_ready);

    if (old)
        gm_buffer_unref(&old->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

static bool
avf_open(struct gm_device *dev, struct gm_device_config *config, char **err)
{
    gm_debug(dev->log, "AVFrameworks Device Open");

    /* We wait until _configure() time before doing much because we want to
     * allow the device to be configured with an event callback first
     * so we will be able to notify that the device is ready if the Tango
     * service has already been bound.
     */

    dev->video_format = GM_FORMAT_BGRA_U8;
    dev->max_video_pixels = 640 * 480;

    dev->depth_format = GM_FORMAT_Z_F32_M;
    dev->max_depth_pixels = 640 * 480;

    dev->avf.session = ios_util_av_session_new(dev->log,
                                               on_avf_configure_finished_cb,
                                               on_avf_depth_cb,
                                               on_avf_video_cb,
                                               dev);
    //ios_util_session_configure(dev->avf.session);

    return true;
}

static void
avf_close(struct gm_device *dev)
{
    gm_debug(dev->log, "AVFrameworks Device Close");
}

static bool
avf_configure(struct gm_device *dev, char **err)
{
    dev->configured = true;

    gm_debug(dev->log, "AVFoundation Device Configure");
    ios_util_session_configure(dev->avf.session);
    //notify_device_ready(dev);

    return true;
}

static void
avf_start(struct gm_device *dev)
{
    dev->running = true;
    gm_debug(dev->log, "avf_start");
    ios_util_session_start(dev->avf.session);
}

static void
avf_stop(struct gm_device *dev)
{
    gm_debug(dev->log, "avf_stop");
    ios_util_session_stop(dev->avf.session);
    dev->running = false;
}
#endif // USE_AVF

struct gm_device *
gm_device_open(struct gm_logger *log,
               struct gm_device_config *config,
               char **err)
{
    struct gm_device *dev = new gm_device();
    bool status = false;

    pthread_mutex_init(&dev->request_buffers_mask_lock, NULL);
    pthread_mutex_init(&dev->swap_buffers_lock, NULL);

    dev->log = log;
    dev->type = config->type;

    dev->video_buf_pool = mem_pool_alloc(
                     log,
                     "video",
                     INT_MAX, // max size
                     device_video_buf_alloc,
                     device_buffer_free,
                     dev); // user data
    dev->depth_buf_pool = mem_pool_alloc(
                     log,
                     "depth",
                     INT_MAX, // max size
                     device_depth_buf_alloc,
                     device_buffer_free,
                     dev); // user data
    dev->frame_pool = mem_pool_alloc(
                     log,
                     "frame",
                     INT_MAX, // max size
                     device_frame_alloc,
                     device_frame_free,
                     dev); // user data

    switch (config->type) {
    case GM_DEVICE_KINECT:
        gm_debug(log, "Opening Kinect device");
#ifdef USE_FREENECT
        status = kinect_open(dev, config, err);
#else
        gm_assert(log, 0, "Kinect support not enabled");
#endif
        break;
    case GM_DEVICE_RECORDING:
        gm_debug(log, "Opening Glimpse Viewer recording playback device");
        status = recording_open(dev, config, err);
        break;
    case GM_DEVICE_TANGO:
        gm_debug(log, "Opening Tango device");
#ifdef USE_TANGO
        status = tango_open(dev, config, err);
#else
        gm_assert(log, 0, "Tango support not enabled");
#endif
        break;
    case GM_DEVICE_AVF:
        gm_debug(log, "Opening AVFoundation device");
#ifdef USE_AVF
        status = avf_open(dev, config, err);
#else
        gm_assert(log, 0, "AVFoundation support not enabled");
#endif
        break;
    }

    if (!status) {
        gm_device_close(dev);
        return NULL;
    }

    struct gm_ui_property prop;

    /* XXX: there should probably be separate rotation state for the depth
     * and video cameras
     */
    struct gm_ui_enumerant enumerant;
    prop = gm_ui_property();
    prop.object = dev;
    prop.name = "rotation";
    prop.desc = "Rotation of camera images relative to current display orientation";
    prop.type = GM_PROPERTY_ENUM;
    prop.enum_state.ptr = &dev->camera_rotation;
    //prop.read_only = true;

    for (int i = 0; i < 4; i++) {
        enumerant = gm_ui_enumerant();
        enumerant.name = rotation_names[i];
        enumerant.desc = rotation_names[i];
        enumerant.val = i;
        dev->rotation_enumerants.push_back(enumerant);
    }
    prop.enum_state.n_enumerants = dev->rotation_enumerants.size();
    prop.enum_state.enumerants = dev->rotation_enumerants.data();
    dev->camera_rotation_prop_id = dev->properties.size();
    dev->properties.push_back(prop);

    dev->properties_state.n_properties = dev->properties.size();
    pthread_mutex_init(&dev->properties_state.lock, NULL);
    dev->properties_state.properties = &dev->properties[0];

    return dev;
}

enum gm_device_type
gm_device_get_type(struct gm_device *dev)
{
    return dev->type;
}

bool
gm_device_commit_config(struct gm_device *dev, char **err)
{
    bool status = true;

    switch (dev->type) {
    case GM_DEVICE_TANGO:
#ifdef USE_TANGO
        status = tango_configure(dev, err);
#endif
        break;
    case GM_DEVICE_AVF:
#ifdef USE_AVF
        status = avf_configure(dev, err);
#endif
        break;
    default:
        dev->configured = true;
        notify_device_ready(dev);
        status = true;
        break;
    }

    return status;
}

static void
print_trail_for(struct gm_logger *log, void *object, std::vector<struct trail_crumb> *trail)
{
    gm_debug(log, "Trail for %p:", object);

    for (unsigned i = 0; i < trail->size(); i++) {
        struct trail_crumb crumb = trail->at(i);
        if (crumb.n_frames) {
            struct gm_backtrace backtrace = {
                crumb.n_frames,
                (const void **)crumb.backtrace_frame_pointers
            };
            int line_len = 100;
            char *formatted = (char *)alloca(crumb.n_frames * line_len);

            gm_debug(log, "%d) tag = %s", i, crumb.tag);
            gm_logger_get_backtrace_strings(log, &backtrace,
                                            line_len, (char *)formatted);
            for (int i = 0; i < crumb.n_frames; i++) {
                char *line = formatted + line_len * i;
                gm_debug(log, "   #%i %s", i, line);
            }
        }
    }
}

static void
print_frame_info_cb(struct gm_mem_pool *pool,
                    void *resource,
                    void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    struct gm_device_frame *frame = (struct gm_device_frame *)resource;

    gm_assert(dev->log, frame != NULL, "Spurious NULL frame resource");
    gm_error(dev->log, "Unreleased frame %p, ref count = %d, paper trail len = %d",
             frame,
             frame->base.ref,
             (int)frame->trail.size());

    if (frame->trail.size())
        print_trail_for(dev->log, frame, &frame->trail);
}

static void
print_buf_info_cb(struct gm_mem_pool *pool,
                  void *resource,
                  void *user_data)
{
    struct gm_device *dev = (struct gm_device *)user_data;
    struct gm_device_buffer *buf = (struct gm_device_buffer *)resource;
    const char *pool_name = mem_pool_get_name(pool);

    gm_assert(dev->log, buf != NULL, "Spurious NULL %s resource", pool_name);

    gm_error(dev->log, "Unreleased %s buffer %p, ref count = %d, paper trail len = %d",
             pool_name,
             buf,
             buf->base.ref,
             (int)buf->trail.size());

    if (buf->trail.size())
        print_trail_for(dev->log, buf, &buf->trail);
}

void
gm_device_close(struct gm_device *dev)
{
    gm_debug(dev->log, "gm_device_close");
    if (dev->running)
        gm_device_stop(dev);

    switch (dev->type) {
    case GM_DEVICE_KINECT:
#ifdef USE_FREENECT
        gm_debug(dev->log, "kinect_close");
        kinect_close(dev);
#endif
        break;
    case GM_DEVICE_RECORDING:
        gm_debug(dev->log, "recording_close");
        recording_close(dev);
        break;
    case GM_DEVICE_TANGO:
#ifdef USE_TANGO
        gm_debug(dev->log, "tango_close");
        tango_close(dev);
#endif
        break;
    case GM_DEVICE_AVF:
#ifdef USE_AVF
        gm_debug(dev->log, "avf_close");
        avf_close(dev);
#endif
        break;

    }

    /* Make sure to release current back/ready buffers to their
     * pools to avoid assertions when destroying the pools...
     */

    if (dev->last_frame) {
        gm_frame_unref(dev->last_frame);
        dev->last_frame = NULL;
    }

    if (dev->depth_buf_back) {
        gm_buffer_unref(&dev->depth_buf_back->base);
        dev->depth_buf_back = NULL;
    }
    if (dev->depth_buf_ready) {
        gm_buffer_unref(&dev->depth_buf_ready->base);
        dev->depth_buf_ready = NULL;
    }

    if (dev->video_buf_back) {
        gm_buffer_unref(&dev->video_buf_back->base);
        dev->video_buf_back = NULL;
    }
    if (dev->video_buf_ready) {
        gm_buffer_unref(&dev->video_buf_ready->base);
        dev->video_buf_ready = NULL;
    }

    /* We free the pools in order of dependence (parents, then children) so
     * that if we hit any assertions for resource leaks then we will know about
     * the most significant object first because it's then implied there would
     * likely be downstream assertions too.
     */
    mem_pool_foreach(dev->frame_pool,
                     print_frame_info_cb,
                     dev);
    mem_pool_free(dev->frame_pool);

    mem_pool_foreach(dev->depth_buf_pool,
                     print_buf_info_cb,
                     dev);
    mem_pool_free(dev->depth_buf_pool);

    mem_pool_foreach(dev->video_buf_pool,
                     print_buf_info_cb,
                     dev);
    mem_pool_free(dev->video_buf_pool);

    delete dev;
}

void
gm_device_set_event_callback(struct gm_device *dev,
                             void (*event_callback)(struct gm_device_event *event,
                                                    void *user_data),
                             void *user_data)
{
    dev->event_callback = event_callback;
    dev->callback_data = user_data;
}

void
gm_device_start(struct gm_device *dev)
{
    if (dev->running) {
        return;
    }

    switch (dev->type) {
    case GM_DEVICE_KINECT:
#ifdef USE_FREENECT
        kinect_start(dev);
#endif
        break;
    case GM_DEVICE_RECORDING:
        recording_start(dev);
        break;
    case GM_DEVICE_TANGO:
#ifdef USE_TANGO
        tango_start(dev);
#endif
        break;
    case GM_DEVICE_AVF:
#ifdef USE_AVF
        avf_start(dev);
#endif
        break;
    }
}

void
gm_device_stop(struct gm_device *dev)
{
    gm_debug(dev->log, "gm_device_stop");

    if (!dev->running) {
        return;
    }

    switch (dev->type) {
    case GM_DEVICE_KINECT:
#ifdef USE_FREENECT
        gm_debug(dev->log, "kinect_stop");
        kinect_stop(dev);
#endif
        break;
    case GM_DEVICE_RECORDING:
        gm_debug(dev->log, "recording_stop");
        recording_stop(dev);
        break;
    case GM_DEVICE_TANGO:
#ifdef USE_TANGO
        gm_debug(dev->log, "tango_stop");
        tango_stop(dev);
#endif
        break;
    case GM_DEVICE_AVF:
#ifdef USE_AVF
        gm_debug(dev->log, "avf_stop");
        avf_stop(dev);
#endif
        break;
    }
}

int
gm_device_get_max_depth_pixels(struct gm_device *dev)
{
    return dev->max_depth_pixels;
}

int
gm_device_get_max_video_pixels(struct gm_device *dev)
{
    return dev->max_video_pixels;
}

enum gm_rotation
gm_device_get_camera_rotation(struct gm_device *dev)
{
    switch (dev->type) {
#ifdef USE_TANGO
    case GM_DEVICE_TANGO:
        return dev->tango.display_to_camera_rotation;
#endif
    default:
        return GM_ROTATION_0;
    }
}

struct gm_extrinsics *
gm_device_get_depth_to_video_extrinsics(struct gm_device *dev)
{
    return &dev->depth_to_video_extrinsics;
}

void
gm_device_request_frame(struct gm_device *dev, uint64_t buffers_mask)
{
    if (!buffers_mask) {
        return;
    }

    pthread_mutex_lock(&dev->request_buffers_mask_lock);
    dev->frame_request_buffers_mask |= buffers_mask;
    maybe_notify_frame_locked(dev);
    pthread_mutex_unlock(&dev->request_buffers_mask_lock);
}

#if 0
static void
kinect_update_frame(struct gm_device *dev, struct gm_frame *frame)
{
    frame->depth_format = GM_FORMAT_Z_U16_MM;
}

static void
recording_update_frame(struct gm_device *dev, struct gm_frame *frame)
{
    frame->depth_format = GM_FORMAT_Z_F16_M;
}
#endif

struct gm_frame *
gm_device_get_latest_frame(struct gm_device *dev)
{
    struct gm_device_frame *frame = mem_pool_acquire_frame(dev->frame_pool,
                                                           "get latest");

#if 0
    switch (dev->type) {
    case GM_DEVICE_KINECT:
        kinect_update_frame(dev, frame);
        break;
    case GM_DEVICE_RECORDING:
        recording_update_frame(dev, frame);
        break;
    }
#endif

    pthread_mutex_lock(&dev->swap_buffers_lock);

    if (dev->last_frame)
        gm_frame_unref(dev->last_frame);

    dev->last_frame = &frame->base;

    gm_debug(dev->log, "latest frame = %p, buffers_mask = %" PRIx64,
             frame, dev->frame_ready_buffers_mask);

    if (dev->frame_ready_buffers_mask & GM_REQUEST_FRAME_DEPTH) {
        frame->base.depth = &dev->depth_buf_ready->base;
        dev->depth_buf_ready = NULL;
        frame->base.depth_format = dev->depth_format;
        gm_assert(dev->log, frame->base.depth != NULL,
                  "Depth ready flag set but buffer missing");
        gm_debug(dev->log, "> depth = %p, intrinsics w=%d, h=%d",
                 frame->base.depth,
                 dev->depth_intrinsics.width,
                 dev->depth_intrinsics.height);
        frame->base.depth_intrinsics = dev->depth_intrinsics;
        gm_assert(dev->log,
                  dev->depth_intrinsics.width > 0 &&
                  dev->depth_intrinsics.height > 0,
                  "Invalid intrinsics for latest depth buffer");
    }
    if (dev->frame_ready_buffers_mask & GM_REQUEST_FRAME_VIDEO) {
        frame->base.video = &dev->video_buf_ready->base;
        dev->video_buf_ready = NULL;
        frame->base.video_format = dev->video_format;
        gm_assert(dev->log, frame->base.video != NULL,
                  "Video ready flag set but buffer missing");
        gm_debug(dev->log, "> video = %p, intrinsics w=%d, h=%d",
                 frame->base.video,
                 dev->video_intrinsics.width,
                 dev->video_intrinsics.height);
        gm_assert(dev->log,
                  dev->video_intrinsics.width > 0 &&
                  dev->video_intrinsics.height > 0,
                  "Invalid intrinsics for latest video buffer");
        frame->base.video_intrinsics = dev->video_intrinsics;
    }

    frame->base.timestamp = dev->frame_time;
    frame->base.pose = dev->frame_pose;
    frame->base.camera_rotation = (enum gm_rotation)dev->camera_rotation;

    dev->frame_ready_buffers_mask = 0;

    /* Get a ref() for the caller */
    gm_frame_ref(&frame->base);

    pthread_mutex_unlock(&dev->swap_buffers_lock);

    /* We should have one ref for dev->last_frame and return a _ref() to the
     * caller so there's no race between the caller claiming a reference and us
     * possibly dropping our own ref.
     */
    gm_assert(dev->log, frame->base.ref == 2,
              "Spurious ref counting for new frame");
    return &frame->base;
}

/* XXX: not clear how we should handle incompatible frames, e.g. due to
 * mismatching rotations?
 */
struct gm_frame *
gm_device_combine_frames(struct gm_device *dev, struct gm_frame *master,
                         struct gm_frame *depth, struct gm_frame *video)
{
    struct gm_device_frame *frame = mem_pool_acquire_frame(dev->frame_pool, "combined frame");
    gm_assert(dev->log, depth->depth != NULL,
              "Spurious request to combine frame with depth frame having no depth buffer");
    gm_assert(dev->log, video->video != NULL,
              "Spurious request to combine frame with video frame having no video buffer");

    frame->base.timestamp = master->timestamp;
    frame->base.pose = master->pose;
    frame->base.camera_rotation = master->camera_rotation;

    frame->base.depth = gm_buffer_ref(depth->depth);
    frame->base.depth_format = depth->depth_format;
    frame->base.depth_intrinsics = depth->depth_intrinsics;

    frame->base.video = gm_buffer_ref(video->video);
    frame->base.video_format = video->video_format;
    frame->base.video_intrinsics = video->video_intrinsics;

    return &frame->base;
}

struct gm_ui_properties *
gm_device_get_ui_properties(struct gm_device *dev)
{
    return &dev->properties_state;
}

#ifdef __ANDROID__
void
gm_device_attach_jvm(struct gm_device *dev, JavaVM *jvm)
{
    dev->jvm = jvm;
}

static void
handle_jni_OnDisplayRotate(jint rotation)
{
#ifdef USE_TANGO
    // we might race with gm_device_configure which also wants to check the
    // latest display rotation
    pthread_mutex_lock(&jni_lock);

    tango_display_rotation = (enum gm_rotation)rotation;

    if (tango_singleton_dev) {
        tango_set_display_rotation(tango_singleton_dev, (enum gm_rotation)rotation);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "Glimpse Device", "Early onDisplayRotate JNI");
    }

    pthread_mutex_unlock(&jni_lock);
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_impossible_glimpse_GlimpseNativeActivity_OnDisplayRotate(
    JNIEnv* /*env*/, jobject /*obj*/, jint rotation)
{
    handle_jni_OnDisplayRotate(rotation);
}

extern "C" JNIEXPORT void JNICALL
Java_com_impossible_glimpse_GlimpseUnityActivity_OnDisplayRotate(
    JNIEnv* /*env*/, jobject /*obj*/, jint rotation)
{
    handle_jni_OnDisplayRotate(rotation);
}

#ifdef USE_TANGO
extern "C" JNIEXPORT void JNICALL
Java_com_impossible_glimpse_GlimpseJNI_onTangoServiceConnected(JNIEnv *env,
                                                               jobject /*obj*/,
                                                               jobject binder)
{
    // we might race with gm_device_configure which also wants to know whether
    // the service is already connected...
    pthread_mutex_lock(&jni_lock);

    if (tango_singleton_dev) {
        tango_set_service_binder(tango_singleton_dev, env, binder);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "Glimpse Device", "Early onTangoServiceConnected JNI");
        early_tango_service_binder = env->NewWeakGlobalRef(binder);
    }

    pthread_mutex_unlock(&jni_lock);
}
#endif

#endif // __ANDROID__
