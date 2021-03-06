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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <assert.h>
#include <time.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>

#include <math.h>

#include <pthread.h>

#include <list>
#include <vector>

#include <epoxy/gl.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>
#include <imgui_internal.h> // For PushItemFlags(ImGuiItemFlags_Disabled)

#ifdef __ANDROID__
#    include <android/log.h>
#    include <jni.h>
#endif

#ifdef __IOS__
#include "ios_utils.h"
#endif

#ifdef USE_GLFM
#    define GLFM_INCLUDE_NONE
#    include <glfm.h>
#    include <imgui_impl_glfm_gles3.h>
#else
#    define GLFW_INCLUDE_NONE
#    include <GLFW/glfw3.h>
#    include <imgui_impl_glfw_gles3.h>
#    include <getopt.h>
#endif

#if defined(__APPLE__) && !defined(__IOS__)
#define GLSL_SHADER_VERSION "#version 400\n"
#else
#define GLSL_SHADER_VERSION "#version 300 es\n"
#endif

#ifdef USE_TANGO
#include <tango_client_api.h>
#include <tango_support_api.h>
#endif

#include <profiler.h>

#include "parson.h"

#include "glimpse_log.h"
#include "glimpse_context.h"
#include "glimpse_device.h"
#include "glimpse_record.h"
#include "glimpse_assets.h"
#include "glimpse_gl.h"

#undef GM_LOG_CONTEXT
#ifdef __ANDROID__
#define GM_LOG_CONTEXT "Glimpse Viewer"
#else
#define GM_LOG_CONTEXT "viewer"
#endif

#define ARRAY_LEN(X) (sizeof(X)/sizeof(X[0]))
#define LOOP_INDEX(x,y) ((x)[(y) % ARRAY_LEN(x)])

#define TOOLBAR_WIDTH 300
#define MAX_VIEWS 5

#define xsnprintf(dest, n, fmt, ...) do { \
        if (snprintf(dest, n, fmt,  __VA_ARGS__) >= (int)(n)) \
            exit(1); \
    } while(0)

enum event_type
{
    EVENT_DEVICE,
    EVENT_CONTEXT
};

struct event
{
    enum event_type type;
    union {
        struct gm_event *context_event;
        struct gm_device_event *device_event;
        int android_event;
    };
};

typedef struct {
    float x;
    float y;
    float z;
    uint32_t rgba;
} XYZRGBA;

typedef struct _Data
{
    struct gm_logger *log;
    FILE *log_fp;

    /* On Android we don't actually initialize a lot of state including
     * ImGui until we've negotiated permissions, since we might not be
     * able to load the font we need. viewer_init() will be called if
     * the check passes.
     */
    bool initialized;
    bool gl_initialized;

    /* Some GL state is re-initialized each time we switch devices */
    bool device_gl_initialized;

    struct gm_context *ctx;

#ifdef USE_GLFW
    GLFWwindow *window;
#else
    bool surface_created;
#endif
    int win_width;
    int win_height;

    /* Normally this is 'false' and we show lots of intermediate debug buffers
     * but e.g. if running on Android with Tango then we try to more closely
     * represent a 'real' augmented reality app with fullscreen video plus a
     * skeleton overlay so we can better judge the base-line performance we
     * can expect to achieve for these kinds of applications.
     * (uploading all of the debug textures can significantly impact the
     * runtime performance, e.g. taking > 100ms each time we get a tracking
     * update)
     */
    bool realtime_ar_mode;

    bool show_profiler;

    /* In realtime mode, we use predicted joint positions so that the
     * presented skeleton keeps up with the video. This allows us to add a
     * synthetic delay to the timestamp we request in this mode, which adds
     * some lag, but improves the quality of the positions as it doesn't need
     * to extrapolate so far into the future.
     */
    int prediction_delay;

    /* The size of the depth buffer visualisation texture */
    int depth_rgb_width;
    int depth_rgb_height;

    /* The size of the video buffer visualisation texture */
    int video_rgb_width;
    int video_rgb_height;

    /* The size of the depth classification buffer visualisation texture */
    int classify_rgb_width;
    int classify_rgb_height;

    /* The size of the candidate clusters visualisation texture */
    int cclusters_rgb_width;
    int cclusters_rgb_height;

    /* The size of the labels visualisation texture */
    int labels_rgb_width;
    int labels_rgb_height;

    /* A convenience for accessing number of joints in latest tracking */
    int n_joints;
    int n_bones;

    glm::vec3 focal_point;
    float camera_rot_yx[2];
    JSON_Value *joint_map;

    /* When we request gm_device for a frame we set a buffers_mask for what the
     * frame should include. We track the buffers_mask so we avoid sending
     * subsequent frame requests that would downgrade the buffers_mask
     */
    uint64_t pending_frame_buffers_mask;

    /* Set when gm_device sends a _FRAME_READY device event */
    bool device_frame_ready;

    /* Once we've been notified that there's a device frame ready for us then
     * we store the latest frames from gm_device_get_latest_frame() here...
     */
    struct gm_frame *last_depth_frame;
    struct gm_frame *last_video_frame;

    /* Set when gm_context sends a _REQUEST_FRAME event */
    bool context_needs_frame;
    /* Set when gm_context sends a _TRACKING_READY event */
    bool tracking_ready;

    /* Once we've been notified that there's a skeleton tracking update for us
     * then we store the latest tracking data from
     * gm_context_get_latest_tracking() here...
     */
    struct gm_tracking *latest_tracking;

    /* Recording is handled by the gm_recording structure, which saves out
     * frames as we add them.
     */
    bool overwrite_recording;
    struct gm_recording *recording;
    struct gm_device *recording_device;
    std::vector<char *> recordings;
    std::vector<char *> recording_names;
    int selected_playback_recording;

    struct gm_device *playback_device;

    struct gm_device *active_device;

    /* Events from the gm_context and gm_device apis may be delivered via any
     * arbitrary thread which we don't want to block, and at a time where
     * the gm_ apis may not be reentrant due to locks held during event
     * notification
     */
    pthread_mutex_t event_queue_lock;
    std::vector<struct event> *events_back;
    std::vector<struct event> *events_front;

    JSON_Value *joints_recording_val;
    JSON_Array *joints_recording;
    int requested_recording_len;

    GLuint video_program;
    GLuint video_quad_attrib_bo;

    /* Even though glEnable/DisableVertexAttribArray take unsigned integers,
     * these are signed because GL's glGetAttribLocation api returns attribute
     * locations as signed values where -1 means the attribute isn't
     * active. ...!?
     */
    GLint video_quad_attrib_pos;
    GLint video_quad_attrib_tex_coords;


    GLuint cloud_fbo;
    GLuint cloud_depth_renderbuf;
    GLuint cloud_fbo_tex;
    bool cloud_fbo_valid;

    GLuint cloud_program;
    GLuint cloud_uniform_mvp;
    GLuint cloud_uniform_pt_size;

    GLuint cloud_bo;
    GLint cloud_attr_pos;
    GLint cloud_attr_col;
    int n_cloud_points;

    GLuint lines_bo;
    GLint lines_attr_pos;
    GLint lines_attr_col;
    int n_lines;

    GLuint skel_joints_bo;
    GLuint skel_bones_bo;

    GLuint video_rgb_tex;

    GLuint ar_video_tex_sampler;
    std::vector<GLuint> ar_video_queue;
    int ar_video_queue_len;
    int ar_video_queue_pos;

} Data;

#ifdef __ANDROID__
static JavaVM *android_jvm_singleton;
#endif

static uint32_t joint_palette[] = {
    0xFFFFFFFF, // head.tail
    0xCCCCCCFF, // neck_01.head
    0xFF8888FF, // upperarm_l.head
    0x8888FFFF, // upperarm_r.head
    0xFFFF88FF, // lowerarm_l.head
    0xFFFF00FF, // lowerarm_l.tail
    0x88FFFFFF, // lowerarm_r.head
    0x00FFFFFF, // lowerarm_r.tail
    0x33FF33FF, // thigh_l.head
    0x33AA33FF, // thigh_l.tail
    0xFFFF33FF, // thigh_r.head
    0xAAAA33FF, // thigh_r.tail
    0x3333FFFF, // foot_l.head
    0xFF3333FF, // foot_r.head
};

char *glimpse_recordings_path;

static GLuint gl_labels_tex;
static GLuint gl_depth_rgb_tex;
static GLuint gl_classify_rgb_tex;
static GLuint gl_cclusters_rgb_tex;

static const char *views[] = {
    "Controls", "Video Buffer", "Depth Buffer",
    "Depth classification", "Candidate clusters", "Labels", "Cloud" };

static bool pause_profile;

#ifdef USE_GLFM
static bool permissions_check_failed;
static bool permissions_check_passed;
#endif

static enum gm_device_type device_type_opt = GM_DEVICE_KINECT;
static char *device_recording_opt;

static void viewer_init(Data *data);

static void init_basic_opengl(Data *data);
static void init_viewer_opengl(Data *data);
static void init_device_opengl(Data *data);
static void deinit_device_opengl(Data *data);

static void handle_device_ready(Data *data, struct gm_device *dev);
static void on_device_event_cb(struct gm_device_event *device_event,
                               void *user_data);

static void
unref_device_frames(Data *data)
{
    if (data->last_video_frame) {
        gm_frame_unref(data->last_video_frame);
        data->last_video_frame = NULL;
    }
    if (data->last_depth_frame) {
        gm_frame_unref(data->last_depth_frame);
        data->last_depth_frame = NULL;
    }
}

static void
on_profiler_pause_cb(bool pause)
{
    pause_profile = pause;
}

glm::mat4
intrinsics_to_project_matrix(const struct gm_intrinsics *intrinsics,
                             float near, float far)
{
  float width = intrinsics->width;
  float height = intrinsics->height;

  float scalex = near / intrinsics->fx;
  float scaley = near / intrinsics->fy;

  float offsetx = (intrinsics->cx - width / 2.0) * scalex;
  float offsety = (intrinsics->cy - height / 2.0) * scaley;

  return glm::frustum(scalex * -width / 2.0f - offsetx,
                      scalex * width / 2.0f - offsetx,
                      scaley * height / 2.0f - offsety,
                      scaley * -height / 2.0f - offsety, near, far);
}

static bool
index_recordings_recursive(Data *data,
                           const char *recordings_path, const char *rel_path,
                           std::vector<char *> &files,
                           std::vector<char *> &names, char **err)
{
    struct dirent *entry;
    struct stat st;
    DIR *dir;
    bool ret = true;

    char full_path[512];
    xsnprintf(full_path, sizeof(full_path), "%s/%s", recordings_path, rel_path);
    if (!(dir = opendir(full_path))) {
        gm_throw(data->log, err, "Failed to open directory %s\n", full_path);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        char cur_full_path[512];
        char next_rel_path[512];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        xsnprintf(cur_full_path, sizeof(cur_full_path), "%s/%s/%s",
                  recordings_path, rel_path, entry->d_name);
        xsnprintf(next_rel_path, sizeof(next_rel_path), "%s/%s",
                  rel_path, entry->d_name);

        stat(cur_full_path, &st);
        if (S_ISDIR(st.st_mode)) {
            if (!index_recordings_recursive(data, recordings_path,
                                            next_rel_path, files, names, err))
            {
                ret = false;
                break;
            }
        } else if (strlen(rel_path) &&
                   strcmp(entry->d_name, "glimpse_recording.json") == 0) {
            files.push_back(strdup(rel_path));
            char *record_dir = basename(dirname(cur_full_path));
            names.push_back(strdup(record_dir));
        }
    }

    closedir(dir);

    return ret;
}

