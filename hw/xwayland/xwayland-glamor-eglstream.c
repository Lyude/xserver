/*
 * Copyright © 2017 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Lyude Paul <lyude@redhat.com>
 *
 */

#include "xwayland.h"
#include "xwayland-glamor.h"

#include "eglstream-client-protocol.h"
#include "eglstream-controller-client-protocol.h"

#define MESA_EGL_NO_X11_HEADERS
#include <glamor_egl.h>
#include <glamor.h>
#include <glamor_priv.h>
#include <glamor_transform.h>
#include <glamor_transfer.h>

#include <xf86drm.h>

#include <epoxy/egl.h>

/* CURRENT TODO:
 * - Probe for all of the following GL extensions as well while probing for
 *   EGLDevices:
 *   - GL_ARB_vertex_array_object
 *   - (maybe?) GL_OES_EGL_image
 */

/* FIXME: Remove all of this when we're done */
static void GLAPIENTRY
lyude_gl_debug_output_callback(GLenum source,
                               GLenum type,
                               GLuint id,
                               GLenum severity,
                               GLsizei length,
                               const GLchar *message,
                               const void *userParam)
{
    LogMessageVerb(X_ERROR, 0, "eglstream: GL error: %*s\n",
                   length, message);
}

static void EGLAPIENTRY
lyude_egl_debug_output_callback(EGLenum error,
                                const char *command,
                                EGLint message_type,
                                EGLLabelKHR thread_label,
                                EGLLabelKHR object_label,
                                const char *message)
{
    LogMessageVerb(X_ERROR, 0,
                   "eglstream: EGL error from %s (0x%x): %s",
                   command, error, message ?: "<null>");
}

static void
lyude_enable_debug(void)
{
    EGLint ret;

    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR,
                          GL_DONT_CARE, 0, NULL, GL_TRUE);
    glDebugMessageCallback(lyude_gl_debug_output_callback, NULL);

    if (epoxy_has_gl_extension("GL_KHR_debug"))
        glEnable(GL_DEBUG_OUTPUT);

    ret = eglDebugMessageControlKHR(
        lyude_egl_debug_output_callback,
        (long[]) {
            EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
            EGL_DEBUG_MSG_ERROR_KHR,    EGL_TRUE,
            EGL_DEBUG_MSG_WARN_KHR,     EGL_TRUE,
            EGL_DEBUG_MSG_INFO_KHR,     EGL_TRUE,
            EGL_NONE
        });
    if (ret != EGL_SUCCESS) {
        LogMessageVerb(X_ERROR, 0,
                       "eglstream: Can't setup EGL debugging code 0x%x\n",
                       eglGetError());
    }
}

static GLint
lyude_gl_get_int(GLint attribute)
{
    int ret;

    glGetIntegerv(attribute, &ret);
    return ret;
}

/***************************************************************************/
/* XXX: Real code starts here */
/***************************************************************************/

struct xwl_eglstream_private {
    struct wl_eglstream_display *display;
    struct wl_eglstream_controller *controller;
    uint32_t display_caps;

    EGLConfig config;

    Bool have_egl_damage;

    GLint blit_prog;

    GLuint blit_vbo;
    GLuint blit_vao;
    GLuint blit_sampler;

    GLint  blit_position_loc;
    GLint  blit_texcoord_loc;
};

struct xwl_pixmap {
    struct wl_buffer *buffer;

    GLuint texture;
    EGLStreamKHR stream;
    EGLSurface surface;
};

/* TODO: Use these during the initial probing process when setting up EGL
 * display contexts
 */
static inline struct xwl_eglstream_private *
xwl_eglstream_get(struct xwl_screen *xwl_screen)
{
    return xwl_screen->egl_backend.priv;
}

static GLenum gl_iformat_from_config(EGLDisplay egl_display, EGLConfig config)
{
    int buffer_size;

    eglGetConfigAttrib(egl_display, config, EGL_BUFFER_SIZE, &buffer_size);
    switch (buffer_size) {
    default:
        ErrorF("Unexpected buffer size %d\n", buffer_size);
    case 32: return GL_RGBA8;
    case 24: return GL_RGB8;
    case 16: return GL_RGB565;
    }
}

