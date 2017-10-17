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

#include "wayland-eglstream-client-protocol.h"
#include "wayland-eglstream-controller-client-protocol.h"

#define MESA_EGL_NO_X11_HEADERS
#include <glamor_egl.h>
#include <glamor.h>
#include <glamor_transform.h>
#include <glamor_transfer.h>

#include <xf86drm.h>

#include <epoxy/egl.h>

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
struct xwl_eglstream_pending_stream {
    PixmapPtr pixmap;
    WindowPtr window;

    struct xwl_pixmap *xwl_pixmap;
    struct wl_callback *cb;

    Bool pixmap_was_changed;

    struct xorg_list link;
};

struct xwl_eglstream_private {
    struct wl_eglstream_display *display;
    struct wl_eglstream_controller *controller;
    uint32_t display_caps;

    EGLConfig config;

    struct {
        EGLDisplay display;
        EGLSurface read, draw;
        EGLContext ctx;
    } saved;
    SetWindowPixmapProcPtr SetWindowPixmap;

    struct xorg_list pending_streams;

    Bool have_egl_damage;

    GLint blit_prog;
    GLuint blit_vao;
    GLuint blit_vbo;
};

struct xwl_pixmap {
    struct wl_buffer *buffer;
    struct xwl_screen *xwl_screen;

    /* The stream and associated resources have their own lifetime seperate
     * from the pixmap's */
    int refcount;

    EGLStreamKHR stream;
    int stream_fd;
    EGLSurface surface;
};

static DevPrivateKeyRec xwl_eglstream_private_key;
static DevPrivateKeyRec xwl_eglstream_window_private_key;

static inline struct xwl_eglstream_private *
xwl_eglstream_get(struct xwl_screen *xwl_screen)
{
    return dixLookupPrivate(&xwl_screen->screen->devPrivates,
                            &xwl_eglstream_private_key);
}

static inline struct xwl_eglstream_pending_stream *
xwl_eglstream_window_get_pending(WindowPtr window)
{
    return dixLookupPrivate(&window->devPrivates,
                            &xwl_eglstream_window_private_key);
}

static inline void
xwl_eglstream_window_set_pending(WindowPtr window,
                                 struct xwl_eglstream_pending_stream *stream)
{
    dixSetPrivate(&window->devPrivates,
                  &xwl_eglstream_window_private_key, stream);
}

/* TODO: we don't need to be this careful with the context. Fix this before
 * finalizing */
static inline void
xwl_eglstream_make_current(struct xwl_screen *xwl_screen,
                           EGLSurface surface)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);

    /* Don't replace the saved state if we're already currently in our own */
    if (xwl_eglstream->saved.display == EGL_NO_DISPLAY) {
        xwl_eglstream->saved.display = eglGetCurrentDisplay();
        xwl_eglstream->saved.ctx = eglGetCurrentContext();
        xwl_eglstream->saved.read = eglGetCurrentSurface(EGL_READ);
        xwl_eglstream->saved.draw = eglGetCurrentSurface(EGL_DRAW);
    }

    eglMakeCurrent(xwl_screen->egl_display,
                   surface, surface,
                   xwl_screen->egl_context);
}

static inline void
xwl_eglstream_restore_current(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);

    eglMakeCurrent(xwl_eglstream->saved.display,
                   xwl_eglstream->saved.read, xwl_eglstream->saved.draw,
                   xwl_eglstream->saved.ctx);
    xwl_eglstream->saved.display = EGL_NO_DISPLAY;
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
    if (xwl_eglstream->blit_prog) {
        glDeleteProgram(xwl_eglstream->blit_prog);
        glDeleteBuffers(1, &xwl_eglstream->blit_vbo);
    }

    free(xwl_eglstream);
}

static void
xwl_eglstream_unref_pixmap_stream(struct xwl_pixmap *xwl_pixmap)
{
    struct xwl_screen *xwl_screen = xwl_pixmap->xwl_screen;

    if (--xwl_pixmap->refcount >= 1)
        return;

    if (xwl_pixmap->surface)
        eglDestroySurface(xwl_screen->egl_display, xwl_pixmap->surface);

    close(xwl_pixmap->stream_fd);
    eglDestroyStreamKHR(xwl_screen->egl_display, xwl_pixmap->stream);

    wl_buffer_destroy(xwl_pixmap->buffer);
    free(xwl_pixmap);
}