static void
index_recordings(Data *data)
{
    data->recordings.clear();
    data->recording_names.clear();

    char *index_err = NULL;
    index_recordings_recursive(data,
                               glimpse_recordings_path,
                               "", // relative path
                               data->recordings,
                               data->recording_names,
                               &index_err);
    if (index_err) {
        gm_error(data->log, "Failed to index recordings: %s", index_err);
        free(index_err);
    }
}

static void
draw_properties(struct gm_ui_properties *props)
{
    for (int i = 0; i < props->n_properties; i++) {
        struct gm_ui_property *prop = &props->properties[i];

        if (prop->read_only) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        }

        switch (prop->type) {
        case GM_PROPERTY_INT:
            {
                int current_val = gm_prop_get_int(prop), save_val = current_val;
                ImGui::SliderInt(prop->name, &current_val,
                                 prop->int_state.min, prop->int_state.max);
                if (current_val != save_val)
                    gm_prop_set_int(prop, current_val);
            }
            break;
        case GM_PROPERTY_ENUM:
            {
                int current_enumerant = 0, save_enumerant = 0;
                int current_val = gm_prop_get_enum(prop);

                for (int j = 0; j < prop->enum_state.n_enumerants; j++) {
                    if (prop->enum_state.enumerants[j].val == current_val) {
                        current_enumerant = save_enumerant = j;
                        break;
                    }
                }

                std::vector<const char*> labels(prop->enum_state.n_enumerants);
                for (int j = 0; j < prop->enum_state.n_enumerants; j++) {
                    labels[j] = prop->enum_state.enumerants[j].name;
                }

                ImGui::Combo(prop->name, &current_enumerant, labels.data(),
                             labels.size());

                if (current_enumerant != save_enumerant) {
                    int e = current_enumerant;
                    gm_prop_set_enum(prop, prop->enum_state.enumerants[e].val);
                }
            }
            break;
        case GM_PROPERTY_BOOL:
            {
                bool current_val = gm_prop_get_bool(prop),
                     save_val = current_val;
                ImGui::Checkbox(prop->name, &current_val);
                if (current_val != save_val)
                    gm_prop_set_bool(prop, current_val);
            }
            break;
        case GM_PROPERTY_SWITCH:
            {
                if (i && props->properties[i-1].type == GM_PROPERTY_SWITCH) {
                    ImGui::SameLine();
                }
                if (ImGui::Button(prop->name)) {
                    gm_prop_set_switch(prop);
                }
            }
            break;
        case GM_PROPERTY_FLOAT:
            {
                float current_val = gm_prop_get_float(prop), save_val = current_val;
                ImGui::SliderFloat(prop->name, &current_val,
                                   prop->float_state.min, prop->float_state.max);
                if (current_val != save_val)
                    gm_prop_set_float(prop, current_val);
            }
            break;
        case GM_PROPERTY_FLOAT_VEC3:
            if (prop->read_only) {
                ImGui::LabelText(prop->name, "%.3f,%.3f,%.3f",
                                 //prop->vec3_state.components[0],
                                 prop->vec3_state.ptr[0],
                                 //prop->vec3_state.components[1],
                                 prop->vec3_state.ptr[1],
                                 //prop->vec3_state.components[2],
                                 prop->vec3_state.ptr[2]);
            } // else TODO
            break;
        // FIXME: Handle GM_PROPERTY_STRING
        }

        if (prop->read_only) {
            ImGui::PopStyleVar();
            ImGui::PopItemFlag();
        }
    }
}

static void
adjust_aspect(ImVec2 &input, int width, int height)
{
    ImVec2 output = input;
    float aspect = width / (float)height;
    if (aspect > input.x / input.y) {
        output.y = input.x / aspect;
    } else {
        output.x = input.y * aspect;
    }
    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPosX(cur.x + (input.x - output.x) / 2.f);
    ImGui::SetCursorPosY(cur.y + (input.y - output.y) / 2.f);
    input = output;
}

static struct gm_ui_property *
find_prop(struct gm_ui_properties *props, const char *name)
{
    for (int p = 0; p < props->n_properties; ++p) {
        struct gm_ui_property *prop = &props->properties[p];

        if (prop->read_only)
            continue;

        if (strcmp(name, prop->name) == 0)
            return prop;
    }

    return NULL;
}

