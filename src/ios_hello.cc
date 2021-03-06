
// Example app that draws a triangle. The triangle can be moved via touch or keyboard arrow keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <epoxy/gl.h>

#include <imgui.h>
#include <imgui_internal.h> // For PushItemFlags(ImGuiItemFlags_Disabled)

#define GLFM_INCLUDE_NONE
#include "glfm.h"
#include <imgui_impl_glfm_gles3.h>

#define FILE_COMPAT_ANDROID_ACTIVITY glfmAndroidGetActivity()
#include "file_compat.h"

#include "glimpse_log.h"
#include "glimpse_assets.h"

#include "ios_utils.h"

typedef struct {
    GLuint program;
    GLuint vertexBuffer;

    double lastTouchX;
    double lastTouchY;

    float offsetX;
    float offsetY;
} ExampleApp;

static void onFrame(GLFMDisplay *display, double frameTime);
static void onSurfaceCreated(GLFMDisplay *display, int width, int height);
static void onSurfaceDestroyed(GLFMDisplay *display);
static bool onTouch(GLFMDisplay *display, int touch, GLFMTouchPhase phase, double x, double y);
static bool onKey(GLFMDisplay *display, GLFMKey keyCode, GLFMKeyAction action, int modifiers);

typedef struct {
    
    FILE *log_fp;
    struct gm_logger *log;
} Data;

static Data data;

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
        }

        free(msg);
    }
}


// Main entry point
void glfmMain(GLFMDisplay *display) {
    ExampleApp *app = (ExampleApp *)calloc(1, sizeof(ExampleApp));

    char *documents_dir = ios_util_get_documents_path();
    char log_filename[PATH_MAX];
    snprintf(log_filename, sizeof(log_filename), "%s/glimpse.log", documents_dir);
    data.log_fp = fopen(log_filename, "w");
    free(documents_dir);

    //data.log_fp = stdout;
    data.log = gm_logger_new(logger_cb, &data);
    gm_logger_set_abort_callback(data.log, logger_abort_cb, &data);

    char *resources_dir = ios_util_get_resources_path();
    gm_set_assets_root(data.log, resources_dir);
    free(resources_dir);

    gm_debug(data.log, "Glimpse Log Message");

    printf("glfmMain XXX\n");
    glfmSetDisplayConfig(display,
                         GLFMRenderingAPIOpenGLES3,
                         GLFMColorFormatRGBA8888,
                         GLFMDepthFormatNone,
                         GLFMStencilFormatNone,
                         GLFMMultisampleNone);
    glfmSetUserData(display, app);
    glfmSetSurfaceCreatedFunc(display, onSurfaceCreated);
    glfmSetSurfaceResizedFunc(display, onSurfaceCreated);
    glfmSetSurfaceDestroyedFunc(display, onSurfaceDestroyed);
    glfmSetMainLoopFunc(display, onFrame);
    glfmSetTouchFunc(display, onTouch);
    glfmSetKeyFunc(display, onKey);
}

static bool onTouch(GLFMDisplay *display, int touch, GLFMTouchPhase phase, double x, double y) {
    if (phase == GLFMTouchPhaseHover) {
        return false;
    }
    ExampleApp *app = (ExampleApp *)glfmGetUserData(display);
    if (phase != GLFMTouchPhaseBegan) {
        int width, height;
        glfmGetDisplaySize(display, &width, &height);
        app->offsetX += 2 * (x - app->lastTouchX) / width;
        app->offsetY -= 2 * (y - app->lastTouchY) / height;
    }
    app->lastTouchX = x;
    app->lastTouchY = y;
    return true;
}

static bool onKey(GLFMDisplay *display, GLFMKey keyCode, GLFMKeyAction action, int modifiers) {
    bool handled = false;
    if (action == GLFMKeyActionPressed) {
        ExampleApp *app = (ExampleApp *)glfmGetUserData(display);
        switch (keyCode) {
            case GLFMKeyLeft:
                app->offsetX -= 0.1f;
                handled = true;
                break;
            case GLFMKeyRight:
                app->offsetX += 0.1f;
                handled = true;
                break;
            case GLFMKeyUp:
                app->offsetY += 0.1f;
                handled = true;
                break;
            case GLFMKeyDown:
                app->offsetY -= 0.1f;
                handled = true;
                break;
            default:
                break;
        }
    }
    return handled;
}