/* TODO: turn this into a shared helper, the wheel's been invented 3 times
 * now */
static GLint
xwl_glamor_eglstream_compile_glsl_prog(GLenum type, const char *source)
{
    GLint ok;
    GLint prog;

    prog = glCreateShader(type);
    glShaderSource(prog, 1, (const GLchar **) &source, NULL);
    glCompileShader(prog);
    glGetShaderiv(prog, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar *info;
        GLint size;

        glGetShaderiv(prog, GL_INFO_LOG_LENGTH, &size);
        info = malloc(size);
        if (info) {
            glGetShaderInfoLog(prog, size, NULL, info);
            ErrorF("Failed to compile %s: %s\n",
                   type == GL_FRAGMENT_SHADER ? "FS" : "VS", info);
            ErrorF("Program source:\n%s", source);
            free(info);
        }
        else
            ErrorF("Failed to get shader compilation info.\n");
        FatalError("GLSL compile failure\n");
    }

    return prog;
}

static GLuint
xwl_glamor_eglstream_build_glsl_prog(GLuint vs, GLuint fs)
{
    GLint ok;
    GLuint prog;

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);

    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar *info;
        GLint size;

        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
        info = malloc(size);

        glGetProgramInfoLog(prog, size, NULL, info);
        ErrorF("Failed to link: %s\n", info);
        FatalError("GLSL link failure\n");
    }

    return prog;
}

static void
xwl_glamor_eglstream_cleanup(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);

    if (xwl_eglstream->display)
        wl_eglstream_display_destroy(xwl_eglstream->display);
    if (xwl_eglstream->controller)
        wl_eglstream_controller_destroy(xwl_eglstream->controller);
    if (xwl_eglstream->blit_prog)
        glDeleteProgram(xwl_eglstream->blit_prog);

    free(xwl_eglstream);
    xwl_screen->egl_backend.priv = NULL;
}

static PixmapPtr
xwl_glamor_eglstream_create_pixmap_egl(ScreenPtr screen,
                                       int width, int height, int depth,
                                       unsigned int hint)
{
    PixmapPtr pixmap = NULL;
    struct xwl_pixmap *xwl_pixmap;

    xwl_pixmap = calloc(sizeof(*xwl_pixmap), 1);
    if (!xwl_pixmap)
        goto error;

    pixmap = glamor_create_pixmap(screen, width, height, depth, hint);
    if (!pixmap)
        goto error;

    xwl_pixmap->texture = glamor_get_pixmap_texture(pixmap);
    xwl_pixmap_set_private(pixmap, xwl_pixmap);

    return pixmap;
error:
    ErrorF("Failed to create glamor pixmap\n");
    if (pixmap)
        glamor_destroy_pixmap(pixmap);
    if (xwl_pixmap)
        free(xwl_pixmap);

    return NULL;
}

static PixmapPtr
xwl_glamor_eglstream_create_pixmap(ScreenPtr screen,
                                   int width, int height, int depth,
                                   unsigned int hint)
{
    if (width > 0 && height > 0 && depth >= 15 &&
        (!hint ||
         hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP ||
         hint == CREATE_PIXMAP_USAGE_SHARED)) {
        return xwl_glamor_eglstream_create_pixmap_egl(screen, width, height,
                                                      depth, hint);
    }

    return glamor_create_pixmap(screen, width, height, depth, hint);
}

static Bool
xwl_glamor_eglstream_destroy_pixmap(PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap && pixmap->refcnt == 1) {
        if (xwl_pixmap->buffer) {
            wl_buffer_destroy(xwl_pixmap->buffer);
            eglDestroyStreamKHR(xwl_screen->egl_display, xwl_pixmap->stream);
            eglDestroySurface(xwl_screen->egl_display, xwl_pixmap->surface);
        }

        /*glDeleteTextures(1, &xwl_pixmap->texture);*/
        free(xwl_pixmap);
    }

    return glamor_destroy_pixmap(pixmap);
}