static GLuint
gen_ar_video_texture(Data *data)
{
    GLuint ar_video_tex;

    glGenTextures(1, &ar_video_tex);

    GLenum target = GL_TEXTURE_2D;
    if (gm_device_get_type(data->active_device) == GM_DEVICE_TANGO) {
        target = GL_TEXTURE_EXTERNAL_OES;
    }

    glBindTexture(target, ar_video_tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return ar_video_tex;
}

static void
update_ar_video_queue_len(Data *data, int len)
{
    if (len >= data->ar_video_queue_len) {
        data->ar_video_queue_len = len;
        return;
    }
    glDeleteTextures(data->ar_video_queue.size(),
                     data->ar_video_queue.data());
    data->ar_video_queue.resize(0);
    data->ar_video_queue_len = len;
    data->ar_video_queue_pos = -1;
}

static GLuint
get_next_ar_video_tex(Data *data)
{
    if (data->ar_video_queue_len < 1) {
        update_ar_video_queue_len(data, 1);
    }

    if (data->ar_video_queue.size() < data->ar_video_queue_len) {
        GLuint ar_video_tex = gen_ar_video_texture(data);

        data->ar_video_queue_pos = data->ar_video_queue.size();
        data->ar_video_queue.push_back(ar_video_tex);
        return data->ar_video_queue.back();
    } else {
        data->ar_video_queue_pos =
            (data->ar_video_queue_pos + 1) % data->ar_video_queue_len;
        return data->ar_video_queue[data->ar_video_queue_pos];
    }
}

static GLuint
get_oldest_ar_video_tex(Data *data)
{
    if (data->ar_video_queue.size() < data->ar_video_queue_len) {
        return data->ar_video_queue[0];
    } else {
        int oldest = (data->ar_video_queue_pos + 1) % data->ar_video_queue_len;
        return data->ar_video_queue[oldest];
    }
}


static bool
draw_controls(Data *data, int x, int y, int width, int height, bool disabled)
{
    struct gm_ui_properties *dev_props =
        gm_device_get_ui_properties(data->active_device);
    struct gm_ui_properties *ctx_props =
        gm_context_get_ui_properties(data->ctx);

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("Controls", NULL,
                 ImGuiWindowFlags_NoTitleBar|
                 ImGuiWindowFlags_NoResize|
                 ImGuiWindowFlags_NoMove|
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (disabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    }

    bool focused = ImGui::IsWindowFocused();

    ImGui::TextDisabled("Viewer properties...");
    ImGui::Separator();
    ImGui::Spacing();

    bool current_ar_mode = data->realtime_ar_mode;
    ImGui::Checkbox("Real-time AR Mode", &data->realtime_ar_mode);
    if (data->realtime_ar_mode != current_ar_mode)
    {
        if (data->realtime_ar_mode) {
            // Make sure to disable the debug cloud in real-time AR mode since it
            // may be costly to create.
            //
            // Note: We don't have to explicitly disable most debug views because
            // we only do work when we pull the data from the context, but that's
            // not the case for the cloud debug view.
            gm_prop_set_enum(find_prop(ctx_props, "cloud_mode"), 0);
        } else {
            gm_prop_set_enum(find_prop(ctx_props, "cloud_mode"), 1);
        }
    }

    ImGui::Checkbox("Show profiler", &data->show_profiler);

    int queue_len = data->ar_video_queue_len;
    ImGui::SliderInt("AR video queue len", &queue_len, 1, 30);
    if (data->ar_video_queue_len != queue_len) {
        update_ar_video_queue_len(data, queue_len);
    }

    ImGui::Checkbox("Overwrite recording", &data->overwrite_recording);
    ImGui::SliderInt("Prediction delay", &data->prediction_delay,
                     0, 1000000000);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Device properties...");
    ImGui::Separator();
    ImGui::Spacing();

    draw_properties(dev_props);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Mo-Cap properties...");
    ImGui::Separator();
    ImGui::Spacing();

    draw_properties(ctx_props);

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save config")) {
        JSON_Value *props_object = json_value_init_object();
        gm_props_to_json(data->log, ctx_props, props_object);
        char *json = json_serialize_to_string_pretty(props_object);
        json_value_free(props_object);
        props_object = NULL;

        const char *assets_root = gm_get_assets_root();
        char filename[512];

        if (snprintf(filename, sizeof(filename), "%s/%s",
                     assets_root, "glimpse-config.json") <
            (int)sizeof(filename))
        {
            FILE *output = fopen(filename, "w");
            if (output) {
                if (fputs(json, output) == EOF) {
                    gm_error(data->log, "Error writing config: %s",
                             strerror(errno));
                } else {
                    gm_debug(data->log, "Wrote %s", filename);
                }
                if (fclose(output) == EOF) {
                    gm_error(data->log, "Error closing config: %s",
                             strerror(errno));
                }
            } else {
                gm_error(data->log, "Error saving config: %s", strerror(errno));
            }
        }

        free(json);
    }

    if (disabled) {
        ImGui::PopItemFlag();
    }

    ImGui::End();

    return focused;
}

static void
viewer_close_playback_device(Data *data)
{
    gm_device_stop(data->playback_device);

    unref_device_frames(data);

    if (data->latest_tracking) {
        gm_tracking_unref(data->latest_tracking);
        data->latest_tracking = nullptr;
    }

    // Flush old device-dependent data from the context
    gm_context_flush(data->ctx, NULL);
    data->tracking_ready = false;

    gm_device_close(data->playback_device);
    data->playback_device = nullptr;

    data->active_device = data->recording_device;
    deinit_device_opengl(data);
}

static void
draw_playback_controls(Data *data, const ImVec4 &bounds)
{
    ImGui::Begin("Playback controls", NULL,
                 //ImGuiWindowFlags_NoTitleBar|
                 ImGuiWindowFlags_NoResize|
                 ImGuiWindowFlags_ShowBorders|
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Spacing();

    if (ImGui::Button(data->recording ? "Stop" : "Record")) {
        if (data->recording) {
            gm_recording_close(data->recording);
            data->recording = NULL;
            index_recordings(data);
        } else if (!data->playback_device) {
            const char *rel_path = NULL;
            bool overwrite = false;
            if (data->overwrite_recording && data->recordings.size()) {
                rel_path = data->recordings.at(data->selected_playback_recording);
                overwrite = true;
            }

            data->recording = gm_recording_init(data->log,
                                                data->recording_device,
                                                glimpse_recordings_path,
                                                rel_path,
                                                overwrite);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(data->playback_device ? "Unload" : "Load") &&
        !data->recording)
    {
        if (data->playback_device) {
            viewer_close_playback_device(data);

            // Wake up the recording device again
            handle_device_ready(data, data->recording_device);
        } else if (data->recordings.size()) {
            gm_device_stop(data->recording_device);

            unref_device_frames(data);

            if (data->latest_tracking) {
                gm_tracking_unref(data->latest_tracking);
                data->latest_tracking = nullptr;
            }

            gm_context_flush(data->ctx, NULL);
            data->tracking_ready = false;

            struct gm_device_config config = {};
            config.type = GM_DEVICE_RECORDING;

            const char *rel_path = data->recordings.at(data->selected_playback_recording);
            char full_path[1024];
            xsnprintf(full_path, sizeof(full_path), "%s/%s",
                      glimpse_recordings_path, rel_path);
            config.recording.path = full_path;

            char *open_err = NULL;
            data->playback_device = gm_device_open(data->log, &config, &open_err);

            if (data->playback_device) {
                gm_device_set_event_callback(data->playback_device,
                                             on_device_event_cb, data);
                data->active_device = data->playback_device;
                deinit_device_opengl(data);

                gm_device_commit_config(data->playback_device, NULL);
            } else {
                gm_error(data->log, "Failed to start recording playback: %s",
                         open_err);
                free(open_err);
                // Wake up the recording device again
                handle_device_ready(data, data->recording_device);
            }
        }
    }

    ImGui::Spacing();

    if (data->recording_names.size()) {
        ImGui::Combo("Recording Path",
                     &data->selected_playback_recording,
                     data->recording_names.data(),
                     data->recording_names.size());
    }

    ImGui::SetWindowSize(ImVec2(0, 0), ImGuiCond_Always);

    ImVec2 size = ImGui::GetWindowSize();
    ImGui::SetWindowPos(ImVec2(bounds.x + (bounds.z - size.x) / 2, 16.f),
                        ImGuiCond_FirstUseEver);

    // Make sure the window stays within bounds
    ImVec2 pos = ImGui::GetWindowPos();

    if (pos.x + size.x > bounds.x + bounds.z) {
        pos.x = (bounds.x + bounds.z) - size.x;
    } else if (pos.x < bounds.x) {
        pos.x = bounds.x;
    }
    if (pos.y + size.y > bounds.y + bounds.w) {
        pos.y = (bounds.y + bounds.w) - size.y;
    } else if (pos.y < bounds.y) {
        pos.y = bounds.y;
    }

    ImGui::SetWindowPos(pos, ImGuiCond_Always);

    ImGui::End();
}

static bool
draw_visualisation(Data *data, int x, int y, int width, int height,
                   int aspect_width, int aspect_height,
                   const char *name, GLuint tex,
                   enum gm_rotation rotation)
{
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin(name, NULL,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    bool focused = ImGui::IsWindowFocused();
    if (tex == 0) {
        return focused;
    }

    ImVec2 uv0, uv1, uv2, uv3;

    switch (rotation) {
    case GM_ROTATION_0:
        uv0 = ImVec2(0, 0);
        uv1 = ImVec2(1, 0);
        uv2 = ImVec2(1, 1);
        uv3 = ImVec2(0, 1);
        break;
    case GM_ROTATION_90:
        uv0 = ImVec2(1, 0);
        uv1 = ImVec2(1, 1);
        uv2 = ImVec2(0, 1);
        uv3 = ImVec2(0, 0);
        std::swap(aspect_width, aspect_height);
        break;
    case GM_ROTATION_180:
        uv0 = ImVec2(1, 1);
        uv1 = ImVec2(0, 1);
        uv2 = ImVec2(0, 0);
        uv3 = ImVec2(1, 0);
        break;
    case GM_ROTATION_270:
        uv0 = ImVec2(0, 1);
        uv1 = ImVec2(0, 0);
        uv2 = ImVec2(1, 0);
        uv3 = ImVec2(1, 1);
        std::swap(aspect_width, aspect_height);
        break;
    }

    ImVec2 area_size = ImGui::GetContentRegionAvail();
    adjust_aspect(area_size, aspect_width, aspect_height);

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImVec2 cur = ImGui::GetCursorScreenPos();
    draw_list->PushTextureID((void *)(intptr_t)tex);

    draw_list->PrimReserve(6, 4);
    draw_list->PrimQuadUV(ImVec2(cur.x, cur.y),
                          ImVec2(cur.x+area_size.x, cur.y),
                          ImVec2(cur.x+area_size.x, cur.y+area_size.y),
                          ImVec2(cur.x, cur.y+area_size.y),
                          uv0,
                          uv1,
                          uv2,
                          uv3,
                          ImGui::GetColorU32(ImVec4(1,1,1,1)));
    draw_list->PopTextureID();
    ImGui::End();

    return focused;
}

static bool
update_skeleton_wireframe_gl_bos(Data *data,
                                 uint64_t timestamp,
                                 int *n_joints_ret,
                                 int *n_bones_ret)
{
    int n_bones = data->n_bones;
    int n_joints;

    *n_joints_ret = 0;
    *n_bones_ret = 0;

    if (!data->latest_tracking) {
        return false;
    }

    /*
     * Update labelled point cloud
     */
    struct gm_prediction *prediction =
        gm_context_get_prediction(data->ctx, timestamp);
    if (!prediction) {
        return false;
    }
    const struct gm_skeleton *skeleton = gm_prediction_get_skeleton(prediction);

    // TODO: Take confidence into account to decide whether or not to show
    //       a particular joint position.
    n_joints = gm_skeleton_get_n_joints(skeleton);

    assert(n_joints == data->n_joints);

    // Reformat and copy over joint data
    XYZRGBA colored_joints[n_joints];
    for (int i = 0; i < n_joints; i++) {
        const struct gm_joint *joint = gm_skeleton_get_joint(skeleton, i);
        colored_joints[i].x = joint->x;
        colored_joints[i].y = joint->y;
        colored_joints[i].z = joint->z;
        colored_joints[i].rgba = LOOP_INDEX(joint_palette, i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, data->skel_joints_bo);
    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(XYZRGBA) * n_joints,
                 colored_joints, GL_DYNAMIC_DRAW);

    // Reformat and copy over bone data
    // TODO: Don't parse this JSON structure here
    XYZRGBA colored_bones[data->n_bones * 2];
    for (int i = 0, b = 0; i < data->n_joints; i++) {
        JSON_Object *joint =
            json_array_get_object(json_array(data->joint_map), i);
        JSON_Array *connections =
            json_object_get_array(joint, "connections");
        for (size_t c = 0; c < json_array_get_count(connections); c++) {
            const char *joint_name = json_array_get_string(connections, c);
            for (int j = 0; j < n_joints; j++) {
                JSON_Object *joint2 = json_array_get_object(
                    json_array(data->joint_map), j);
                if (strcmp(joint_name,
                           json_object_get_string(joint2, "joint")) == 0) {
                    colored_bones[b++] = colored_joints[i];
                    colored_bones[b++] = colored_joints[j];
                    break;
                }
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, data->skel_bones_bo);
    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(XYZRGBA) * n_bones * 2,
                 colored_bones, GL_DYNAMIC_DRAW);

    // Clean-up
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    gm_prediction_unref(prediction);

    *n_joints_ret = n_joints;
    *n_bones_ret = n_bones;

    return true;
}

static void
draw_skeleton_wireframe(Data *data, glm::mat4 mvp,
                        float pt_size,
                        int n_joints,
                        int n_bones)
{
    glUseProgram(data->cloud_program);

    // Set projection transform
    glUniformMatrix4fv(data->cloud_uniform_mvp, 1, GL_FALSE, glm::value_ptr(mvp));

    // Enable vertex arrays for drawing joints/bones
    glEnableVertexAttribArray(data->cloud_attr_pos);
    glEnableVertexAttribArray(data->cloud_attr_col);

    glBindBuffer(GL_ARRAY_BUFFER, data->skel_bones_bo);

    glVertexAttribPointer(data->cloud_attr_pos, 3, GL_FLOAT,
                          GL_FALSE, sizeof(XYZRGBA), nullptr);
    glVertexAttribPointer(data->cloud_attr_col, 4, GL_UNSIGNED_BYTE,
                          GL_TRUE, sizeof(XYZRGBA),
                          (void *)offsetof(XYZRGBA, rgba));

    glDrawArrays(GL_LINES, 0, n_bones * 2);

    glUniform1f(data->cloud_uniform_pt_size, pt_size * 3.f);

    glBindBuffer(GL_ARRAY_BUFFER, data->skel_joints_bo);

    glVertexAttribPointer(data->cloud_attr_pos, 3, GL_FLOAT,
                          GL_FALSE, sizeof(XYZRGBA), nullptr);
    glVertexAttribPointer(data->cloud_attr_col, 4, GL_UNSIGNED_BYTE,
                          GL_TRUE, sizeof(XYZRGBA),
                          (void *)offsetof(XYZRGBA, rgba));

    glEnable(GL_PROGRAM_POINT_SIZE);
    glDrawArrays(GL_POINTS, 0, n_joints);
    glDisable(GL_PROGRAM_POINT_SIZE);

    glDisableVertexAttribArray(data->cloud_attr_pos);
    glDisableVertexAttribArray(data->cloud_attr_col);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

static void
draw_debug_lines(Data *data, glm::mat4 mvp)
{
    if (!data->n_lines)
        return;

    glUseProgram(data->cloud_program);

    glUniformMatrix4fv(data->cloud_uniform_mvp, 1, GL_FALSE, glm::value_ptr(mvp));

    glEnableVertexAttribArray(data->cloud_attr_pos);
    glEnableVertexAttribArray(data->cloud_attr_col);

    glBindBuffer(GL_ARRAY_BUFFER, data->lines_bo);

    glVertexAttribPointer(data->cloud_attr_pos, 3, GL_FLOAT,
                          GL_FALSE, sizeof(struct gm_point_rgba), nullptr);
    glVertexAttribPointer(data->cloud_attr_col, 4, GL_UNSIGNED_BYTE,
                          GL_TRUE, sizeof(struct gm_point_rgba),
                          (void *)offsetof(struct gm_point_rgba, rgba));

    glDrawArrays(GL_LINES, 0, data->n_lines * 2);

    glDisableVertexAttribArray(data->cloud_attr_pos);
    glDisableVertexAttribArray(data->cloud_attr_col);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

static void
draw_tracking_scene_to_texture(Data *data,
                               struct gm_tracking *tracking,
                               ImVec2 win_size, ImVec2 uiScale)
{
    const struct gm_intrinsics *depth_intrinsics =
        gm_tracking_get_depth_camera_intrinsics(tracking);
    int depth_width = depth_intrinsics->width;

    GLint saved_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    // Ensure the framebuffer texture is valid
    if (!data->cloud_fbo_valid) {
        int width = win_size.x * uiScale.x;
        int height = win_size.y * uiScale.y;

        // Generate textures
        glBindTexture(GL_TEXTURE_2D, data->cloud_fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        // Bind colour/depth to point-cloud fbo
        glBindFramebuffer(GL_FRAMEBUFFER, data->cloud_fbo);

        glBindRenderbuffer(GL_RENDERBUFFER, data->cloud_depth_renderbuf);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                              width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, data->cloud_depth_renderbuf);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               data->cloud_fbo_tex, 0);

        GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, drawBuffers);

        gm_assert(data->log,
                  (glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                   GL_FRAMEBUFFER_COMPLETE),
                  "Incomplete framebuffer\n");

        data->cloud_fbo_valid = true;
    }

    if (data->cloud_bo) {
        glm::mat4 proj = intrinsics_to_project_matrix(depth_intrinsics, 0.01f, 10);
        glm::mat4 mvp = glm::scale(proj, glm::vec3(1.0, 1.0, -1.0));
        mvp = glm::translate(mvp, data->focal_point);
        mvp = glm::rotate(mvp, data->camera_rot_yx[0], glm::vec3(0.0, 1.0, 0.0));
        mvp = glm::rotate(mvp, data->camera_rot_yx[1], glm::vec3(1.0, 0.0, 0.0));
        mvp = glm::translate(mvp, -data->focal_point);

        glBindFramebuffer(GL_FRAMEBUFFER, data->cloud_fbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, win_size.x * uiScale.x, win_size.y * uiScale.y);

        glUseProgram(data->cloud_program);
        glUniformMatrix4fv(data->cloud_uniform_mvp, 1, GL_FALSE, glm::value_ptr(mvp));
        float pt_size = ceilf((win_size.x * uiScale.x) / depth_width);
        glUniform1f(data->cloud_uniform_pt_size, pt_size);

        glBindBuffer(GL_ARRAY_BUFFER, data->cloud_bo);
        if (data->cloud_attr_pos != -1) {
            glEnableVertexAttribArray(data->cloud_attr_pos);
            glVertexAttribPointer(data->cloud_attr_pos, 3, GL_FLOAT, GL_FALSE,
                                  sizeof(struct gm_point_rgba), 0);
        }
        glEnableVertexAttribArray(data->cloud_attr_col);
        glVertexAttribPointer(data->cloud_attr_col, 4, GL_UNSIGNED_BYTE,
                              GL_TRUE,
                              sizeof(struct gm_point_rgba),
                              (void *)offsetof(struct gm_point_rgba, rgba));

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDepthFunc(GL_LESS);

        glDrawArrays(GL_POINTS, 0, data->n_cloud_points);

        glDisable(GL_PROGRAM_POINT_SIZE);
        glDisable(GL_DEPTH_TEST);

        glDisableVertexAttribArray(data->cloud_attr_pos);
        if (data->cloud_attr_pos != -1)
            glDisableVertexAttribArray(data->cloud_attr_col);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);

        int n_joints = 0;
        int n_bones = 0;
        if (update_skeleton_wireframe_gl_bos(data,
                                             gm_tracking_get_timestamp(data->latest_tracking),
                                             &n_joints,
                                             &n_bones))
        {
            draw_skeleton_wireframe(data, mvp, pt_size, n_joints, n_bones);
        }

        draw_debug_lines(data, mvp);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
}

static bool
draw_cloud_visualisation(Data *data, ImVec2 &uiScale,
                         int x, int y, int width, int height)
{
    const struct gm_intrinsics *depth_intrinsics =
        gm_tracking_get_depth_camera_intrinsics(data->latest_tracking);
    int depth_width = depth_intrinsics->width;
    int depth_height = depth_intrinsics->height;

    bool focused = draw_visualisation(data, x, y, width, height,
                                      depth_width, depth_height,
                                      "Cloud", 0, GM_ROTATION_0);

    ImVec2 win_size = ImGui::GetContentRegionMax();
    adjust_aspect(win_size, depth_width, depth_height);
    draw_tracking_scene_to_texture(data, data->latest_tracking, win_size, uiScale);

    ImGui::Image((void *)(intptr_t)data->cloud_fbo_tex, win_size);

    // Handle input for cloud visualisation
    if (ImGui::IsWindowHovered()) {
        if (ImGui::IsMouseDragging()) {
            ImVec2 drag_delta = ImGui::GetMouseDragDelta();
            data->camera_rot_yx[0] += (drag_delta.x * M_PI / 180.f) * 0.2f;
            data->camera_rot_yx[1] += (drag_delta.y * M_PI / 180.f) * 0.2f;
            ImGui::ResetMouseDragDelta();
        }
    }

    ImGui::End();

    return focused;
}

static bool
draw_view(Data *data, int view, ImVec2 &uiScale,
          int x, int y, int width, int height, bool disabled)
{
    switch(view) {
    case 0:
        return draw_controls(data, x, y, width, height, disabled);
    case 1: {
        return draw_visualisation(data, x, y, width, height,
                                  data->video_rgb_width,
                                  data->video_rgb_height,
                                  views[view], data->video_rgb_tex,
                                  GM_ROTATION_0);
    }
    case 2:
        return draw_visualisation(data, x, y, width, height,
                                  data->depth_rgb_width,
                                  data->depth_rgb_height,
                                  views[view], gl_depth_rgb_tex,
                                  GM_ROTATION_0);
    case 3:
        return draw_visualisation(data, x, y, width, height,
                                  data->classify_rgb_width,
                                  data->classify_rgb_height,
                                  views[view], gl_classify_rgb_tex,
                                  GM_ROTATION_0);
    case 4:
        return draw_visualisation(data, x, y, width, height,
                                  data->cclusters_rgb_width,
                                  data->cclusters_rgb_height,
                                  views[view], gl_cclusters_rgb_tex,
                                  GM_ROTATION_0);
    case 5:
        return draw_visualisation(data, x, y, width, height,
                                  data->labels_rgb_width,
                                  data->labels_rgb_height,
                                  views[view], gl_labels_tex,
                                  GM_ROTATION_0);
    case 6:
        if (!data->latest_tracking) {
            return false;
        }
        return draw_cloud_visualisation(data, uiScale,
                                        x, y, width, height);
    }

    return false;
}

static void
draw_ui(Data *data)
{
    static int cloud_view = ARRAY_LEN(views) - 1;
    static int main_view = 1;
    int current_view = main_view;

    ProfileScopedSection(DrawIMGUI, ImGuiControl::Profiler::Dark);

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 uiScale = io.DisplayFramebufferScale;
    ImVec2 origin = io.DisplayVisibleMin;
    ImVec2 win_size = ImVec2(io.DisplayVisibleMax.x - io.DisplayVisibleMin.x,
                             io.DisplayVisibleMax.y - io.DisplayVisibleMin.y);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

    bool skip_controls = false;
    if (current_view != 0) {
        // Draw playback controls if UI controls isn't the main view
        draw_playback_controls(data, ImVec4(0, 0, win_size.x, win_size.y));
    }
    if (win_size.x >= 1024 && win_size.y >= 600) {
        // Draw control panel on the left if we have a large window
        draw_controls(data, origin.x, origin.y,
                      TOOLBAR_WIDTH + origin.x, win_size.y - origin.y, false);

        win_size.x -= TOOLBAR_WIDTH;
        origin.x += TOOLBAR_WIDTH;

        skip_controls = true;
    }
#if 1
    if (data->realtime_ar_mode) {
        // Draw a view-picker at the top
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSizeConstraints(ImVec2(win_size.x, 0),
                                            ImVec2(win_size.x, win_size.y));
        ImGui::Begin("View picker", NULL,
                     ImGuiWindowFlags_NoTitleBar|
                     ImGuiWindowFlags_NoResize|
                     ImGuiWindowFlags_NoMove|
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        /* XXX: assuming that "Controls" and "Video Buffer" are the first two
         * entries, we only want to expose these two options in
         * realtime_ar_mode, while we aren't uploading any other debug textures
         */
        if (ImGui::Button((main_view == 0) ? "Close" : "Properties")) {
            main_view = (main_view == 0) ? 1 : 0;
        }

        int x = origin.x;
        int y = ImGui::GetWindowHeight() + origin.y;
        ImVec2 main_area_size = ImVec2(win_size.x,
                                       win_size.y - ImGui::GetWindowHeight());

        ImGui::End();

        /* We only need to consider drawing the controls while in this mode
         * since we don't use imgui to render the video background while in
         * real-time mode
         */
        if (current_view == 0 && skip_controls == false) {
            draw_view(data, current_view, uiScale, x, y,
                      main_area_size.x, main_area_size.y, false);
        }
    } else {
        // Draw sub-views on the axis with the most space
        float depth_aspect = data->depth_rgb_height ?
            data->depth_rgb_width / (float)data->depth_rgb_height : 1.f;
        int view = skip_controls ? 1 : 0;
        int n_views = ARRAY_LEN(views) - (skip_controls ? 1 : 0);
        for (int s = 0; s <= (n_views - 1) / MAX_VIEWS; ++s) {
            int subview_width, subview_height;
            float win_aspect = win_size.x / (float)win_size.y;
            if (win_aspect > depth_aspect) {
                subview_height = win_size.y / MAX_VIEWS;
                subview_width = data->depth_rgb_height ?
                    subview_height * (data->depth_rgb_width /
                                      (float)data->depth_rgb_height) :
                    subview_height;
            } else {
                subview_width = win_size.x / MAX_VIEWS;
                subview_height = data->depth_rgb_width ?
                    subview_width * (data->depth_rgb_height /
                                     (float)data->depth_rgb_width) :
                    subview_width;
            }
            for (int i = 0; i < MAX_VIEWS; ++i, ++view) {
                if (view == current_view) {
                    ++view;
                }
                if (view >= (int)ARRAY_LEN(views)) {
                    break;
                }

                int x, y;
                if (win_aspect > depth_aspect) {
                    x = origin.x + win_size.x - subview_width;
                    y = origin.y + (subview_height * i);
                } else {
                    y = origin.y + (win_size.y - subview_height);
                    x = origin.x + (subview_width * i);
                }

                if (draw_view(data, view, uiScale, x, y,
                              subview_width, subview_height, view == 0)) {
                    main_view = view;
                }
            }

            if (win_aspect > depth_aspect) {
                win_size.x -= subview_width;
            } else {
                win_size.y -= subview_height;
            }
        }

        // Draw the main view in the remaining space in the center
        draw_view(data, current_view, uiScale, origin.x, origin.y,
                  win_size.x, win_size.y, false);
    }
#endif
    ImGui::PopStyleVar();

    if (data->show_profiler) {
        // Draw profiler window always-on-top
        ImGui::SetNextWindowPos(origin, ImGuiCond_Once);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        ProfileDrawUI();
    }

    ImGui::Render();

    // If we've toggled between the cloud view, invalidate the texture so
    // it gets recreated at the right size next time it's displayed.
    if (main_view != current_view &&
        (main_view == cloud_view || current_view == cloud_view)) {
        data->cloud_fbo_valid = false;
    }
}

static void
draw_ar_video(Data *data)
{
    if (!data->device_gl_initialized || data->last_video_frame == NULL)
        return;

    gm_assert(data->log, !!data->ctx, "draw_ar_video, NULL ctx");

    enum gm_rotation rotation = data->last_video_frame->camera_rotation;
    const struct gm_intrinsics *video_intrinsics =
        &data->last_video_frame->video_intrinsics;
    int video_width = video_intrinsics->width;
    int video_height = video_intrinsics->height;

    int aspect_width = video_width;
    int aspect_height = video_height;

    struct {
        float x, y, s, t;
    } xyst_verts[4] = {
        { -1,  1, 0, 0, }, //  0 -- 1
        {  1,  1, 1, 0, }, //  | \  |
        {  1, -1, 1, 1  }, //  |  \ |
        { -1, -1, 0, 1, }, //  3 -- 2
    };
    int n_verts = ARRAY_LEN(xyst_verts);

    gm_debug(data->log, "rendering background with camera rotation of %d degrees",
             ((int)rotation) * 90);

    switch (rotation) {
    case GM_ROTATION_0:
        break;
    case GM_ROTATION_90:
        xyst_verts[0].s = 1; xyst_verts[0].t = 0;
        xyst_verts[1].s = 1; xyst_verts[1].t = 1;
        xyst_verts[2].s = 0; xyst_verts[2].t = 1;
        xyst_verts[3].s = 0; xyst_verts[3].t = 0;
        std::swap(aspect_width, aspect_height);
        break;
    case GM_ROTATION_180:
        xyst_verts[0].s = 1; xyst_verts[0].t = 1;
        xyst_verts[1].s = 0; xyst_verts[1].t = 1;
        xyst_verts[2].s = 0; xyst_verts[2].t = 0;
        xyst_verts[3].s = 1; xyst_verts[3].t = 0;
        break;
    case GM_ROTATION_270:
        xyst_verts[0].s = 0; xyst_verts[0].t = 1;
        xyst_verts[1].s = 0; xyst_verts[1].t = 0;
        xyst_verts[2].s = 1; xyst_verts[2].t = 0;
        xyst_verts[3].s = 1; xyst_verts[3].t = 1;
        std::swap(aspect_width, aspect_height);
        break;
    }

    float display_aspect = data->win_width / (float)data->win_height;
    float video_aspect = aspect_width / (float)aspect_height;
    float aspect_x_scale = 1;
    float aspect_y_scale = 1;
    if (video_aspect > display_aspect) {
        // fit by scaling down y-axis of video
        float fit_height = (float)data->win_width / video_aspect;
        aspect_y_scale = fit_height / (float)data->win_height;
    } else {
        // fit by scaling x-axis of video
        float fit_width = video_aspect * data->win_height;
        aspect_x_scale = fit_width / (float)data->win_width;
    }

    gm_debug(data->log, "UVs: %f,%f %f,%f %f,%f, %f,%f",
             xyst_verts[0].s,
             xyst_verts[0].t,
             xyst_verts[1].s,
             xyst_verts[1].t,
             xyst_verts[2].s,
             xyst_verts[2].t,
             xyst_verts[3].s,
             xyst_verts[3].t);

    /* trivial enough to just do the transform on the cpu... */
    for (int i = 0; i < n_verts; i++) {
        xyst_verts[i].x *= aspect_x_scale;
        xyst_verts[i].y *= aspect_y_scale;
    }

    /* XXX: we could just cache buffers for each rotation */
    glBindBuffer(GL_ARRAY_BUFFER, data->video_quad_attrib_bo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * n_verts,
                 xyst_verts, GL_STATIC_DRAW);

    glUseProgram(data->video_program);
    glBindBuffer(GL_ARRAY_BUFFER, data->video_quad_attrib_bo);

    glEnableVertexAttribArray(data->video_quad_attrib_pos);
    glVertexAttribPointer(data->video_quad_attrib_pos,
                          2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void *)0);

    if (data->video_quad_attrib_tex_coords != -1) {
        glEnableVertexAttribArray(data->video_quad_attrib_tex_coords);
        glVertexAttribPointer(data->video_quad_attrib_tex_coords,
                              2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void *)8);
    }

    enum gm_device_type device_type = gm_device_get_type(data->active_device);
    GLenum target = GL_TEXTURE_2D;
    if (device_type == GM_DEVICE_TANGO)
        target = GL_TEXTURE_EXTERNAL_OES;
    GLuint ar_video_tex = get_oldest_ar_video_tex(data);
    glBindTexture(target, ar_video_tex);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_FAN, 0, n_verts);
    gm_debug(data->log, "draw_video");
    glDepthMask(GL_TRUE);

    glBindTexture(target, 0);

    glDisableVertexAttribArray(data->video_quad_attrib_pos);
    if (data->video_quad_attrib_tex_coords != -1)
        glDisableVertexAttribArray(data->video_quad_attrib_tex_coords);

    glUseProgram(0);

    if (data->latest_tracking) {
        struct gm_intrinsics rotated_intrinsics;

        gm_context_rotate_intrinsics(data->ctx,
                                     video_intrinsics,
                                     &rotated_intrinsics,
                                     rotation);

        float pt_size = ((float)data->win_width / 240.0f) * aspect_x_scale;
        glm::mat4 proj = intrinsics_to_project_matrix(&rotated_intrinsics, 0.01f, 10);
        glm::mat4 mvp = glm::scale(proj, glm::vec3(aspect_x_scale, -aspect_y_scale, -1.0));

        int n_joints = 0;
        int n_bones = 0;
        if (update_skeleton_wireframe_gl_bos(data,
                                             data->last_video_frame->timestamp -
                                             data->prediction_delay,
                                             &n_joints,
                                             &n_bones))
        {
            draw_skeleton_wireframe(data, mvp, pt_size, n_joints, n_bones);
        }
    }
}

/* If we've already requested gm_device for a frame then this won't submit
 * a request that downgrades the buffers_mask
 */
static void
request_device_frame(Data *data, uint64_t buffers_mask)
{
    uint64_t new_buffers_mask = data->pending_frame_buffers_mask | buffers_mask;

    if (data->pending_frame_buffers_mask != new_buffers_mask) {
        gm_device_request_frame(data->active_device, new_buffers_mask);
        data->pending_frame_buffers_mask = new_buffers_mask;
    }
}

static void
handle_device_frame_updates(Data *data)
{
    ProfileScopedSection(UpdatingDeviceFrame);
    bool upload_video_texture = false;

    if (!data->device_frame_ready)
        return;

    {
        ProfileScopedSection(GetLatestFrame);
        /* NB: gm_device_get_latest_frame will give us a _ref() */
        gm_frame *device_frame = gm_device_get_latest_frame(data->active_device);
        if (!device_frame) {
            return;
        }

        if (device_frame->depth) {
            if (data->last_depth_frame) {
                gm_frame_unref(data->last_depth_frame);
            }
            gm_frame_ref(device_frame);
            data->last_depth_frame = device_frame;
            data->pending_frame_buffers_mask &= ~GM_REQUEST_FRAME_DEPTH;
        }

        if (device_frame->video) {
            if (data->last_video_frame) {
                gm_frame_unref(data->last_video_frame);
            }
            gm_frame_ref(device_frame);
            data->last_video_frame = device_frame;
            data->pending_frame_buffers_mask &= ~GM_REQUEST_FRAME_VIDEO;
            upload_video_texture = true;
        }

        if (data->recording) {
            gm_recording_save_frame(data->recording, device_frame);
        }

        gm_frame_unref(device_frame);
    }

    if (data->context_needs_frame &&
        data->last_depth_frame && data->last_video_frame) {
        ProfileScopedSection(FwdContextFrame);

        // Combine the two video/depth frames into a single frame for gm_context
        if (data->last_depth_frame != data->last_video_frame) {
            struct gm_frame *full_frame =
                gm_device_combine_frames(data->active_device,
                                         data->last_depth_frame,
                                         data->last_depth_frame,
                                         data->last_video_frame);

            // We don't need the individual frames any more
            gm_frame_unref(data->last_depth_frame);
            gm_frame_unref(data->last_video_frame);

            data->last_depth_frame = full_frame;
            data->last_video_frame = gm_frame_ref(full_frame);
        }

        data->context_needs_frame =
            !gm_context_notify_frame(data->ctx, data->last_depth_frame);

        // We don't want to send duplicate frames to tracking, so discard now
        gm_frame_unref(data->last_depth_frame);
        data->last_depth_frame = NULL;
    }

    data->device_frame_ready = false;

    {
        ProfileScopedSection(DeviceFrameRequest);

        /* immediately request a new frame since we want to render the camera
         * at the native capture rate, even though we might not be tracking
         * at that rate.
         *
         * Similarly, if we're recording, request depth frames so that we can
         * record at a rate that exceeds the tracking rate.
         *
         * Note: the buffers_mask may be upgraded to ask for _DEPTH data
         * after the next iteration of skeltal tracking completes.
         */
        request_device_frame(data, data->recording ?
                             (GM_REQUEST_FRAME_DEPTH | GM_REQUEST_FRAME_VIDEO) :
                             GM_REQUEST_FRAME_VIDEO);
    }

    enum gm_device_type device_type = gm_device_get_type(data->active_device);

    if (upload_video_texture && data->device_gl_initialized) {
        if (device_type != GM_DEVICE_TANGO) {
            const struct gm_intrinsics *video_intrinsics =
                &data->last_video_frame->video_intrinsics;
            int video_width = video_intrinsics->width;
            int video_height = video_intrinsics->height;

            ProfileScopedSection(UploadFrameTextures);

            /*
             * Update video from camera
             */
            GLuint ar_video_tex = get_next_ar_video_tex(data);
            glBindTexture(GL_TEXTURE_2D, ar_video_tex);

            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            void *video_front = data->last_video_frame->video->data;
            enum gm_format video_format = data->last_video_frame->video_format;

            switch (video_format) {
            case GM_FORMAT_LUMINANCE_U8:
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                             video_width, video_height,
                             0, GL_LUMINANCE, GL_UNSIGNED_BYTE, video_front);
                break;

            case GM_FORMAT_RGB_U8:
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             video_width, video_height,
                             0, GL_RGB, GL_UNSIGNED_BYTE, video_front);
                break;
            case GM_FORMAT_BGR_U8:
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             video_width, video_height,
                             0, GL_BGR, GL_UNSIGNED_BYTE, video_front);
                break;

            case GM_FORMAT_RGBX_U8:
            case GM_FORMAT_RGBA_U8:
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             video_width, video_height,
                             0, GL_RGBA, GL_UNSIGNED_BYTE, video_front);
                break;
            case GM_FORMAT_BGRX_U8:
            case GM_FORMAT_BGRA_U8:
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             video_width, video_height,
                             0, GL_BGRA, GL_UNSIGNED_BYTE, video_front);
                break;

            case GM_FORMAT_UNKNOWN:
            case GM_FORMAT_Z_U16_MM:
            case GM_FORMAT_Z_F32_M:
            case GM_FORMAT_Z_F16_M:
            case GM_FORMAT_POINTS_XYZC_F32_M:
                gm_assert(data->log, 0, "Unexpected format for video buffer");
                break;
            }
        } else {
#ifdef USE_TANGO
            GLuint ar_video_tex = get_next_ar_video_tex(data);
            if (TangoService_updateTextureExternalOes(
                    TANGO_CAMERA_COLOR, ar_video_tex,
                    NULL /* ignore timestamp */) != TANGO_SUCCESS)
            {
                gm_warn(data->log, "Failed to update video frame via TangoService_updateTextureExternalOes");
            }
#endif
        }
    }
}