static void onSurfaceCreated(GLFMDisplay *display, int width, int height) {
    glViewport(0, 0, width, height);

    GLFMRenderingAPI api = glfmGetRenderingAPI(display);
    printf("Hello from GLFM! Using OpenGL %s\n",
           api == GLFMRenderingAPIOpenGLES32 ? "ES 3.2" :
           api == GLFMRenderingAPIOpenGLES31 ? "ES 3.1" :
           api == GLFMRenderingAPIOpenGLES3 ? "ES 3.0" : "ES 2.0");
    printf("Flibble\n");
}

static void onSurfaceDestroyed(GLFMDisplay *display) {
    // When the surface is destroyed, all existing GL resources are no longer valid.
    ExampleApp *app = (ExampleApp *)glfmGetUserData(display);
    app->program = 0;
    app->vertexBuffer = 0;
}

static GLuint compileShader(GLenum type, const char *shaderName) {
    char fullPath[PATH_MAX];
    fc_resdir(fullPath, sizeof(fullPath));
    strncat(fullPath, shaderName, sizeof(fullPath) - strlen(fullPath) - 1);

    // Get shader string
    char *shaderString = NULL;
    FILE *shaderFile = fopen(fullPath, "rb");
    if (shaderFile) {
        fseek(shaderFile, 0, SEEK_END);
        long length = ftell(shaderFile);
        fseek(shaderFile, 0, SEEK_SET);

        shaderString = (char *)malloc(length + 1);
        if (shaderString) {
            fread(shaderString, length, 1, shaderFile);
            shaderString[length] = 0;
        }
        fclose(shaderFile);
    }
    if (!shaderString) {
        printf("Couldn't read file: %s\n", fullPath);
        return 0;
    }

    // Compile
    const char *constChaderString = shaderString;
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &constChaderString, NULL);
    glCompileShader(shader);
    free(shaderString);

    // Check compile status
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == 0) {
        printf("Couldn't compile shader: %s\n", shaderName);
        GLint logLength;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            GLchar *log = (GLchar *)malloc(logLength);
            glGetShaderInfoLog(shader, logLength, &logLength, log);
            if (log[0] != 0) {
                printf("Shader log: %s\n", log);
            }
            free(log);
        }
        glDeleteShader(shader);
        shader = 0;
    }
    return shader;
}

static void onFrame(GLFMDisplay *display, double frameTime) {
    ExampleApp *app = (ExampleApp *)glfmGetUserData(display);

    // Draw background
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    printf("onFrame\n");

    // Draw triangle
    if (app->program == 0) {
        GLuint vertShader = compileShader(GL_VERTEX_SHADER, "simple.vert");
        GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, "simple.frag");
        if (vertShader == 0 || fragShader == 0) {
            glfmSetMainLoopFunc(display, NULL);
            return;
        }
        app->program = glCreateProgram();

        glAttachShader(app->program, vertShader);
        glAttachShader(app->program, fragShader);

        glBindAttribLocation(app->program, 0, "a_position");
        glBindAttribLocation(app->program, 1, "a_color");

        glLinkProgram(app->program);

        glDeleteShader(vertShader);
        glDeleteShader(fragShader);
    }
    glUseProgram(app->program);
    if (app->vertexBuffer == 0) {
        glGenBuffers(1, &app->vertexBuffer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, app->vertexBuffer);
    const size_t stride = sizeof(GLfloat) * 6;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *)(sizeof(GLfloat) * 3));

    const GLfloat vertices[] = {
        // x,y,z, r,g,b
        app->offsetX + 0.0f, app->offsetY + 0.5f, 0.0f,  1.0f, 0.0f, 0.0f,
        app->offsetX - 0.5f, app->offsetY - 0.5f, 0.0f,  0.0f, 1.0f, 0.0f,
        app->offsetX + 0.5f, app->offsetY - 0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
