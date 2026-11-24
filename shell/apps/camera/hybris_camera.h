#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>

#if __has_include(<hybris/camera/camera_compatibility_layer.h>)
#include <hybris/camera/camera_compatibility_layer.h>
#include <hybris/camera/camera_compatibility_layer_capabilities.h>
#else
extern "C" {
typedef enum {
    BACK_FACING_CAMERA_TYPE = 0,
    FRONT_FACING_CAMERA_TYPE = 1
} CameraType;

typedef enum {
    PREVIEW_CALLBACK_DISABLED = 0,
    PREVIEW_CALLBACK_ENABLED = 1
} PreviewCallbackMode;

typedef enum {
    FLASH_MODE_OFF = 0,
    FLASH_MODE_AUTO = 1,
    FLASH_MODE_ON = 2,
    FLASH_MODE_TORCH = 3,
    FLASH_MODE_RED_EYE = 4
} FlashMode;

typedef enum {
    AUTO_FOCUS_MODE_OFF = 0,
    AUTO_FOCUS_MODE_CONTINUOUS_VIDEO = 1,
    AUTO_FOCUS_MODE_AUTO = 2,
    AUTO_FOCUS_MODE_MACRO = 3,
    AUTO_FOCUS_MODE_CONTINUOUS_PICTURE = 4,
    AUTO_FOCUS_MODE_INFINITY = 5
} AutoFocusMode;

typedef struct {
    int top;
    int left;
    int bottom;
    int right;
    int weight;
} FocusRegion, MeteringRegion;

struct CameraControl;

typedef void (*on_msg_error)(void *context);
typedef void (*on_msg_shutter)(void *context);
typedef void (*on_msg_focus)(void *context);
typedef void (*on_msg_zoom)(void *context, int32_t new_zoom_level);
typedef void (*on_data_raw_image)(void *data, uint32_t data_size, void *context);
typedef void (*on_data_compressed_image)(void *data, uint32_t data_size, void *context);
typedef void (*on_preview_texture_needs_update)(void *context);
typedef void (*on_preview_frame)(void *data, uint32_t data_size, void *context);
typedef void (*flash_mode_callback)(void *ctx, FlashMode mode);

struct CameraControlListener {
    on_msg_error on_msg_error_cb;
    on_msg_shutter on_msg_shutter_cb;
    on_msg_focus on_msg_focus_cb;
    on_msg_zoom on_msg_zoom_cb;
    on_data_raw_image on_data_raw_image_cb;
    on_data_compressed_image on_data_compressed_image_cb;
    on_preview_texture_needs_update on_preview_texture_needs_update_cb;
    void *context;
    on_preview_frame on_preview_frame_cb;
};
}
#endif

// Runtime dlopen wrapper around libcamera.so.1 (libhybris camera compatibility layer).
// NOTE: android_camera_get_preview_format is intentionally omitted because it is not
// exported by libcamera.so.1 on Xiaomi Pad 6 (pipa / Halium 13).
struct HybrisCameraApi {
    void *handle = nullptr;

    int (*get_number_of_devices)() = nullptr;
    int (*get_device_info)(int32_t, int *, int *) = nullptr;
    struct CameraControl *(*connect_by_id)(int32_t, struct CameraControlListener *) = nullptr;
    void (*disconnect)(struct CameraControl *) = nullptr;
    void (*delete_ctrl)(struct CameraControl *) = nullptr;
    void (*set_preview_texture)(struct CameraControl *, int) = nullptr;
    void (*update_preview_texture)(struct CameraControl *) = nullptr;
    void (*set_preview_size)(struct CameraControl *, int, int) = nullptr;
    void (*set_picture_size)(struct CameraControl *, int, int) = nullptr;
    void (*set_jpeg_quality)(struct CameraControl *, int) = nullptr;
    void (*set_rotation)(struct CameraControl *, int) = nullptr;
    void (*set_zoom)(struct CameraControl *, int32_t) = nullptr;
    void (*get_max_zoom)(struct CameraControl *, int *) = nullptr;
    void (*set_flash_mode)(struct CameraControl *, FlashMode) = nullptr;
    void (*enumerate_supported_flash_modes)(struct CameraControl *, flash_mode_callback, void *) = nullptr;
    void (*set_focus_region)(struct CameraControl *, FocusRegion *) = nullptr;
    void (*set_auto_focus_mode)(struct CameraControl *, AutoFocusMode) = nullptr;
    void (*start_autofocus)(struct CameraControl *) = nullptr;
    int (*set_preview_callback_mode)(struct CameraControl *, PreviewCallbackMode) = nullptr;
    void (*start_preview)(struct CameraControl *) = nullptr;
    void (*stop_preview)(struct CameraControl *) = nullptr;
    void (*take_snapshot)(struct CameraControl *) = nullptr;

    ~HybrisCameraApi() { unload(); }

    template <typename Fn>
    bool resolve(Fn &out, const char *name, bool required)
    {
        out = reinterpret_cast<Fn>(dlsym(handle, name));
        if (required && !out) {
            return false;
        }
        return true;
    }