static struct wl_buffer *
xwl_glamor_eglstream_get_wl_buffer_for_pixmap(PixmapPtr pixmap,
                                              WindowPtr window)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);
    struct xwl_window *xwl_window = xwl_window_get(window);
    struct xwl_eglstream_private *xwl_eglstream = xwl_eglstream_get(xwl_screen);
    struct wl_buffer *buffer;
    struct wl_array stream_attribs;
    int *attrib_list;
    int fd = EGL_NO_FILE_DESCRIPTOR_KHR;

    if (xwl_pixmap->buffer)
        return xwl_pixmap->buffer;

    if (lastGLContext != xwl_screen->glamor_ctx) {
        lastGLContext = xwl_screen->glamor_ctx;
        xwl_glamor_egl_make_current(xwl_screen->glamor_ctx);
    }

    xwl_pixmap->stream = eglCreateStreamKHR(xwl_screen->egl_display, NULL);
    if (xwl_pixmap->stream == EGL_NO_STREAM_KHR) {
        ErrorF("Failed to create EGLStreamKHR object\n");
        goto error;
    }

    fd = eglGetStreamFileDescriptorKHR(xwl_screen->egl_display,
                                       xwl_pixmap->stream);
    if (fd == EGL_NO_FILE_DESCRIPTOR_KHR) {
        ErrorF("Failed to get file descriptor for egl stream\n");
        return NULL;
    }

    wl_array_init(&stream_attribs);
    buffer = wl_eglstream_display_create_stream(
        xwl_eglstream->display,
        pixmap->drawable.width, pixmap->drawable.height, fd,
        WL_EGLSTREAM_HANDLE_TYPE_FD, &stream_attribs);
    if (!buffer) {
        ErrorF("Failed to create wl_buffer from EGLStream\n");
        goto error;
    }

    wl_eglstream_controller_attach_eglstream_consumer(
        xwl_eglstream->controller, xwl_window->surface, buffer);
    wl_display_roundtrip(xwl_screen->display);

    attrib_list = (int[]) {
        EGL_WIDTH, pixmap->drawable.width,
        EGL_HEIGHT, pixmap->drawable.height,
        EGL_NONE
    };
    xwl_pixmap->surface = eglCreateStreamProducerSurfaceKHR(
        xwl_screen->egl_display, xwl_eglstream->config, xwl_pixmap->stream,
        attrib_list);
    if (xwl_pixmap->surface == EGL_NO_SURFACE) {
        ErrorF("Failed to create EGLStream's producer surface\n");
        goto error;
    }

    xwl_pixmap->buffer = buffer;
    return buffer;

error:
    if (fd != EGL_NO_FILE_DESCRIPTOR_KHR)
        close(fd);
    if (xwl_pixmap->stream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(xwl_screen->egl_display, xwl_pixmap->stream);
        xwl_pixmap->stream = EGL_NO_STREAM_KHR;
    }
    if (xwl_pixmap->surface != EGL_NO_SURFACE) {
        eglDestroySurface(xwl_screen->egl_display, xwl_pixmap->surface);
        xwl_pixmap->surface = EGL_NO_SURFACE;
    }

    return NULL;
}

static void
xwl_glamor_eglstream_post_damage(struct xwl_screen *xwl_screen,
                                 struct xwl_window *xwl_window,
                                 PixmapPtr pixmap, RegionPtr region)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);
    BoxPtr box = RegionExtents(region);
    EGLint egl_damage[] = {
        box->x1,           box->y1,
        box->x2 - box->x1, box->y2 - box->y1
    };
    GLint saved_vao;

    eglMakeCurrent(eglGetCurrentDisplay(),
                   xwl_pixmap->surface,
                   xwl_pixmap->surface,
                   eglGetCurrentContext());

    glUseProgram(xwl_eglstream->blit_prog);
    glViewport(0, 0, pixmap->drawable.width, pixmap->drawable.height);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    glBindVertexArray(xwl_eglstream->blit_vao);

    /* Blit rendered image into EGLStream surface */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, xwl_pixmap->texture);

    /*glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,*/
                           /*GL_TEXTURE_2D, xwl_pixmap->texture, 0);*/

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* Flush */
    if (xwl_eglstream->have_egl_damage)
        eglSwapBuffersWithDamageKHR(xwl_screen->egl_display,
                                    xwl_pixmap->surface, egl_damage, 1);
    else
        eglSwapBuffers(xwl_screen->egl_display, xwl_pixmap->surface);

    /*[> Restore GL context <]*/
    eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE,
                   eglGetCurrentContext());

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(saved_vao);
}