static void
upload_tracking_textures(Data *data)
{
    /* The tracking textures are all for debug purposes and we want to skip
     * the overhead of uploading them while in realtime_ar_mode
     */
    if (data->realtime_ar_mode)
        return;

    ProfileScopedSection(UploadTrackingBufs);

    /*
     * Update the RGB visualization of the depth buffer
     */
    uint8_t *depth_rgb = NULL;
    gm_tracking_create_rgb_depth(data->latest_tracking,
                                 &data->depth_rgb_width,
                                 &data->depth_rgb_height,
                                 &depth_rgb);

    if (depth_rgb) {
        glBindTexture(GL_TEXTURE_2D, gl_depth_rgb_tex);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        /* NB: gles2 only allows npot textures with clamp to edge
         * coordinate wrapping
         */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     data->depth_rgb_width, data->depth_rgb_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, depth_rgb);
        free(depth_rgb);
    }

    int n_points = 0;
    const struct gm_point_rgba *debug_points =
        gm_tracking_get_debug_point_cloud(data->latest_tracking, &n_points);
    if (n_points) {
        if (!data->cloud_bo)
            glGenBuffers(1, &data->cloud_bo);
        glBindBuffer(GL_ARRAY_BUFFER, data->cloud_bo);
        glBufferData(GL_ARRAY_BUFFER,
                     sizeof(debug_points[0]) * n_points,
                     debug_points, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        data->n_cloud_points = n_points;
    }

    int n_lines = 0;
    const struct gm_point_rgba *debug_lines =
        gm_tracking_get_debug_lines(data->latest_tracking, &n_lines);
    if (n_lines) {
        if (!data->lines_bo)
            glGenBuffers(1, &data->lines_bo);
        glBindBuffer(GL_ARRAY_BUFFER, data->lines_bo);
        glBufferData(GL_ARRAY_BUFFER,
                     sizeof(debug_lines[0]) * n_lines * 2,
                     debug_lines, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        data->n_lines = n_lines;
    }

    uint8_t *video_rgb = NULL;
    gm_tracking_create_rgb_video(data->latest_tracking,
                                 &data->video_rgb_width,
                                 &data->video_rgb_height,
                                 &video_rgb);
    if (video_rgb) {
        glBindTexture(GL_TEXTURE_2D, data->video_rgb_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     data->video_rgb_width, data->video_rgb_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, video_rgb);
        free(video_rgb);
    }

    /* Update depth classification buffer */
    uint8_t *classify_rgb = NULL;
    gm_tracking_create_rgb_depth_classification(data->latest_tracking,
                                                &data->classify_rgb_width,
                                                &data->classify_rgb_height,
                                                &classify_rgb);

    if (classify_rgb) {
        glBindTexture(GL_TEXTURE_2D, gl_classify_rgb_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     data->classify_rgb_width, data->classify_rgb_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, classify_rgb);
        free(classify_rgb);
    }

    /* Update candidate clusters buffer */
    uint8_t *cclusters_rgb = NULL;
    gm_tracking_create_rgb_candidate_clusters(data->latest_tracking,
                                              &data->cclusters_rgb_width,
                                              &data->cclusters_rgb_height,
                                              &cclusters_rgb);

    if (cclusters_rgb) {
        glBindTexture(GL_TEXTURE_2D, gl_cclusters_rgb_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     data->cclusters_rgb_width, data->cclusters_rgb_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, cclusters_rgb);
        free(cclusters_rgb);
    }

    /*
     * Update inferred label map
     */
    uint8_t *labels_rgb = NULL;
    gm_tracking_create_rgb_label_map(data->latest_tracking,
                                     &data->labels_rgb_width,
                                     &data->labels_rgb_height,
                                     &labels_rgb);

    if (labels_rgb) {
        glBindTexture(GL_TEXTURE_2D, gl_labels_tex);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        /* NB: gles2 only allows npot textures with clamp to edge
         * coordinate wrapping
         */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     data->labels_rgb_width, data->labels_rgb_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, labels_rgb);
        free(labels_rgb);
    }
}