    bool load(std::string *errOut = nullptr)
    {
        if (handle)
            return true;
        handle = dlopen("libcamera.so.1", RTLD_LAZY);
        if (!handle)
            handle = dlopen("libcamera.so", RTLD_LAZY);
        if (!handle) {
            if (errOut) {
                const char *e = dlerror();
                *errOut = e ? std::string("dlopen libcamera.so.1 failed: ") + e
                            : std::string("dlopen libcamera.so.1 failed");
            }
            return false;
        }

        bool ok = true;
        ok &= resolve(get_number_of_devices, "android_camera_get_number_of_devices", true);
        ok &= resolve(get_device_info, "android_camera_get_device_info", true);
        ok &= resolve(connect_by_id, "android_camera_connect_by_id", true);
        ok &= resolve(disconnect, "android_camera_disconnect", true);
        ok &= resolve(delete_ctrl, "android_camera_delete", true);
        ok &= resolve(set_preview_texture, "android_camera_set_preview_texture", true);
        ok &= resolve(set_preview_size, "android_camera_set_preview_size", true);
        ok &= resolve(set_picture_size, "android_camera_set_picture_size", true);
        ok &= resolve(set_jpeg_quality, "android_camera_set_jpeg_quality", false);
        ok &= resolve(set_rotation, "android_camera_set_rotation", false);
        ok &= resolve(set_zoom, "android_camera_set_zoom", false);
        ok &= resolve(get_max_zoom, "android_camera_get_max_zoom", false);
        ok &= resolve(set_flash_mode, "android_camera_set_flash_mode", false);
        ok &= resolve(enumerate_supported_flash_modes, "android_camera_enumerate_supported_flash_modes", false);
        ok &= resolve(set_focus_region, "android_camera_set_focus_region", false);
        ok &= resolve(set_auto_focus_mode, "android_camera_set_auto_focus_mode", false);
        ok &= resolve(start_autofocus, "android_camera_start_autofocus", false);
        ok &= resolve(set_preview_callback_mode, "android_camera_set_preview_callback_mode", true);
        ok &= resolve(start_preview, "android_camera_start_preview", true);
        ok &= resolve(stop_preview, "android_camera_stop_preview", true);
        ok &= resolve(take_snapshot, "android_camera_take_snapshot", true);
        resolve(update_preview_texture, "android_camera_update_preview_texture", false);

        if (!ok) {
            if (errOut)
                *errOut = "Required symbols missing in libcamera.so.1";
            unload();
            return false;
        }
        return true;
    }

    void unload()
    {
        if (handle) {
            dlclose(handle);
            handle = nullptr;
        }
    }
};

// Offscreen 16x16 EGL Pbuffer + OpenGL ES 2.0 context bound on the camera thread.
// Android Camera HAL requires a valid GL texture passed to android_camera_set_preview_texture
// before start_preview will deliver NV21 frames or snapshots.
class OffscreenEglContext {
public:
    ~OffscreenEglContext() { shutdown(); }

    bool init(std::string *errOut = nullptr)
    {
        if (m_ready)
            return true;

        // Ensure libhybris EGL selects its null/Android passthrough ws even when launched
        // under naomi-shell (which exports EGL_PLATFORM=wayland for normal Wayland clients).
        setenv("HYBRIS_EGLPLATFORM", "null", 1);
        setenv("EGL_PLATFORM", "null", 1);

        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_display == EGL_NO_DISPLAY) {
            if (errOut)
                *errOut = "eglGetDisplay(EGL_DEFAULT_DISPLAY) failed";
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(m_display, &major, &minor)) {
            if (errOut)
                *errOut = "eglInitialize failed";
            shutdown();
            return false;
        }

        eglBindAPI(EGL_OPENGL_ES_API);

        const EGLint cfgAttribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLConfig config = nullptr;
        EGLint numConfigs = 0;
        if (!eglChooseConfig(m_display, cfgAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
            // Relax alpha requirement if vendor config omits alpha on pbuffers.
            const EGLint fallbackAttribs[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_NONE
            };
            if (!eglChooseConfig(m_display, fallbackAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
                if (errOut)
                    *errOut = "eglChooseConfig(PBUFFER, ES2) failed";
                shutdown();
                return false;
            }
        }

        const EGLint pbufAttribs[] = {
            EGL_WIDTH, 16,
            EGL_HEIGHT, 16,
            EGL_NONE
        };
        m_surface = eglCreatePbufferSurface(m_display, config, pbufAttribs);
        if (m_surface == EGL_NO_SURFACE) {
            if (errOut)
                *errOut = "eglCreatePbufferSurface(16x16) failed";
            shutdown();
            return false;
        }

        const EGLint ctxAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, ctxAttribs);
        if (m_context == EGL_NO_CONTEXT) {
            if (errOut)
                *errOut = "eglCreateContext(ES2) failed";
            shutdown();
            return false;
        }

        if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
            if (errOut)
                *errOut = "eglMakeCurrent failed";
            shutdown();
            return false;
        }

        glGenTextures(1, &m_texture);
        if (m_texture == 0) {
            if (errOut)
                *errOut = "glGenTextures returned 0";
            shutdown();
            return false;
        }

#ifdef GL_TEXTURE_EXTERNAL_OES
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#endif

        m_ready = true;
        return true;
    }

    void shutdown()
    {
        if (m_display != EGL_NO_DISPLAY) {
            if (m_texture != 0 && m_context != EGL_NO_CONTEXT) {
                eglMakeCurrent(m_display, m_surface, m_surface, m_context);
                glDeleteTextures(1, &m_texture);
                m_texture = 0;
            }
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_context != EGL_NO_CONTEXT) {
                eglDestroyContext(m_display, m_context);
                m_context = EGL_NO_CONTEXT;
            }
            if (m_surface != EGL_NO_SURFACE) {
                eglDestroySurface(m_display, m_surface);
                m_surface = EGL_NO_SURFACE;
            }
            eglTerminate(m_display);
            m_display = EGL_NO_DISPLAY;
        }
        m_texture = 0;
        m_ready = false;
    }

    bool isReady() const { return m_ready; }
    GLuint textureId() const { return m_texture; }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;
    GLuint m_texture = 0;
    bool m_ready = false;
};