static void
xwl_eglstream_display_handle_caps(void *data,
                                  struct wl_eglstream_display *disp,
                                  int32_t caps)
{
    xwl_eglstream_get(data)->display_caps = caps;
}

static void
xwl_eglstream_display_handle_swapinterval_override(void *data,
                                                   struct wl_eglstream_display *disp,
                                                   int32_t swapinterval,
                                                   struct wl_buffer *stream)
{
    /* TODO */
}

const struct wl_eglstream_display_listener eglstream_display_listener = {
    .caps = xwl_eglstream_display_handle_caps,
    .swapinterval_override = xwl_eglstream_display_handle_swapinterval_override,
};

static void
xwl_glamor_eglstream_init_wl_registry(struct xwl_screen *xwl_screen,
                                      struct wl_registry *wl_registry,
                                      const char *name,
                                      uint32_t id, uint32_t version)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);

    if (strcmp(name, "wl_eglstream_display") == 0) {
        xwl_eglstream->display = wl_registry_bind(
            wl_registry, id, &wl_eglstream_display_interface, version);

        wl_eglstream_display_add_listener(xwl_eglstream->display,
                                          &eglstream_display_listener,
                                          xwl_screen);
    } else if (strcmp(name, "wl_eglstream_controller") == 0) {
        xwl_eglstream->controller = wl_registry_bind(
            wl_registry, id, &wl_eglstream_controller_interface, version);
    }
}

static EGLConfig
xwl_glamor_eglstream_find_best_config(struct xwl_screen *xwl_screen)
{
    const EGLint config_requirements[] = {
        EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 5,
        EGL_GREEN_SIZE, 6,
        EGL_BLUE_SIZE, 5,
        /*EGL_CONFIG_ID, 0x32,*/
        EGL_NONE,
    };
    EGLConfig *configs;
    EGLConfig best_config;
    int i, num_config;
    int score = 0, high_score = 0;
    int red_size, green_size, blue_size, alpha_size;
    Bool ret;

    /* Query the number of compatible EGLConfigs */
    ret = eglChooseConfig(xwl_screen->egl_display, config_requirements,
                          NULL, 0, &num_config);
    if (!ret || !num_config)
        return FALSE;

    configs = malloc(sizeof(EGLConfig) * num_config);
    if (!configs)
        return FALSE;

    eglChooseConfig(xwl_screen->egl_display, config_requirements,
                    configs, num_config, &num_config);

    for (i = 0; i < num_config; i++) {
        eglGetConfigAttrib(xwl_screen->egl_display, configs[i],
                           EGL_RED_SIZE, &red_size);
        eglGetConfigAttrib(xwl_screen->egl_display, configs[i],
                           EGL_GREEN_SIZE, &green_size);
        eglGetConfigAttrib(xwl_screen->egl_display, configs[i],
                           EGL_BLUE_SIZE, &blue_size);
        eglGetConfigAttrib(xwl_screen->egl_display, configs[i],
                           EGL_ALPHA_SIZE, &alpha_size);

        score  = alpha_size == 8;
        score += red_size == 8;
        score += green_size == 8;
        score += blue_size == 8;

        if (score > high_score) {
            high_score = score;
            best_config = configs[i];
        }
    }

    free(configs);

    eglGetConfigAttrib(xwl_screen->egl_display, best_config,
                       EGL_RED_SIZE, &red_size);
    eglGetConfigAttrib(xwl_screen->egl_display, best_config,
                       EGL_GREEN_SIZE, &green_size);
    eglGetConfigAttrib(xwl_screen->egl_display, best_config,
                       EGL_BLUE_SIZE, &blue_size);
    eglGetConfigAttrib(xwl_screen->egl_display, best_config,
                       EGL_ALPHA_SIZE, &alpha_size);

    xwl_screen->formats = XWL_FORMAT_RGB565;
    if (red_size == 8 && green_size == 8 && blue_size == 8) {
        if (alpha_size == 8)
            xwl_screen->formats |= XWL_FORMAT_ARGB8888;

        xwl_screen->formats |= XWL_FORMAT_XRGB8888;
    }

    return best_config;
}