static Bool
xwl_glamor_eglstream_destroy_pixmap(PixmapPtr pixmap)
{
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap && pixmap->refcnt == 1)
        xwl_eglstream_unref_pixmap_stream(xwl_pixmap);

    return glamor_destroy_pixmap(pixmap);
}

static struct wl_buffer *
xwl_glamor_eglstream_get_wl_buffer_for_pixmap(PixmapPtr pixmap)
{
    return xwl_pixmap_get(pixmap)->buffer;
}

static void
xwl_eglstream_set_window_pixmap(WindowPtr window, PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(window->drawable.pScreen);
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_eglstream_pending_stream *pending;

    pending = xwl_eglstream_window_get_pending(window);
    if (pending) {
        /* The pixmap for this window has changed before the compositor
         * finished attaching the consumer for the window's pixmap's original
         * eglstream. This means that the the pixmap has been effectively
         * orphaned, cannot have a producer attached and should have it's
         * wayland resources destroyed.
         */
        pending->pixmap_was_changed = TRUE;

        /* FIXME: we should have a release event for this
         * The stream might still be getting setup in the compositor, so wait
         * until it's callback gets invoked to destroy it's wl_buffer */
        pending->xwl_pixmap->refcount++;
    }

    xwl_screen->screen->SetWindowPixmap = xwl_eglstream->SetWindowPixmap;
    (*xwl_screen->screen->SetWindowPixmap)(window, pixmap);
    xwl_eglstream->SetWindowPixmap = xwl_screen->screen->SetWindowPixmap;
    xwl_screen->screen->SetWindowPixmap = xwl_eglstream_set_window_pixmap;
}

/* Because we run asynchronously with our wayland compositor, it's possible
 * that an X client event could cause us to begin creating a stream for a
 * pixmap/window combo before the stream for the pixmap this window
 * previously used has been fully initialized. An example:
 *
 * - Start processing X client events.
 * - X window receives resize event, causing us to create a new pixmap and
 *   begin creating the corresponding eglstream. This pixmap is known as
 *   pixmap A.
 * - X window receives another resize event, and again changes it's current
 *   pixmap causing us to create another corresponding eglstream for the same
 *   window. This pixmap is known as pixmap B.
 * - Start handling events from the wayland compositor.
 *
 * Since both pixmap A and B will have scheduled wl_display_sync events to
 * indicate when their respective streams are connected, we will receive each
 * callback in the original order the pixmaps were created. This means the
 * following would happen:
 *
 * - Receive pixmap A's stream callback, attach it's stream to the surface of
 *   the window that just orphaned it.
 * - Receive pixmap B's stream callback, fall over and fail because the
 *   window's surface now incorrectly has pixmap A's stream attached to it.
 *
 * We work around this problem by keeping a queue of pending streams, and
 * only allowing one queue entry to exist for each window. In the scenario
 * listed above, this should happen:
 *
 * - Begin processing X events...
 * - A window is resized, causing us to add an eglstream (known as eglstream
 *   A) waiting for it's consumer to finish attachment to be added to the
 *   queue.
 * - Resize on same window happens. We invalidate the previously pending
 *   stream and add another one to the pending queue (known as eglstream B).
 * - Begin processing Wayland events...
 * - Receive invalidated callback from compositor for eglstream A, destroy
 *   stream.
 * - Receive callback from compositor for eglstream B, create producer.
 * - Success!
 */
static void
xwl_eglstream_consumer_ready_callback(void *data,
                                      struct wl_callback *callback,
                                      uint32_t time)
{
    struct xwl_screen *xwl_screen = data;
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_pixmap *xwl_pixmap;
    struct xwl_eglstream_pending_stream *pending;
    Bool found = FALSE;

    wl_callback_destroy(callback);

    xorg_list_for_each_entry(pending, &xwl_eglstream->pending_streams, link) {
        if (pending->cb == callback) {
            found = TRUE;
            break;
        }
    }
    assert(found);

    if (pending->pixmap_was_changed) {
        xwl_eglstream_unref_pixmap_stream(pending->xwl_pixmap);
        goto out;
    }

    xwl_eglstream_make_current(xwl_screen, EGL_NO_SURFACE);

    xwl_pixmap = pending->xwl_pixmap;
    xwl_pixmap->surface = eglCreateStreamProducerSurfaceKHR(
        xwl_screen->egl_display, xwl_eglstream->config,
        xwl_pixmap->stream, (int[]) {
            EGL_WIDTH,  pending->pixmap->drawable.width,
            EGL_HEIGHT, pending->pixmap->drawable.height,
            EGL_NONE
        });

    DebugF("eglstream: win %d completes eglstream for pixmap %p, congrats!\n",
           pending->window->drawable.id, pending->pixmap);

    xwl_eglstream_window_set_pending(pending->window, NULL);
out:
    xorg_list_del(&pending->link);
    free(pending);
}

