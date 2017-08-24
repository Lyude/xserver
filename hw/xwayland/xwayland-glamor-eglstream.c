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
#include <glamor_context.h>
#include <glamor_transfer.h>

#include <xf86drm.h>

#include <epoxy/egl.h>

struct xwl_eglstream_private {
    struct wl_eglstream_display *display;
    struct wl_eglstream_controller *controller;
    void *egl_display;
    uint32_t display_caps;
};

static Bool already_failed;

static inline struct xwl_eglstream_private *
get_xwl_eglstream(struct xwl_screen *xwl_screen)
{
    return xwl_screen->egl_backend.priv;
}

/* TODO FIXME
 * We should modify libepoxy to expose this from it's own library to everyone
 * else
 */
static bool
egl_extension_in_string(const char *extension_list, const char *ext)
{
    const char *ptr = extension_list;
    int len;

    if (!ext)
        return false;

    len = strlen(ext);

    if (extension_list == NULL || *extension_list == '\0')
        return false;

    /* Make sure that don't just find an extension with our name as a prefix */
    while (true) {
        ptr = strstr(ptr, ext);
        if (!ptr)
            return false;

        if (ptr[len] == ' ' || ptr[len] == 0)
            return true;
        ptr += len;
    }
}

static EGLConfig
egl_config_for_depth(EGLDisplay egl_display, int depth)
{
    EGLConfig config;
    int *attrib_list, n;
    Bool ret;

    switch (depth) {
    case 16:
        attrib_list = (int[]) {
            EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
            EGL_RED_SIZE, 5,
            EGL_GREEN_SIZE, 6,
            EGL_BLUE_SIZE, 5,
            EGL_ALPHA_SIZE, 0,
            EGL_NONE
        };
        break;
    case 24:
        attrib_list = (int[]) {
            EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 0,
            EGL_NONE
        };
        break;
    default:
        ErrorF("unexpected depth: %d\n", depth);
    case 32:
        attrib_list = (int[]) {
            EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        break;
    }

    ret = eglChooseConfig(egl_display, attrib_list, &config, 1, &n);
    if (!ret)
        return EGL_NO_CONFIG_KHR;

    return config;
}

static void
xwl_eglstream_display_handle_caps(void *data,
                                  struct wl_eglstream_display *disp,
                                  int32_t caps)
{
    struct xwl_screen *xwl_screen = data;
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_screen->egl_backend.priv;

    xwl_eglstream->display_caps = caps;
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
xwl_glamor_eglstream_cleanup(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_screen->egl_backend.priv;

    if (xwl_eglstream->display)
        wl_eglstream_display_destroy(xwl_eglstream->display);
    if (xwl_eglstream->controller)
        wl_eglstream_controller_destroy(xwl_eglstream->controller);

    free(xwl_eglstream);
    xwl_screen->egl_backend.priv = NULL;
    xwl_screen->egl_backend.initialized = FALSE;
    already_failed = TRUE;
}

static Bool
xwl_glamor_eglstream_init_egl(struct xwl_screen *xwl_screen)
{
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE,
    };
    EGLConfig config;
    int _unused;
    Bool ret;

    ret = eglChooseConfig(xwl_screen->egl_display, config_attribs, &config,
                          1, &_unused);
    if (!ret) {
        ErrorF("Failed to find suitable EGL config\n");
        goto error;
    }

    xwl_screen->egl_context = eglCreateContext(
        xwl_screen->egl_display, config, EGL_NO_CONTEXT,
        (int[]) { EGL_NONE });

    return TRUE;
error:
    xwl_glamor_eglstream_cleanup(xwl_screen);
    return FALSE;
}

static PixmapPtr
xwl_glamor_eglstream_create_pixmap(ScreenPtr screen,
                                   int width, int height, int depth,
                                   unsigned int hint)
{
    PixmapPtr pixmap;
    struct xwl_pixmap *xwl_pixmap;

    xwl_pixmap = calloc(sizeof *xwl_pixmap, 1);
    if (!xwl_pixmap)
        return NULL;

    xwl_pixmap->backing = EGL_NO_STREAM_KHR;

    pixmap = glamor_create_pixmap(screen, width, height, depth, hint);
    if (!pixmap) {
        ErrorF("Failed to create glamor pixmap\n");
        free(xwl_pixmap);
        return NULL;
    }

    xwl_pixmap_set_private(pixmap, xwl_pixmap);
    xwl_pixmap->texture = glamor_get_pixmap_texture(pixmap);

    return pixmap;
}

static Bool
xwl_glamor_eglstream_destroy_pixmap(PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap && pixmap->refcnt == 1) {
        if (xwl_pixmap->buffer)
            wl_buffer_destroy(xwl_pixmap->buffer);

        eglDestroyStreamKHR(xwl_screen->egl_display, xwl_pixmap->backing);
        eglDestroySurface(xwl_screen->egl_display, xwl_pixmap->image);
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
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_screen->egl_backend.priv;
    EGLConfig config;
    struct wl_buffer *buffer;
    struct wl_array stream_attribs;
    int *attrib_list;
    int fd = EGL_NO_FILE_DESCRIPTOR_KHR;
    Bool ret;

    if (lastGLContext != xwl_screen->glamor_ctx) {
        lastGLContext = xwl_screen->glamor_ctx;
        xwl_glamor_egl_make_current(xwl_screen->glamor_ctx);
    }

    xwl_pixmap->backing = eglCreateStreamKHR(xwl_screen->egl_display, NULL);
    if (xwl_pixmap->backing == EGL_NO_STREAM_KHR) {
        ErrorF("Failed to create EGLStreamKHR object\n");
        goto error;
    }

    fd = eglGetStreamFileDescriptorKHR(xwl_screen->egl_display,
                                       xwl_pixmap->backing);
    if (fd == EGL_NO_FILE_DESCRIPTOR_KHR) {
        ErrorF("Failed to get file descriptor for egl stream\n");
        return NULL;
    }

    config = egl_config_for_depth(xwl_screen->egl_display,
                                  pixmap->drawable.depth);
    if (config == EGL_NO_CONFIG_KHR) {
        ErrorF("Unable to find config for pixmap with depth of %d\n",
               pixmap->drawable.depth);
        goto error;
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
    xwl_pixmap->image = eglCreateStreamProducerSurfaceKHR(
        xwl_screen->egl_display, config, xwl_pixmap->backing, attrib_list);
    if (xwl_pixmap->image == EGL_NO_SURFACE) {
        ErrorF("Failed to create EGLStream's producer surface\n");
        goto error;
    }

    /* FIXME: just retrieve the texture here, we don't need to carry it around
     * with us
     */
    ret = eglBindTexImage(xwl_screen->egl_display, xwl_pixmap->image,
                          xwl_pixmap->texture);
    if (!ret) {
        ErrorF("Failed to bind texture to eglstream producer\n");
        goto error;
    }

    return buffer;

error:
    if (fd != EGL_NO_FILE_DESCRIPTOR_KHR)
        close(fd);
    if (xwl_pixmap->backing != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(xwl_screen->egl_display, xwl_pixmap->backing);
        xwl_pixmap->backing = EGL_NO_STREAM_KHR;
    }
    if (xwl_pixmap->image != EGL_NO_SURFACE) {
        eglDestroySurface(xwl_screen->egl_display, xwl_pixmap->image);
        xwl_pixmap->image = EGL_NO_SURFACE;
    }

    return NULL;
}

static Bool
xwl_glamor_eglstream_init_screen(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream =
        xwl_screen->egl_backend.priv;

    if (!xwl_eglstream->controller) {
        ErrorF("No eglstream controller was exposed in the wayland registry. "
               "This means your version of nvidia's EGL wayland libraries "
               "are too old, as we require support for this. Sorry, but see "
               "ya!\n");
        xwl_glamor_eglstream_cleanup(xwl_screen);
        return FALSE;
    }

    xwl_screen->screen->CreatePixmap = xwl_glamor_eglstream_create_pixmap;
    xwl_screen->screen->DestroyPixmap = xwl_glamor_eglstream_destroy_pixmap;

    return TRUE;
}

/*
 * Unlike GBM, we need to have our EGL display ready before glamor asks for it
 * since we need to actually check the extensions supported by the display to
 * make sure that EGLStreams are actually going to work. Luckily, calling
 * eglInitialize() twice isn't an error according to the spec.
 */
static void *
xwl_glamor_eglstream_try_get_device_display(EGLDeviceEXT device)
{
    const char *required_exts[] = {
        "OES_EGL_image_external",
        "EGL_KHR_stream",
        "EGL_KHR_stream_producer_eglsurface",
    };
    void *egl_display;
    Bool egl_initialized;
    int i;

    egl_display = glamor_egl_get_display(EGL_PLATFORM_DEVICE_EXT, device);
    if (egl_display == EGL_NO_DISPLAY)
        return EGL_NO_DISPLAY;

    egl_initialized = eglInitialize(egl_display, NULL, NULL);
    if (!egl_initialized)
        return EGL_NO_DISPLAY;

    for (i = 0; i < ARRAY_SIZE(required_exts); i++) {
        if (!epoxy_has_egl_extension(egl_display, required_exts[i])) {
            eglTerminate(egl_display);
            return EGL_NO_DISPLAY;
        }
    }

    return egl_display;
}

static void *
xwl_glamor_eglstream_probe_devices(void)
{
    EGLDeviceEXT *devices = NULL;
    void *egl_display = NULL;
    int num_devices, i;
    Bool ret;

    /* Get the number of devices */
    ret = eglQueryDevicesEXT(0, NULL, &num_devices);
    if (!ret || num_devices < 1)
        return EGL_NO_DISPLAY;

    devices = calloc(num_devices, sizeof(EGLDeviceEXT));
    if (!devices)
        return EGL_NO_DISPLAY;

    ret = eglQueryDevicesEXT(num_devices, devices, &num_devices);
    if (!ret) {
        free(devices);
        return EGL_NO_DISPLAY;
    }

    for (i = 0; i < num_devices; i++) {
        const char *extension_str = extension_str = eglQueryDeviceStringEXT(
            devices[i], EGL_EXTENSIONS);

        if (!egl_extension_in_string(extension_str, "EGL_EXT_device_drm"))
            continue;

        egl_display = xwl_glamor_eglstream_try_get_device_display(devices[i]);
        if (egl_display != EGL_NO_DISPLAY)
            break;
    }

    free(devices);
    return egl_display;
}

static Bool
xwl_glamor_eglstream_init(struct xwl_screen *xwl_screen)
{
    struct xwl_eglstream_private *xwl_eglstream;
    void *egl_display;

    if (already_failed)
        return FALSE;
    if (xwl_screen->egl_backend.initialized)
        return TRUE;

    if (!(epoxy_has_egl_extension(NULL, "EGL_EXT_platform_base") &&
          epoxy_has_egl_extension(NULL, "EGL_EXT_platform_device") &&
          (epoxy_has_egl_extension(NULL, "EGL_EXT_device_base") ||
           (epoxy_has_egl_extension(NULL, "EGL_EXT_device_enumeration") &&
            epoxy_has_egl_extension(NULL, "EGL_EXT_device_query")))))
        goto error;

    egl_display = xwl_glamor_eglstream_probe_devices();
    if (!egl_display) {
        ErrorF("No EGLStream capable devices found, disabling\n");
        goto error;
    }

    xwl_eglstream = calloc(sizeof(*xwl_eglstream), 1);
    if (!xwl_eglstream) {
        ErrorF("Failed to allocate memory required to init eglstream support\n");
        goto error;
    }

    xwl_eglstream->egl_display = egl_display;

    xwl_screen->egl_backend.priv = xwl_eglstream;
    xwl_screen->egl_backend.init_egl = xwl_glamor_eglstream_init_egl;
    xwl_screen->egl_backend.init_screen =
        xwl_glamor_eglstream_init_screen;
    xwl_screen->egl_backend.get_wl_buffer_for_pixmap =
        xwl_glamor_eglstream_get_wl_buffer_for_pixmap;

    ErrorF("glamor: Using nvidia's proprietary eglstream interface\n");

    xwl_screen->egl_backend.initialized = TRUE;

    return TRUE;
error:
    already_failed = TRUE;
    return FALSE;
}

Bool xwl_glamor_eglstream_bind_display(struct xwl_screen *xwl_screen,
                                       struct wl_registry *registry,
                                       uint32_t id, uint32_t version)
{
    struct xwl_eglstream_private *xwl_eglstream;

    if (!xwl_glamor_eglstream_init(xwl_screen))
        return FALSE;

    xwl_eglstream = xwl_screen->egl_backend.priv;
    xwl_eglstream->display = wl_registry_bind(registry, id,
                                              &wl_eglstream_display_interface,
                                              version);
    if (!xwl_eglstream->display) {
        ErrorF("Failed to bind wl_eglstream_display\n");
        xwl_glamor_eglstream_cleanup(xwl_screen);
        return FALSE;
    }

    wl_eglstream_display_add_listener(xwl_eglstream->display,
                                      &eglstream_display_listener,
                                      xwl_screen);

    return TRUE;
}

Bool xwl_glamor_eglstream_bind_controller(struct xwl_screen *xwl_screen,
                                          struct wl_registry *registry,
                                          uint32_t id, uint32_t version)
{
    struct xwl_eglstream_private *xwl_eglstream;

    if (!xwl_glamor_eglstream_init(xwl_screen))
        return FALSE;

    xwl_eglstream = xwl_screen->egl_backend.priv;
    xwl_eglstream->controller = wl_registry_bind(
        registry, id, &wl_eglstream_controller_interface, version);
    if (!xwl_eglstream->controller) {
        ErrorF("Failed to bind wl_eglstream_controller\n");
        xwl_glamor_eglstream_cleanup(xwl_screen);
    }

    return TRUE;
}