static void
destroy_joints_recording(Data *data)
{
    if (data->joints_recording_val) {
        json_value_free(data->joints_recording_val);
        data->joints_recording_val = NULL;
        data->joints_recording = NULL;
    }
}

static void
start_joints_recording(Data *data)
{
    destroy_joints_recording(data);
    data->joints_recording_val = json_value_init_array();
    data->joints_recording = json_array(data->joints_recording_val);
}

static void
handle_context_tracking_updates(Data *data)
{
    ProfileScopedSection(UpdatingTracking);

    if (!data->tracking_ready)
        return;

    data->tracking_ready = false;

    if (data->latest_tracking)
        gm_tracking_unref(data->latest_tracking);

    data->latest_tracking = gm_context_get_latest_tracking(data->ctx);

    // When flushing the context, we can end up with notified tracking but
    // no tracking to pick up
    if (!data->latest_tracking) {
        return;
    }

    if (data->joints_recording) {
        int n_joints;
        const float *joints =
            gm_tracking_get_joint_positions(data->latest_tracking,
                                            &n_joints);
        JSON_Value *joints_array_val = json_value_init_array();
        JSON_Array *joints_array = json_array(joints_array_val);
        for (int i = 0; i < n_joints; i++) {
            const float *joint = joints + 3 * i;
            JSON_Value *coord_val = json_value_init_array();
            JSON_Array *coord = json_array(coord_val);

            json_array_append_number(coord, joint[0]);
            json_array_append_number(coord, joint[1]);
            json_array_append_number(coord, joint[2]);

            json_array_append_value(joints_array, coord_val);
        }

        json_array_append_value(data->joints_recording, joints_array_val);

        int n_frames = json_array_get_count(data->joints_recording);
        if (n_frames >= data->requested_recording_len) {
            json_serialize_to_file_pretty(data->joints_recording_val,
                                          "glimpse-joints-recording.json");
            destroy_joints_recording(data);
        }
    }

    upload_tracking_textures(data);
}