static const struct wl_callback_listener consumer_ready_listener = {
    xwl_eglstream_consumer_ready_callback
};

static void
xwl_eglstream_queue_pending_stream(struct xwl_screen *xwl_screen,
                                   WindowPtr window, PixmapPtr pixmap)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_eglstream_pending_stream *pending_stream;

#ifdef DEBUG
    if (!xwl_eglstream_window_get_pending(window))
        DebugF("eglstream: win %d begins new eglstream for pixmap %p\n",
               window->drawable.id, pixmap);
    else
        DebugF("eglstream: win %d interrupts and replaces pending eglstream for pixmap %p\n",
               window->drawable.id, pixmap);
#endif

    pending_stream = malloc(sizeof(*pending_stream));
    pending_stream->window = window;
    pending_stream->pixmap = pixmap;
    pending_stream->xwl_pixmap = xwl_pixmap_get(pixmap);
    pending_stream->pixmap_was_changed = FALSE;
    xorg_list_init(&pending_stream->link);
    xorg_list_add(&pending_stream->link, &xwl_eglstream->pending_streams);
    xwl_eglstream_window_set_pending(window, pending_stream);

    pending_stream->cb = wl_display_sync(xwl_screen->display);
    wl_callback_add_listener(pending_stream->cb, &consumer_ready_listener,
                             xwl_screen);
}

static void
xwl_eglstream_buffer_release_callback(void *data, struct wl_buffer *wl_buffer)
{
    xwl_eglstream_unref_pixmap_stream(data);
}

static const struct wl_buffer_listener xwl_eglstream_buffer_release_listener = {
    xwl_eglstream_buffer_release_callback
};

static void
xwl_eglstream_create_pending_stream(struct xwl_screen *xwl_screen,
                                    WindowPtr window, PixmapPtr pixmap)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_pixmap *xwl_pixmap;
    struct xwl_window *xwl_window = xwl_window_get(window);
    struct wl_array stream_attribs;

    xwl_pixmap = calloc(sizeof(*xwl_pixmap), 1);
    if (!xwl_pixmap)
        FatalError("Not enough memory to create pixmap\n");
    xwl_pixmap_set_private(pixmap, xwl_pixmap);

    xwl_eglstream_make_current(xwl_screen, EGL_NO_SURFACE);

    xwl_pixmap->xwl_screen = xwl_screen;
    xwl_pixmap->refcount++;
    xwl_pixmap->stream = eglCreateStreamKHR(xwl_screen->egl_display, NULL);
    xwl_pixmap->stream_fd = eglGetStreamFileDescriptorKHR(
        xwl_screen->egl_display, xwl_pixmap->stream);

    wl_array_init(&stream_attribs);
    xwl_pixmap->buffer =
        wl_eglstream_display_create_stream(xwl_eglstream->display,
                                           pixmap->drawable.width,
                                           pixmap->drawable.height,
                                           xwl_pixmap->stream_fd,
                                           WL_EGLSTREAM_HANDLE_TYPE_FD,
                                           &stream_attribs);

    wl_buffer_add_listener(xwl_pixmap->buffer,
                           &xwl_eglstream_buffer_release_listener,
                           xwl_pixmap);

    wl_eglstream_controller_attach_eglstream_consumer(
        xwl_eglstream->controller, xwl_window->surface, xwl_pixmap->buffer);

    xwl_eglstream_queue_pending_stream(xwl_screen, window, pixmap);

    xwl_eglstream_restore_current(xwl_screen);
}