static inline Bool
xwl_glamor_eglstream_init_shaders(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    GLint fs, vs;

    const char *blit_vs_src =
        "attribute vec2 texcoord;\n"
        "attribute vec2 position;\n"
        "varying vec2 t;\n"
        "void main() {\n"
        "   t = texcoord;\n"
        "   gl_Position = vec4(position, 0, 1);\n"
        "}";

    const char *blit_fs_src =
        GLAMOR_DEFAULT_PRECISION
        "varying vec2 t;\n"
        "uniform sampler2D s;\n"
        "void main() {\n"
        "   gl_FragColor = texture2D(s, t);\n"
        "}\n";

    static const float position[] = {
        /* position */
        -1, -1,
         1, -1,
         1,  1,
        -1,  1,
        /* texcoord */
         0,  1,
         1,  1,
         1,  0,
         0,  0,
    };

    vs = xwl_glamor_eglstream_compile_glsl_prog(GL_VERTEX_SHADER,
                                                blit_vs_src);
    fs = xwl_glamor_eglstream_compile_glsl_prog(GL_FRAGMENT_SHADER,
                                                blit_fs_src);

    xwl_eglstream->blit_prog = xwl_glamor_eglstream_build_glsl_prog(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    xwl_eglstream->blit_position_loc =
        glGetAttribLocation(xwl_eglstream->blit_prog, "position");
    xwl_eglstream->blit_texcoord_loc =
        glGetAttribLocation(xwl_eglstream->blit_prog, "texcoord");

    /* Create the blitter's vao */
    glGenVertexArrays(1, &xwl_eglstream->blit_vao);
    glBindVertexArray(xwl_eglstream->blit_vao);

    /* Set the data for both position and texcoord in the vbo */
    glGenBuffers(1, &xwl_eglstream->blit_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, xwl_eglstream->blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

    /* Define each shader attribute's data location in our vbo */
    glVertexAttribPointer(xwl_eglstream->blit_position_loc,
                          2, GL_FLOAT, TRUE, 0, NULL);
    glVertexAttribPointer(xwl_eglstream->blit_texcoord_loc,
                          2, GL_FLOAT, TRUE, 0, (void*)(sizeof(float) * 8));

    glEnableVertexAttribArray(xwl_eglstream->blit_position_loc);
    glEnableVertexAttribArray(xwl_eglstream->blit_texcoord_loc);

    glUseProgram(xwl_eglstream->blit_prog);

    return TRUE;
}

static Bool
xwl_glamor_eglstream_init_egl(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    EGLConfig config;
    EGLint config_attribs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,

        /* Use 4.2 so we can get texture views */
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 2,
        EGL_NONE
    };
    const char *required_gl_exts[] = {
        "GL_ARB_texture_view",
        "GL_ARB_texture_storage",
    };
    int i;

    xwl_screen->egl_display = glamor_egl_get_display(EGL_PLATFORM_DEVICE_EXT,
                                                     xwl_screen->egl_device);
    if (!xwl_screen->egl_display)
        goto error;

    if (!eglInitialize(xwl_screen->egl_display, NULL, NULL)) {
        xwl_screen->egl_display = NULL;
        goto error;
    }

    config = xwl_glamor_eglstream_find_best_config(xwl_screen);
    if (!config) {
        ErrorF("No acceptable EGL configs found\n");
        goto error;
    }
    xwl_eglstream->config = config;

    eglBindAPI(EGL_OPENGL_API);
    xwl_screen->egl_context = eglCreateContext(
        xwl_screen->egl_display, config, EGL_NO_CONTEXT, config_attribs);
    if (xwl_screen->egl_context == EGL_NO_CONTEXT) {
        ErrorF("Failed to create main EGL context: 0x%x\n",
               eglGetError());
        goto error;
    }

    if (!eglMakeCurrent(xwl_screen->egl_display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        xwl_screen->egl_context)) {
        ErrorF("Failed to make EGL context current\n");
        goto error;
    }
    lyude_enable_debug(); /* FIXME */

    for (i = 0; i < ARRAY_SIZE(required_gl_exts); i++) {
        if (!epoxy_has_gl_extension(required_gl_exts[i])) {
            ErrorF("OpenGL ES driver doesn't support %s\n",
                   required_gl_exts[i]);
            goto error;
        }
    }

    xwl_eglstream->have_egl_damage =
        epoxy_has_egl_extension(xwl_screen->egl_display,
                                "EGL_KHR_swap_buffers_with_damage");
    if (!xwl_eglstream->have_egl_damage)
        ErrorF("Driver lacks EGL_KHR_swap_buffers_with_damage, performance "
               "will be affected\n");

    /* FIXME */
    /* apitrace doesn't currently support eglSwapBuffersWithDamage, disable it
     * for now
     */
    xwl_eglstream->have_egl_damage = FALSE;

    if (!xwl_glamor_eglstream_init_shaders(xwl_screen))
        goto error;

    return TRUE;
error:
    xwl_glamor_eglstream_cleanup(xwl_screen);
    return FALSE;
}

static Bool
xwl_glamor_eglstream_init_screen(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);

    if (!xwl_eglstream->controller) {
        ErrorF("No eglstream controller was exposed in the wayland registry. "
               "This means your version of nvidia's EGL wayland libraries "
               "are too old, as we require support for this.\n");
        xwl_glamor_eglstream_cleanup(xwl_screen);
        return FALSE;
    }

    xwl_screen->screen->CreatePixmap = xwl_glamor_eglstream_create_pixmap;
    xwl_screen->screen->DestroyPixmap = xwl_glamor_eglstream_destroy_pixmap;

    return TRUE;
}