static void
handle_device_ready(Data *data, struct gm_device *dev)
{
    gm_debug(data->log, "%s device ready\n",
            dev == data->playback_device ? "Playback" : "Default");

    init_viewer_opengl(data);
    init_device_opengl(data);

    int max_depth_pixels =
        gm_device_get_max_depth_pixels(dev);
    gm_context_set_max_depth_pixels(data->ctx, max_depth_pixels);

    int max_video_pixels =
        gm_device_get_max_video_pixels(dev);
    gm_context_set_max_video_pixels(data->ctx, max_video_pixels);

    /*gm_context_set_depth_to_video_camera_extrinsics(data->ctx,
      gm_device_get_depth_to_video_extrinsics(dev));*/

    uint64_t old_reqs = data->pending_frame_buffers_mask;
    data->pending_frame_buffers_mask = 0;
    gm_device_start(dev);
    gm_context_enable(data->ctx);
    if (old_reqs) {
        request_device_frame(data, old_reqs);
    }

    if (data->requested_recording_len)
        start_joints_recording(data);
}

static void
handle_device_event(Data *data, struct gm_device_event *event)
{
    // Ignore unexpected device events
    if (event->device != data->active_device) {
        gm_device_event_free(event);
        return;
    }

    switch (event->type) {
    case GM_DEV_EVENT_READY:
        handle_device_ready(data, event->device);
        break;
    case GM_DEV_EVENT_FRAME_READY:
        if (event->frame_ready.buffers_mask & data->pending_frame_buffers_mask)
        {
            data->device_frame_ready = true;
        }
        break;
    }

    gm_device_event_free(event);
}

static void
handle_context_event(Data *data, struct gm_event *event)
{
    switch (event->type) {
    case GM_EVENT_REQUEST_FRAME:
        gm_debug(data->log, "Requesting frame\n");
        data->context_needs_frame = true;
        request_device_frame(data,
                             (GM_REQUEST_FRAME_DEPTH |
                              GM_REQUEST_FRAME_VIDEO));
        break;
    case GM_EVENT_TRACKING_READY:
        data->tracking_ready = true;
        break;
    }

    gm_context_event_free(event);
}

static void
event_loop_iteration(Data *data)
{
    {
        ProfileScopedSection(GlimpseEvents);
        pthread_mutex_lock(&data->event_queue_lock);
        std::swap(data->events_front, data->events_back);
        pthread_mutex_unlock(&data->event_queue_lock);

        for (unsigned i = 0; i < data->events_front->size(); i++) {
            struct event event = (*data->events_front)[i];

            switch (event.type) {
            case EVENT_DEVICE:
                handle_device_event(data, event.device_event);
                break;
            case EVENT_CONTEXT:
                handle_context_event(data, event.context_event);
                break;
            }
        }

        data->events_front->clear();
    }

    handle_device_frame_updates(data);
    handle_context_tracking_updates(data);

    {
        ProfileScopedSection(GlimpseGPUHook);
        gm_context_render_thread_hook(data->ctx);
    }

}

#ifdef USE_GLFM
static void
surface_created_cb(GLFMDisplay *display, int width, int height)
{
    Data *data = (Data *)glfmGetUserData(display);

    gm_debug(data->log, "Surface created (%dx%d)", width, height);

    if (!data->surface_created) {
        init_basic_opengl(data);
        data->surface_created = true;
    }

    data->win_width = width;
    data->win_height = height;
    data->cloud_fbo_valid = false;
}

static void
surface_destroyed_cb(GLFMDisplay *display)
{
    Data *data = (Data *)glfmGetUserData(display);
    gm_debug(data->log, "Surface destroyed");
    data->surface_created = false;
    data->cloud_fbo_valid = false;
}

static void
app_focus_cb(GLFMDisplay *display, bool focused)
{
    Data *data = (Data *)glfmGetUserData(display);
    gm_debug(data->log, focused ? "Focused" : "Unfocused");

    if (focused) {
        if (data->playback_device) {
            gm_device_start(data->playback_device);
        } else {
            gm_device_start(data->recording_device);
        }
    } else {
        if (data->playback_device) {
            gm_device_stop(data->playback_device);
        } else {
            gm_device_stop(data->recording_device);
        }
    }
}

static void
frame_cb(GLFMDisplay* display, double frameTime)
{
    Data *data = (Data*)glfmGetUserData(display);

    if (permissions_check_passed) {
        if (!data->initialized)
            viewer_init(data);

        ProfileNewFrame();
        ProfileScopedSection(Frame);
        event_loop_iteration(data);

        {
            ProfileScopedSection(Redraw);

            glViewport(0, 0, data->win_width, data->win_height);
            glClear(GL_COLOR_BUFFER_BIT);
            if (data->realtime_ar_mode)
                draw_ar_video(data);
            ImGui_ImplGlfmGLES3_NewFrame(display, frameTime);
            draw_ui(data);
        }

    } else if (permissions_check_failed) {
        /* At least some visual feedback that we failed to
         * acquire the permissions we need...
         */
        glClearColor(1.0, 0.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        glClear(GL_COLOR_BUFFER_BIT);
    }
}
#endif

#ifdef USE_GLFW
static void
event_loop(Data *data)
{
    while (!glfwWindowShouldClose(data->window)) {
        ProfileNewFrame();

        ProfileScopedSection(Frame);

        {
            ProfileScopedSection(GLFWEvents);
            glfwPollEvents();
        }

        event_loop_iteration(data);

        {
            ProfileScopedSection(Redraw);

            glViewport(0, 0, data->win_width, data->win_height);
            glClear(GL_COLOR_BUFFER_BIT);
            if (data->realtime_ar_mode)
                draw_ar_video(data);
            ImGui_ImplGlfwGLES3_NewFrame();
            draw_ui(data);
        }

        {
            ProfileScopedSection(SwapBuffers);
            glfwSwapBuffers(data->window);
        }
    }
}

static void
on_window_fb_size_change_cb(GLFWwindow *window, int width, int height)
{
    Data *data = (Data *)glfwGetWindowUserPointer(window);

    data->win_width = width;
    data->win_height = height;
    data->cloud_fbo_valid = false;
}

static void
on_key_input_cb(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    Data *data = (Data *)glfwGetWindowUserPointer(window);

    if (action != GLFW_PRESS)
        return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
    case GLFW_KEY_Q:
        glfwSetWindowShouldClose(data->window, 1);
        break;
    }

    ImGui_ImplGlfwGLES3_KeyCallback(window, key, scancode, action, mods);
}

static void
on_glfw_error_cb(int error_code, const char *error_msg)
{
    fprintf(stderr, "GLFW ERROR: %d: %s\n", error_code, error_msg);
}
#endif

static void
on_khr_debug_message_cb(GLenum source,
                        GLenum type,
                        GLuint id,
                        GLenum gl_severity,
                        GLsizei length,
                        const GLchar *message,
                        void *user_data)
{
    Data *data = (Data *)user_data;

    switch (gl_severity) {
    case GL_DEBUG_SEVERITY_HIGH:
        gm_log(data->log, GM_LOG_ERROR, "Viewer GL", "%s", message);
        break;
    case GL_DEBUG_SEVERITY_MEDIUM:
        gm_log(data->log, GM_LOG_WARN, "Viewer GL", "%s", message);
        break;
    case GL_DEBUG_SEVERITY_LOW:
        gm_log(data->log, GM_LOG_WARN, "Viewer GL", "%s", message);
        break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
        gm_log(data->log, GM_LOG_INFO, "Viewer GL", "%s", message);
        break;
    }
}

