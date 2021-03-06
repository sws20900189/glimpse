
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

char *
ios_util_get_documents_path(void);

char *
ios_util_get_resources_path(void);

void
ios_log(const char *msg);

struct ios_av_session;

struct ios_av_session *
ios_util_av_session_new(struct gm_logger *log,
                        void (*configured_cb)(struct ios_av_session *session, void *user_data),
                        void (*depth_cb)(struct ios_av_session *session,
                                         struct gm_intrinsics *intrinsics,
                                         int stride,
                                         float *disparity,
                                         void *user_data),
                        void (*video_cb)(struct ios_av_session *session,
                                         struct gm_intrinsics *intrinsics,
                                         int stride,
                                         uint8_t *video,
                                         void *user_data),
                        void *user_data);

void
ios_util_session_configure(struct ios_av_session *_session);

void
ios_util_session_start(struct ios_av_session *session);

void
ios_util_session_stop(struct ios_av_session *_session);

void
ios_util_session_destroy(struct ios_av_session *session);

#ifdef __cplusplus
}
#endif