static Bool
xwl_glamor_eglstream_allow_commits(struct xwl_window *xwl_window)
{
    struct xwl_screen *xwl_screen = xwl_window->xwl_screen;
    struct xwl_eglstream_pending_stream *pending =
        xwl_eglstream_window_get_pending(xwl_window->window);
    PixmapPtr pixmap =
        (*xwl_screen->screen->GetWindowPixmap)(xwl_window->window);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap) {
        if (pending) {
            /* Wait for the compositor to finish connecting the consumer for
             * this eglstream */
            if (!pending->pixmap_was_changed)
                return FALSE;

            /* The pixmap for this window was changed before the compositor
             * finished connecting the eglstream for the window's previous
             * pixmap. Don't allow commits, and begin the process of
             * connecting a new eglstream */
        } else {
            /* Pixmap's eglstream is ready for use */
            return TRUE;
        }
    }

    xwl_eglstream_create_pending_stream(xwl_screen, xwl_window->window,
                                        pixmap);

    /* We don't know the state of the consumer until the next time we process
     * events from the Wayland compositor, so disable commits to this window
     * until then to prevent us from blitting to an invalid eglsurface */
    return FALSE;
}

static void
xwl_glamor_eglstream_post_damage(struct xwl_window *xwl_window,
                                 PixmapPtr pixmap, RegionPtr region)
{
    struct xwl_screen *xwl_screen = xwl_window->xwl_screen;
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);
    BoxPtr box = RegionExtents(region);
    EGLint egl_damage[] = {
        box->x1,           box->y1,
        box->x2 - box->x1, box->y2 - box->y1
    };
    GLint saved_vao;

    /* Unbind the framebuffer BEFORE binding the EGLSurface, otherwise we
     * won't actually draw to it
     */
    xwl_eglstream_make_current(xwl_screen, EGL_NO_SURFACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    xwl_eglstream_make_current(xwl_screen, xwl_pixmap->surface);

    /* Save current GL state */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);

    /* Setup our GL state */
    glUseProgram(xwl_eglstream->blit_prog);
    glViewport(0, 0, pixmap->drawable.width, pixmap->drawable.height);
    glDisable(GL_COLOR_LOGIC_OP);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(xwl_eglstream->blit_vao);
    glBindTexture(GL_TEXTURE_2D, glamor_get_pixmap_texture(pixmap));

    /* Blit rendered image into EGLStream surface */
    glDrawBuffer(GL_BACK);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    if (xwl_eglstream->have_egl_damage)
        eglSwapBuffersWithDamageKHR(xwl_screen->egl_display,
                                    xwl_pixmap->surface, egl_damage, 1);
    else
        eglSwapBuffers(xwl_screen->egl_display, xwl_pixmap->surface);

    /* Restore previous state */
    glBindVertexArray(saved_vao);
    glBindTexture(GL_TEXTURE_2D, 0);

    xwl_eglstream_restore_current(xwl_screen);

    /* After this we will hand off the eglstream's wl_buffer to the
     * compositor, which will own it until it sends a release() event. */
    xwl_pixmap->refcount++;
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

static inline void
xwl_glamor_eglstream_init_shaders(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    GLint fs, vs, attrib;
    GLuint vbo;

    const char *blit_vs_src =
        "attribute vec2 texcoord;\n"
        "attribute vec2 position;\n"
        "varying vec2 t;\n"
        "void main() {\n"
        "   t = texcoord;\n"
        "   gl_Position = vec4(position, 0, 1);\n"
        "}";

    const char *blit_fs_src =
        "varying vec2 t;\n"
        "uniform sampler2D s;\n"
        "void main() {\n"
        "   gl_FragColor = texture2D(s, t);\n"
        "}";

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

    /* Create the blitter's vao */
    glGenVertexArrays(1, &xwl_eglstream->blit_vao);
    glBindVertexArray(xwl_eglstream->blit_vao);

    /* Set the data for both position and texcoord in the vbo */
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);
    xwl_eglstream->blit_vbo = vbo;

    /* Define each shader attribute's data location in our vbo */
    attrib = glGetAttribLocation(xwl_eglstream->blit_prog, "position");
    glVertexAttribPointer(attrib, 2, GL_FLOAT, TRUE, 0, NULL);
    glEnableVertexAttribArray(attrib);

    attrib = glGetAttribLocation(xwl_eglstream->blit_prog, "texcoord");
    glVertexAttribPointer(attrib, 2, GL_FLOAT, TRUE, 0,
                          (void*)(sizeof(float) * 8));
    glEnableVertexAttribArray(attrib);
}