/* NB: it's undefined what thread this is called on so we queue events to
 * be processed as part of the mainloop processing.
 */
static void
on_event_cb(struct gm_context *ctx,
            struct gm_event *context_event, void *user_data)
{
    Data *data = (Data *)user_data;

    struct event event = {};
    event.type = EVENT_CONTEXT;
    event.context_event = context_event;

    pthread_mutex_lock(&data->event_queue_lock);
    data->events_back->push_back(event);
    pthread_mutex_unlock(&data->event_queue_lock);
}

static void
on_device_event_cb(struct gm_device_event *device_event,
                   void *user_data)
{
    Data *data = (Data *)user_data;

    struct event event = {};
    event.type = EVENT_DEVICE;
    event.device_event = device_event;

    pthread_mutex_lock(&data->event_queue_lock);
    data->events_back->push_back(event);
    pthread_mutex_unlock(&data->event_queue_lock);
}

/* Initialize enough OpenGL state to handle rendering before being
 * notified that the Glimpse device is 'ready' (i.e. before it's
 * possible to query camera intrinsics)
 */
static void
init_basic_opengl(Data *data)
{
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClearStencil(0);
#if 0
    glDebugMessageControl(GL_DONT_CARE, /* source */
                          GL_DONT_CARE, /* type */
                          GL_DONT_CARE, /* severity */
                          0,
                          NULL,
                          false);

    glDebugMessageControl(GL_DONT_CARE, /* source */
                          GL_DEBUG_TYPE_ERROR,
                          GL_DONT_CARE, /* severity */
                          0,
                          NULL,
                          true);

    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback((GLDEBUGPROC)on_khr_debug_message_cb, data);
#endif

#if defined(__APPLE__) && !defined(__IOS__)
    // In the forwards-compatible context, there's no default vertex array.
    GLuint vertex_array;
    glGenVertexArrays(1, &vertex_array);
    glBindVertexArray(vertex_array);
#endif
}

static void
init_viewer_opengl(Data *data)
{
    if (data->gl_initialized)
        return;

    static const char *cloud_vert_shader =
        GLSL_SHADER_VERSION
        "precision mediump float;\n"
        "uniform mat4 mvp;\n"
        "uniform float size;\n"
        "in vec3 pos;\n"
        "in vec4 color_in;\n"
        "out vec4 v_color;\n"
        "\n"
        "void main() {\n"
        "  gl_PointSize = size;\n"
        "  gl_Position =  mvp * vec4(pos.x, pos.y, pos.z, 1.0);\n"
        "  v_color = color_in;\n"
        "}\n";

    static const char *cloud_frag_shader =
        GLSL_SHADER_VERSION
        "precision mediump float;\n"
        "in vec4 v_color;\n"
        "layout(location = 0) out vec4 color;\n"
        "void main() {\n"
        "  color = v_color.abgr;\n"
        "}\n";

    data->cloud_program = gm_gl_create_program(data->log,
                                               cloud_vert_shader,
                                               cloud_frag_shader,
                                               NULL);

    glUseProgram(data->cloud_program);

    data->cloud_attr_pos = glGetAttribLocation(data->cloud_program, "pos");
    data->cloud_attr_col = glGetAttribLocation(data->cloud_program, "color_in");
    data->cloud_uniform_mvp = glGetUniformLocation(data->cloud_program, "mvp");
    data->cloud_uniform_pt_size = glGetUniformLocation(data->cloud_program, "size");

    glUseProgram(0);

    glGenBuffers(1, &data->lines_bo);
    glGenBuffers(1, &data->skel_bones_bo);
    glGenBuffers(1, &data->skel_joints_bo);

    // Generate texture objects
    glGenTextures(1, &gl_depth_rgb_tex);
    glBindTexture(GL_TEXTURE_2D, gl_depth_rgb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &gl_classify_rgb_tex);
    glBindTexture(GL_TEXTURE_2D, gl_classify_rgb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &gl_cclusters_rgb_tex);
    glBindTexture(GL_TEXTURE_2D, gl_cclusters_rgb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &gl_labels_tex);
    glBindTexture(GL_TEXTURE_2D, gl_labels_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenTextures(1, &data->video_rgb_tex);
    glBindTexture(GL_TEXTURE_2D, data->video_rgb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &data->cloud_fbo);
    glGenRenderbuffers(1, &data->cloud_depth_renderbuf);
    glGenTextures(1, &data->cloud_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, data->cloud_fbo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glGenBuffers(1, &data->video_quad_attrib_bo);

    data->gl_initialized = true;
}

static void
init_device_opengl(Data *data)
{
    if (data->device_gl_initialized)
        return;

    gm_assert(data->log, data->video_program == 0,
              "Spurious GL video_program while device_gl_initialized == false");

    const char *vert_shader =
        GLSL_SHADER_VERSION
        "precision mediump float;\n"
        "precision mediump int;\n"
        "in vec2 pos;\n"
        "in vec2 tex_coords_in;\n"
        "out vec2 tex_coords;\n"
        "void main() {\n"
        "  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);\n"
        "  tex_coords = tex_coords_in;\n"
        "}\n";
    const char *frag_shader =
        GLSL_SHADER_VERSION
        "precision highp float;\n"
        "precision highp int;\n"
        "uniform sampler2D tex_sampler;\n"
        "in vec2 tex_coords;\n"
        "out lowp vec4 frag_color;\n"
        "void main() {\n"
        "  frag_color = texture(tex_sampler, tex_coords);\n"
        "}\n";
    const char *external_tex_frag_shader =
        GLSL_SHADER_VERSION
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "uniform samplerExternalOES tex_sampler;\n"
        "in vec2 tex_coords;\n"
        "out lowp vec4 frag_color;\n"
        "void main() {\n"
        "  frag_color = texture(tex_sampler, tex_coords);\n"
        "}\n";

    if (gm_device_get_type(data->active_device) == GM_DEVICE_TANGO) {
        data->video_program = gm_gl_create_program(data->log,
                                                   vert_shader,
                                                   external_tex_frag_shader,
                                                   NULL);
    } else {
        data->video_program = gm_gl_create_program(data->log,
                                                   vert_shader,
                                                   frag_shader,
                                                   NULL);
    }

    data->video_quad_attrib_pos =
        glGetAttribLocation(data->video_program, "pos");
    data->video_quad_attrib_tex_coords =
        glGetAttribLocation(data->video_program, "tex_coords_in");

    data->ar_video_tex_sampler = glGetUniformLocation(data->video_program, "tex_sampler");

    glUseProgram(data->video_program);
    glUniform1i(data->ar_video_tex_sampler, 0);
    glUseProgram(0);
    update_ar_video_queue_len(data, 6);

    // XXX: inconsistent that cloud_fbo is allocated in init_viewer_opengl
    data->cloud_fbo_valid = false;

    data->device_gl_initialized = true;
}

static void
deinit_device_opengl(Data *data)
{
    if (!data->device_gl_initialized)
        return;

    if (data->video_program) {
        glDeleteProgram(data->video_program);
        data->video_program = 0;

        data->video_quad_attrib_pos = 0;
        data->video_quad_attrib_tex_coords = 0;
        data->ar_video_tex_sampler = 0;
    }

    update_ar_video_queue_len(data, 0);

    // XXX: inconsistent that cloud_fbo is allocated in init_viewer_opengl
    data->cloud_fbo_valid = false;

    data->device_gl_initialized = false;
}

static void
logger_cb(struct gm_logger *logger,
          enum gm_log_level level,
          const char *context,
          struct gm_backtrace *backtrace,
          const char *format,
          va_list ap,
          void *user_data)
{
    Data *data = (Data *)user_data;
    char *msg = NULL;

    if (vasprintf(&msg, format, ap) > 0) {
#ifdef __ANDROID__
        switch (level) {
        case GM_LOG_ASSERT:
            __android_log_print(ANDROID_LOG_FATAL, context, "%s", msg);
            break;
        case GM_LOG_ERROR:
            __android_log_print(ANDROID_LOG_ERROR, context, "%s", msg);
            break;
        case GM_LOG_WARN:
            __android_log_print(ANDROID_LOG_WARN, context, "%s", msg);
            break;
        case GM_LOG_INFO:
            __android_log_print(ANDROID_LOG_INFO, context, "%s", msg);
            break;
        case GM_LOG_DEBUG:
            __android_log_print(ANDROID_LOG_DEBUG, context, "%s", msg);
            break;
        }
#endif

        if (data->log_fp) {
            switch (level) {
            case GM_LOG_ERROR:
                fprintf(data->log_fp, "%s: ERROR: ", context);
                break;
            case GM_LOG_WARN:
                fprintf(data->log_fp, "%s: WARN: ", context);
                break;
            default:
                fprintf(data->log_fp, "%s: ", context);
            }

            fprintf(data->log_fp, "%s\n", msg);
#ifdef __IOS__
            ios_log(msg);
#endif

            if (backtrace) {
                int line_len = 100;
                char *formatted = (char *)alloca(backtrace->n_frames * line_len);

                gm_logger_get_backtrace_strings(logger, backtrace,
                                                line_len, (char *)formatted);
                for (int i = 0; i < backtrace->n_frames; i++) {
                    char *line = formatted + line_len * i;
                    fprintf(data->log_fp, "> %s\n", line);
                }
            }

            fflush(data->log_fp);
            fflush(stdout);
        }

        free(msg);
    }
}

static void
logger_abort_cb(struct gm_logger *logger,
                void *user_data)
{
    Data *data = (Data *)user_data;

    if (data->log_fp) {
        fprintf(data->log_fp, "ABORT\n");
        fflush(data->log_fp);
        fclose(data->log_fp);
    }

    abort();
}

#ifdef USE_GLFM
static void
init_winsys_glfm(Data *data, GLFMDisplay *display)
{
    glfmSetDisplayConfig(display,
                         GLFMRenderingAPIOpenGLES3,
                         GLFMColorFormatRGBA8888,
                         GLFMDepthFormatNone,
                         GLFMStencilFormatNone,
                         GLFMMultisampleNone);
    glfmSetDisplayChrome(display,
                         GLFMUserInterfaceChromeNavigationAndStatusBar);
    glfmSetUserData(display, data);
    glfmSetSurfaceCreatedFunc(display, surface_created_cb);
    glfmSetSurfaceResizedFunc(display, surface_created_cb);
    glfmSetSurfaceDestroyedFunc(display, surface_destroyed_cb);
    glfmSetAppFocusFunc(display, app_focus_cb);
    glfmSetMainLoopFunc(display, frame_cb);

    ImGui_ImplGlfmGLES3_Init(display, true);

    // Quick hack to make scrollbars a bit more usable on small devices
    ImGui::GetStyle().ScrollbarSize *= 2;
}
#endif

#ifdef USE_GLFW
static void
init_winsys_glfw(Data *data)
{
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW, OpenGL windows system library\n");
        exit(1);
    }

    data->win_width = 1280;
    data->win_height = 720;

#if defined(__APPLE__) && !defined(__IOS__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3) ;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,  2) ;
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3) ;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,  0) ;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#endif

    data->window = glfwCreateWindow(data->win_width,
                                    data->win_height,
                                    "Glimpse Viewer", NULL, NULL);
    if (!data->window) {
        fprintf(stderr, "Failed to create window\n");
        exit(1);
    }

    glfwSetWindowUserPointer(data->window, data);

    glfwSetFramebufferSizeCallback(data->window, on_window_fb_size_change_cb);

    glfwMakeContextCurrent(data->window);
    glfwSwapInterval(1);

    glfwSetErrorCallback(on_glfw_error_cb);

    ImGui_ImplGlfwGLES3_Init(data->window, false /* don't install callbacks */);

    /* will chain on to ImGui_ImplGlfwGLES3_KeyCallback... */
    glfwSetKeyCallback(data->window, on_key_input_cb);
    glfwSetMouseButtonCallback(data->window,
                               ImGui_ImplGlfwGLES3_MouseButtonCallback);
    glfwSetScrollCallback(data->window, ImGui_ImplGlfwGLES3_ScrollCallback);
    glfwSetCharCallback(data->window, ImGui_ImplGlfwGLES3_CharCallback);

    init_basic_opengl(data);
}
#endif // USE_GLFW