static Bool
xwl_glamor_eglstream_get_device(struct xwl_screen *xwl_screen)
{
    void **devices = NULL;
    const char *exts[] = {
        "EGL_KHR_stream",
        "EGL_KHR_stream_producer_eglsurface",
    };
    int num_devices, i;

    if (xwl_screen->egl_device) {
        return xwl_glamor_egl_device_has_egl_extensions(
            xwl_screen->egl_device, exts, ARRAY_SIZE(exts));
    }

    /* No device specified by the user, so find one ourselves */
    devices = xwl_glamor_egl_get_devices(&num_devices);
    if (!devices)
        goto out;

    for (i = 0; i < num_devices; i++) {
        if (xwl_glamor_egl_device_has_egl_extensions(devices[i], exts,
                                                     ARRAY_SIZE(exts))) {
            xwl_screen->egl_device = devices[i];
            break;
        }
    }

out:
    if (devices)
        free(devices);

    if (!xwl_screen->egl_device) {
        ErrorF("glamor: No eglstream capable devices found\n");
        return FALSE;
    }

    return TRUE;
}

Bool
xwl_glamor_init_eglstream(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream;

    if (!xwl_glamor_eglstream_get_device(xwl_screen))
        return FALSE;

    xwl_eglstream = calloc(sizeof(*xwl_eglstream), 1);
    if (!xwl_eglstream) {
        ErrorF("Failed to allocate memory required to init eglstream support\n");
        return FALSE;
    }

    xwl_screen->egl_backend.priv = xwl_eglstream;
    xwl_screen->egl_backend.init_egl = xwl_glamor_eglstream_init_egl;
    xwl_screen->egl_backend.init_wl_registry = xwl_glamor_eglstream_init_wl_registry;
    xwl_screen->egl_backend.init_screen = xwl_glamor_eglstream_init_screen;
    xwl_screen->egl_backend.get_wl_buffer_for_pixmap = xwl_glamor_eglstream_get_wl_buffer_for_pixmap;
    xwl_screen->egl_backend.post_damage = xwl_glamor_eglstream_post_damage;

    ErrorF("glamor: Using nvidia's eglstream interface, like the lame kids.\n");
    return TRUE;
}