static Bool
xwl_glamor_eglstream_init_egl(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_eglstream_get(xwl_screen);
    EGLConfig config;
    const EGLint attrib_list[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR,
        GLAMOR_GL_CORE_VER_MAJOR,
        EGL_CONTEXT_MINOR_VERSION_KHR,
        GLAMOR_GL_CORE_VER_MINOR,
        EGL_NONE
    };
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    int n;

    xwl_screen->egl_display = glamor_egl_get_display(EGL_PLATFORM_DEVICE_EXT,
                                                     xwl_screen->egl_device);
    if (!xwl_screen->egl_display)
        goto error;

    if (!eglInitialize(xwl_screen->egl_display, NULL, NULL)) {
        xwl_screen->egl_display = NULL;
        goto error;
    }

    eglChooseConfig(xwl_screen->egl_display, config_attribs, &config, 1, &n);
    if (!n) {
        ErrorF("No acceptable EGL configs found\n");
        goto error;
    }

    xwl_eglstream->config = config;
    xwl_screen->formats =
        XWL_FORMAT_RGB565 | XWL_FORMAT_XRGB8888 | XWL_FORMAT_ARGB8888;

    eglBindAPI(EGL_OPENGL_API);
    xwl_screen->egl_context = eglCreateContext(
        xwl_screen->egl_display, config, EGL_NO_CONTEXT, attrib_list);
    if (xwl_screen->egl_context == EGL_NO_CONTEXT) {
        ErrorF("Failed to create main EGL context: 0x%x\n", eglGetError());
        goto error;
    }

    if (!eglMakeCurrent(xwl_screen->egl_display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        xwl_screen->egl_context)) {
        ErrorF("Failed to make EGL context current\n");
        goto error;
    }
    lyude_enable_debug(); /* FIXME */

    xwl_eglstream->have_egl_damage =
        epoxy_has_egl_extension(xwl_screen->egl_display,
                                "EGL_KHR_swap_buffers_with_damage");
    if (!xwl_eglstream->have_egl_damage)
        ErrorF("Driver lacks EGL_KHR_swap_buffers_with_damage, performance "
               "will be affected\n");

    xwl_glamor_eglstream_init_shaders(xwl_screen);

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

    /* We can just let glamor handle CreatePixmap */
    xwl_screen->screen->DestroyPixmap = xwl_glamor_eglstream_destroy_pixmap;

    xwl_eglstream->SetWindowPixmap = xwl_screen->screen->SetWindowPixmap;
    xwl_screen->screen->SetWindowPixmap = xwl_eglstream_set_window_pixmap;

    if (!dixRegisterPrivateKey(&xwl_eglstream_window_private_key,
                               PRIVATE_WINDOW, 0))
        return FALSE;

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

    if (!dixRegisterPrivateKey(&xwl_eglstream_private_key, PRIVATE_SCREEN, 0))
        return FALSE;

    xwl_eglstream = calloc(sizeof(*xwl_eglstream), 1);
    if (!xwl_eglstream) {
        ErrorF("Failed to allocate memory required to init eglstream support\n");
        return FALSE;
    }

    dixSetPrivate(&xwl_screen->screen->devPrivates,
                  &xwl_eglstream_private_key, xwl_eglstream);

    xorg_list_init(&xwl_eglstream->pending_streams);

    xwl_screen->egl_backend.init_egl = xwl_glamor_eglstream_init_egl;
    xwl_screen->egl_backend.init_wl_registry = xwl_glamor_eglstream_init_wl_registry;
    xwl_screen->egl_backend.init_screen = xwl_glamor_eglstream_init_screen;
    xwl_screen->egl_backend.get_wl_buffer_for_pixmap = xwl_glamor_eglstream_get_wl_buffer_for_pixmap;
    xwl_screen->egl_backend.post_damage = xwl_glamor_eglstream_post_damage;
    xwl_screen->egl_backend.allow_commits = xwl_glamor_eglstream_allow_commits;

    ErrorF("glamor: Using nvidia's eglstream interface, direct rendering impossible.\n");
    ErrorF("glamor: Performance may be affected. Ask your vendor to support GBM!\n");
    return TRUE;
}