static void __attribute__((unused))
viewer_destroy(Data *data)
{
    if (data->playback_device) {
        viewer_close_playback_device(data);
    }

    /* Destroying the context' tracking pool will assert that all tracking
     * resources have been released first...
     */
    if (data->latest_tracking)
        gm_tracking_unref(data->latest_tracking);

    /* NB: It's our responsibility to be sure that there can be no asynchonous
     * calls into the gm_context api before we start to destroy it!
     *
     * We stop the device first because device callbacks result in calls
     * through to the gm_context api.
     *
     * We don't destroy the device first because destroying the context will
     * release device resources (which need to be release before the device
     * can be cleanly closed).
     */
    gm_device_stop(data->recording_device);

    for (unsigned i = 0; i < data->events_back->size(); i++) {
        struct event event = (*data->events_back)[i];

        switch (event.type) {
        case EVENT_DEVICE:
            gm_device_event_free(event.device_event);
            break;
        case EVENT_CONTEXT:
            gm_context_event_free(event.context_event);
            break;
        }
    }

    gm_context_destroy(data->ctx);

    unref_device_frames(data);

    gm_device_close(data->recording_device);

    json_value_free(data->joint_map);

    gm_logger_destroy(data->log);

    delete data->events_front;
    delete data->events_back;

#ifdef USE_GLFW
    ImGui_ImplGlfwGLES3_Shutdown();
    glfwDestroyWindow(data->window);
    glfwTerminate();
#endif

    delete data;

    ProfileShutdown();
}

static void
viewer_init(Data *data)
{
    ImGuiIO& io = ImGui::GetIO();

    char *open_err = NULL;
    struct gm_asset *font_asset = gm_asset_open(data->log,
                                                "Roboto-Medium.ttf",
                                                GM_ASSET_MODE_BUFFER,
                                                &open_err);
    if (font_asset) {
        const void *buf = gm_asset_get_buffer(font_asset);

        unsigned len = gm_asset_get_length(font_asset);
        void *buf_copy = ImGui::MemAlloc(len);
        memcpy(buf_copy, buf, len);

        ImVec2 uiScale = io.DisplayFramebufferScale;
        io.Fonts->AddFontFromMemoryTTF(buf_copy, 16.f, 16.f * uiScale.x);
        gm_asset_close(font_asset);
    } else {
        gm_error(data->log, "%s", open_err);
        exit(1);
    }

    const char *n_frames_env = getenv("GLIMPSE_RECORD_N_JOINT_FRAMES");
    if (n_frames_env)
        data->requested_recording_len = strtoull(n_frames_env, NULL, 10);

    // TODO: Might be nice to be able to retrieve this information via the API
    //       rather than reading it separately here.
    struct gm_asset *joint_map_asset = gm_asset_open(data->log,
                                                     "joint-map.json",
                                                     GM_ASSET_MODE_BUFFER,
                                                     &open_err);
    if (joint_map_asset) {
        const void *buf = gm_asset_get_buffer(joint_map_asset);
        data->joint_map = json_parse_string((const char *)buf);
        gm_asset_close(joint_map_asset);
    } else {
        gm_error(data->log, "%s", open_err);
        exit(1);
    }

    // Count the number of bones defined by connections in the joint map.
    data->n_bones = 0;
    for (size_t i = 0; i < json_array_get_count(json_array(data->joint_map));
         i++) {
        JSON_Object *joint =
            json_array_get_object(json_array(data->joint_map), i);
        data->n_bones += json_array_get_count(
            json_object_get_array(joint, "connections"));
    }
    data->n_joints = json_array_get_count(json_array(data->joint_map));

    ProfileInitialize(&pause_profile, on_profiler_pause_cb);

    data->ctx = gm_context_new(data->log, NULL);

    gm_context_set_event_callback(data->ctx, on_event_cb, data);

    /* TODO: load config for viewer properties */
    data->prediction_delay = 250000000;

    struct gm_asset *config_asset =
        gm_asset_open(data->log,
                      "glimpse-config.json", GM_ASSET_MODE_BUFFER, &open_err);
    if (config_asset) {
        const char *buf = (const char *)gm_asset_get_buffer(config_asset);
        JSON_Value *json_props = json_parse_string(buf);
        gm_props_from_json(data->log,
                           gm_context_get_ui_properties(data->ctx),
                           json_props);
        json_value_free(json_props);
        gm_asset_close(config_asset);
    } else {
        gm_warn(data->log, "Failed to open glimpse-config.json: %s", open_err);
        free(open_err);
    }

    struct gm_device_config config = {};
#ifdef USE_TANGO
    config.type = GM_DEVICE_TANGO;
#elif defined(USE_AVF)
    config.type = GM_DEVICE_AVF;
#else
    config.type = device_type_opt;
    char rec_path[1024];
    if (config.type == GM_DEVICE_RECORDING) {
        xsnprintf(rec_path, sizeof(rec_path), "%s/%s",
                  glimpse_recordings_path, device_recording_opt);
        config.recording.path = rec_path;
    }
#endif
    data->recording_device = gm_device_open(data->log, &config, NULL);
    data->active_device = data->recording_device;
    gm_device_set_event_callback(data->recording_device, on_device_event_cb, data);
#ifdef __ANDROID__
    gm_device_attach_jvm(data->recording_device, android_jvm_singleton);
#endif
    gm_device_commit_config(data->recording_device, NULL);

    if (config.type == GM_DEVICE_TANGO ||
        config.type == GM_DEVICE_AVF)
    {
        data->realtime_ar_mode = true;
    } else {
        struct gm_ui_properties *ctx_props =
            gm_context_get_ui_properties(data->ctx);
        data->realtime_ar_mode = false;
        gm_prop_set_enum(find_prop(ctx_props, "cloud_mode"), 1);
    }

    update_ar_video_queue_len(data, 6);

    data->initialized = true;
}

#if !defined(USE_GLFM)
static void
usage(void)
{
    printf(
"Usage glimpse_viewer [options]\n"
"\n"
"    -d,--device=DEV            Device type to use\n\n"
"                               - kinect:    Either a Kinect camera or Fakenect\n"
"                                            recording (default)\n"
"                               - recording: A glimpse_viewer recording (must\n"
"                                            pass -r/--recording option too)\n"
"    -r,--recording=NAME        Name or recording to play\n"
"\n"
"    -h,--help                  Display this help\n\n"
"\n"
    );

    exit(1);
}

static void
parse_args(Data *data, int argc, char **argv)
{
    int opt;

#define DEVICE_OPT              (CHAR_MAX + 1)
#define RECORDING_OPT           (CHAR_MAX + 1)

    /* N.B. The initial '+' means that getopt will stop looking for options
     * after the first non-option argument...
     */
    const char *short_options="+hd:r:";
    const struct option long_options[] = {
        {"help",            no_argument,        0, 'h'},
        {"device",          required_argument,  0, DEVICE_OPT},
        {"recording",       required_argument,  0, RECORDING_OPT},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL))
           != -1)
    {
        switch (opt) {
            case 'h':
                usage();
                return;
            case 'd':
                if (strcmp(optarg, "kinect") == 0)
                    device_type_opt = GM_DEVICE_KINECT;
                else if (strcmp(optarg, "recording") == 0)
                    device_type_opt = GM_DEVICE_RECORDING;
                else
                    usage();
                break;
            case 'r':
                    device_recording_opt = strdup(optarg);
                break;
            default:
                usage();
                break;
        }
    }
}
#endif

#ifdef USE_GLFM
void
glfmMain(GLFMDisplay *display)
#else  // USE_GLFW
int
main(int argc, char **argv)
#endif
{
    Data *data = new Data();
    const char *recordings_path = NULL;
#ifdef __IOS__
    char *assets_root = ios_util_get_documents_path();
    char log_filename_tmp[PATH_MAX];
    snprintf(log_filename_tmp, sizeof(log_filename_tmp),
             "%s/glimpse.log", assets_root);
    data->log_fp = fopen(log_filename_tmp, "w");
    char recordings_path_tmp[PATH_MAX];
    snprintf(recordings_path_tmp, sizeof(recordings_path_tmp),
             "%s/ViewerRecording", assets_root);
    recordings_path = recordings_path_tmp;
    permissions_check_passed = true;
#elif defined(__ANDROID__)
    char *assets_root = strdup("/sdcard/Glimpse");
    char log_filename_tmp[PATH_MAX];
    snprintf(log_filename_tmp, sizeof(log_filename_tmp),
             "%s/glimpse.log", assets_root);
    data->log_fp = fopen(log_filename_tmp, "w");
    char recordings_path_tmp[PATH_MAX];
    snprintf(recordings_path_tmp, sizeof(recordings_path_tmp),
             "%s/ViewerRecording", assets_root);
    recordings_path = recordings_path_tmp;
#else
    parse_args(data, argc, argv);

    const char *assets_root_env = getenv("GLIMPSE_ASSETS_ROOT");
    char *assets_root = strdup(assets_root_env ? assets_root_env : "");
    data->log_fp = stderr;
    recordings_path = getenv("GLIMPSE_RECORDING_PATH");
#endif

    if (!getenv("FAKENECT_PATH")) {
        char fakenect_path[PATH_MAX];
        snprintf(fakenect_path, sizeof(fakenect_path),
                 "%s/FakeRecording", assets_root);
        setenv("FAKENECT_PATH", fakenect_path, true);
    }

    data->log = gm_logger_new(logger_cb, data);
    gm_logger_set_abort_callback(data->log, logger_abort_cb, data);

    gm_debug(data->log, "Glimpse Viewer");

    gm_set_assets_root(data->log, assets_root);

    if (!recordings_path)
        recordings_path = gm_get_assets_root();
    glimpse_recordings_path = strdup(recordings_path);
    index_recordings(data);

    data->events_front = new std::vector<struct event>();
    data->events_back = new std::vector<struct event>();
    data->focal_point = glm::vec3(0.0, 0.0, 2.5);

#ifdef USE_GLFM
    init_winsys_glfm(data, display);
#else // USE_GLFW
    init_winsys_glfw(data);

    viewer_init(data);

    event_loop(data);

    viewer_destroy(data);

    return 0;
#endif
}

#ifdef __ANDROID__
extern "C" jint
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    android_jvm_singleton = vm;

    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_impossible_glimpse_GlimpseNativeActivity_OnPermissionsCheckResult(
    JNIEnv *env, jclass type, jboolean permission)
{
    /* Just wait for the next frame to check these */
    if (permission) {
        permissions_check_passed = true;
    } else
        permissions_check_failed = true;
}
#endif
