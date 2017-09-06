/*
 * Copyright © 2011-2014 Intel Corporation
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

#include <fcntl.h>
#include <sys/stat.h>
#include <xf86drm.h>

#define MESA_EGL_NO_X11_HEADERS
#include <gbm.h>
#include <glamor_egl.h>

#include <glamor.h>
#include <glamor_context.h>
#include <dri3.h>
#include "drm-client-protocol.h"

struct xwl_gbm_private {
    struct gbm_device *gbm;
    struct wl_drm *drm;
    char *device_name;
    int drm_fd;
    int fd_render_node;
    Bool drm_authenticated;
    uint32_t capabilities;
};

struct xwl_pixmap {
    struct wl_buffer *buffer;
    EGLImage image;
    unsigned int texture;
    struct gbm_bo *bo;
};

static DevPrivateKeyRec xwl_auth_state_private_key;

static inline struct xwl_gbm_private *
xwl_gbm_get(struct xwl_screen *xwl_screen)
{
    return xwl_screen->egl_backend.priv;
}

static uint32_t
gbm_format_for_depth(int depth)
{
    switch (depth) {
    case 16:
        return GBM_FORMAT_RGB565;
    case 24:
        return GBM_FORMAT_XRGB8888;
    default:
        ErrorF("unexpected depth: %d\n", depth);
    case 32:
        return GBM_FORMAT_ARGB8888;
    }
}

static uint32_t
drm_format_for_depth(int depth)
{
    switch (depth) {
    case 15:
        return WL_DRM_FORMAT_XRGB1555;
    case 16:
        return WL_DRM_FORMAT_RGB565;
    case 24:
        return WL_DRM_FORMAT_XRGB8888;
    default:
        ErrorF("unexpected depth: %d\n", depth);
    case 32:
        return WL_DRM_FORMAT_ARGB8888;
    }
}

static char
is_fd_render_node(int fd)
{
    struct stat render;

    if (fstat(fd, &render))
        return 0;
    if (!S_ISCHR(render.st_mode))
        return 0;
    if (render.st_rdev & 0x80)
        return 1;

    return 0;
}

static PixmapPtr
xwl_glamor_gbm_create_pixmap_for_bo(ScreenPtr screen, struct gbm_bo *bo,
                                    int depth)
{
    PixmapPtr pixmap;
    struct xwl_pixmap *xwl_pixmap;
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);

    xwl_pixmap = malloc(sizeof *xwl_pixmap);
    if (xwl_pixmap == NULL)
        return NULL;

    pixmap = glamor_create_pixmap(screen,
                                  gbm_bo_get_width(bo),
                                  gbm_bo_get_height(bo),
                                  depth,
                                  GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
    if (!pixmap) {
        free(xwl_pixmap);
        return NULL;
    }

    if (lastGLContext != xwl_screen->glamor_ctx) {
        lastGLContext = xwl_screen->glamor_ctx;
        xwl_glamor_egl_make_current(xwl_screen->glamor_ctx);
    }

    xwl_pixmap->bo = bo;
    xwl_pixmap->buffer = NULL;
    xwl_pixmap->image = eglCreateImageKHR(xwl_screen->egl_display,
                                          xwl_screen->egl_context,
                                          EGL_NATIVE_PIXMAP_KHR,
                                          xwl_pixmap->bo, NULL);

    glGenTextures(1, &xwl_pixmap->texture);
    glBindTexture(GL_TEXTURE_2D, xwl_pixmap->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, xwl_pixmap->image);
    glBindTexture(GL_TEXTURE_2D, 0);

    xwl_pixmap_set_private(pixmap, xwl_pixmap);

    glamor_set_pixmap_texture(pixmap, xwl_pixmap->texture);
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);

    return pixmap;
}

static PixmapPtr
xwl_glamor_gbm_create_pixmap(ScreenPtr screen,
                             int width, int height, int depth,
                             unsigned int hint)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
    struct gbm_bo *bo;

    if (width > 0 && height > 0 && depth >= 15 &&
        (hint == 0 ||
         hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP ||
         hint == CREATE_PIXMAP_USAGE_SHARED)) {
        bo = gbm_bo_create(xwl_gbm->gbm, width, height,
                           gbm_format_for_depth(depth),
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

        if (bo)
            return xwl_glamor_gbm_create_pixmap_for_bo(screen, bo, depth);
    }

    return glamor_create_pixmap(screen, width, height, depth, hint);
}

static Bool
xwl_glamor_gbm_destroy_pixmap(PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap && pixmap->refcnt == 1) {
        if (xwl_pixmap->buffer)
            wl_buffer_destroy(xwl_pixmap->buffer);

        eglDestroyImageKHR(xwl_screen->egl_display, xwl_pixmap->image);
        gbm_bo_destroy(xwl_pixmap->bo);
        free(xwl_pixmap);
    }

    return glamor_destroy_pixmap(pixmap);
}

static struct wl_buffer *
xwl_glamor_gbm_get_wl_buffer_for_pixmap(PixmapPtr pixmap,
                                        WindowPtr window)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
    struct wl_buffer *buffer;
    int prime_fd;

    if (xwl_pixmap->buffer)
        return xwl_pixmap->buffer;

    prime_fd = gbm_bo_get_fd(xwl_pixmap->bo);
    if (prime_fd == -1)
        return NULL;

    buffer = wl_drm_create_prime_buffer(
        xwl_gbm->drm, prime_fd,
        pixmap->drawable.width, pixmap->drawable.height,
        drm_format_for_depth(pixmap->drawable.depth),
        0, gbm_bo_get_stride(xwl_pixmap->bo), 0, 0, 0, 0);

    close(prime_fd);

    return buffer;
}

static void
xwl_glamor_gbm_post_damage(struct xwl_screen *xwl_screen,
                           struct xwl_window *xwl_window,
                           PixmapPtr pixmap, RegionPtr region)
{
    /* Unlike certain other backends, GBM doesn't need to do any copying :) */
}

static void
xwl_glamor_gbm_cleanup(struct xwl_screen *xwl_screen)
{
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);

    if (xwl_gbm->device_name)
        free(xwl_gbm->device_name);
    if (xwl_gbm->drm_fd)
        close(xwl_gbm->drm_fd);
    if (xwl_gbm->drm)
        wl_drm_destroy(xwl_gbm->drm);
    if (xwl_gbm->gbm)
        gbm_device_destroy(xwl_gbm->gbm);

    free(xwl_gbm);
}

struct xwl_auth_state {
    int fd;
    ClientPtr client;
    struct wl_callback *callback;
};

static void
free_xwl_auth_state(ClientPtr pClient, struct xwl_auth_state *state)
{
    dixSetPrivate(&pClient->devPrivates, &xwl_auth_state_private_key, NULL);
    if (state) {
        wl_callback_destroy(state->callback);
        free(state);
    }
}

static void
xwl_auth_state_client_callback(CallbackListPtr *pcbl, void *unused, void *data)
{
    NewClientInfoRec *clientinfo = (NewClientInfoRec *) data;
    ClientPtr pClient = clientinfo->client;
    struct xwl_auth_state *state;

    switch (pClient->clientState) {
    case ClientStateGone:
    case ClientStateRetained:
        state = dixLookupPrivate(&pClient->devPrivates,
                                 &xwl_auth_state_private_key);
        free_xwl_auth_state(pClient, state);
        break;
    default:
        break;
    }
}

static void
sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    struct xwl_auth_state *state = data;
    ClientPtr client = state->client;

    /* if the client is gone, the callback is cancelled so it's safe to
     * assume the client is still in ClientStateRunning at this point...
     */
    dri3_send_open_reply(client, state->fd);
    AttendClient(client);
    free_xwl_auth_state(client, state);
}

static const struct wl_callback_listener sync_listener = {
   sync_callback
};

static int
xwl_dri3_open_client(ClientPtr client,
                     ScreenPtr screen,
                     RRProviderPtr provider,
                     int *pfd)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
    struct xwl_auth_state *state;
    drm_magic_t magic;
    int fd;

    fd = open(xwl_gbm->device_name, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return BadAlloc;
    if (xwl_gbm->fd_render_node) {
        *pfd = fd;
        return Success;
    }

    state = malloc(sizeof *state);
    if (state == NULL) {
        close(fd);
        return BadAlloc;
    }

    state->client = client;
    state->fd = fd;

    if (drmGetMagic(state->fd, &magic) < 0) {
        close(state->fd);
        free(state);
        return BadMatch;
    }

    wl_drm_authenticate(xwl_gbm->drm, magic);
    state->callback = wl_display_sync(xwl_screen->display);
    wl_callback_add_listener(state->callback, &sync_listener, state);
    dixSetPrivate(&client->devPrivates, &xwl_auth_state_private_key, state);

    IgnoreClient(client);

    return Success;
}

static PixmapPtr
xwl_dri3_pixmap_from_fd(ScreenPtr screen, int fd,
                        CARD16 width, CARD16 height, CARD16 stride,
                        CARD8 depth, CARD8 bpp)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
    struct gbm_import_fd_data data;
    struct gbm_bo *bo;
    PixmapPtr pixmap;

    if (width == 0 || height == 0 ||
        depth < 15 || bpp != BitsPerPixel(depth) || stride < width * bpp / 8)
        return NULL;

    data.fd = fd;
    data.width = width;
    data.height = height;
    data.stride = stride;
    data.format = gbm_format_for_depth(depth);
    bo = gbm_bo_import(xwl_gbm->gbm, GBM_BO_IMPORT_FD, &data,
                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (bo == NULL)
        return NULL;

    pixmap = xwl_glamor_gbm_create_pixmap_for_bo(screen, bo, depth);
    if (pixmap == NULL) {
        gbm_bo_destroy(bo);
        return NULL;
    }

    return pixmap;
}

static int
xwl_dri3_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                        CARD16 *stride, CARD32 *size)
{
    struct xwl_pixmap *xwl_pixmap;

    xwl_pixmap = xwl_pixmap_get(pixmap);

    *stride = gbm_bo_get_stride(xwl_pixmap->bo);
    *size = pixmap->drawable.width * *stride;

    return gbm_bo_get_fd(xwl_pixmap->bo);
}

static dri3_screen_info_rec xwl_dri3_info = {
    .version = 1,
    .open = NULL,
    .pixmap_from_fd = xwl_dri3_pixmap_from_fd,
    .fd_from_pixmap = xwl_dri3_fd_from_pixmap,
    .open_client = xwl_dri3_open_client,
};

static void
xwl_drm_handle_device(void *data, struct wl_drm *drm, const char *device)
{
   struct xwl_screen *xwl_screen = data;
   struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
   drm_magic_t magic;

   xwl_gbm->device_name = strdup(device);
   if (!xwl_gbm->device_name) {
       xwl_glamor_gbm_cleanup(xwl_screen);
       return;
   }

   xwl_gbm->drm_fd = open(xwl_gbm->device_name, O_RDWR | O_CLOEXEC);
   if (xwl_gbm->drm_fd == -1) {
       ErrorF("wayland-egl: could not open %s (%s)\n",
              xwl_gbm->device_name, strerror(errno));
       xwl_glamor_gbm_cleanup(xwl_screen);
       return;
   }

   if (is_fd_render_node(xwl_gbm->drm_fd)) {
       xwl_gbm->fd_render_node = 1;
   } else {
       drmGetMagic(xwl_gbm->drm_fd, &magic);
       wl_drm_authenticate(xwl_gbm->drm, magic);
   }
}

static void
xwl_drm_handle_format(void *data, struct wl_drm *drm, uint32_t format)
{
   struct xwl_screen *xwl_screen = data;

   switch (format) {
   case WL_DRM_FORMAT_ARGB8888:
      xwl_screen->formats |= XWL_FORMAT_ARGB8888;
      break;
   case WL_DRM_FORMAT_XRGB8888:
      xwl_screen->formats |= XWL_FORMAT_XRGB8888;
      break;
   case WL_DRM_FORMAT_RGB565:
      xwl_screen->formats |= XWL_FORMAT_RGB565;
      break;
   }
}

static void
xwl_drm_handle_authenticated(void *data, struct wl_drm *drm)
{
    xwl_gbm_get(data)->drm_authenticated = TRUE;
}

static void
xwl_drm_handle_capabilities(void *data, struct wl_drm *drm, uint32_t value)
{
    xwl_gbm_get(data)->capabilities = value;
}

static const struct wl_drm_listener xwl_drm_listener = {
    xwl_drm_handle_device,
    xwl_drm_handle_format,
    xwl_drm_handle_authenticated,
    xwl_drm_handle_capabilities
};

static void
xwl_glamor_gbm_init_wl_registry(struct xwl_screen *xwl_screen,
                                struct wl_registry *wl_registry,
                                const char *name,
                                uint32_t id, uint32_t version)
{
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);

    if (strcmp(name, "wl_drm") != 0)
        return;

    if (version < 2) {
        ErrorF("glamor gbm: wl_drm version %d is too old, we require at least v2\n",
               version);
        xwl_glamor_gbm_cleanup(xwl_screen);
        return;
    }

    xwl_gbm->drm = wl_registry_bind(xwl_screen->registry,
                                    id, &wl_drm_interface, 2);
    wl_drm_add_listener(xwl_gbm->drm, &xwl_drm_listener, xwl_screen);
    xwl_screen->expecting_event++;
}

static Bool
xwl_glamor_gbm_init_egl(struct xwl_screen *xwl_screen)
{
    struct xwl_gbm_private *xwl_gbm = xwl_gbm_get(xwl_screen);
    EGLint major, minor;
    Bool egl_initialized = FALSE;
    static const EGLint config_attribs_core[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR,
        GLAMOR_GL_CORE_VER_MAJOR,
        EGL_CONTEXT_MINOR_VERSION_KHR,
        GLAMOR_GL_CORE_VER_MINOR,
        EGL_NONE
    };

    xwl_screen->expecting_event--;

    xwl_gbm->gbm = gbm_create_device(xwl_gbm->drm_fd);
    if (!xwl_gbm->gbm) {
        ErrorF("couldn't create gbm device\n");
        goto error;
    }

    xwl_screen->egl_display = glamor_egl_get_display(EGL_PLATFORM_GBM_MESA,
                                                     xwl_gbm->gbm);
    if (xwl_screen->egl_display == EGL_NO_DISPLAY) {
        ErrorF("glamor_egl_get_display() failed\n");
        goto error;
    }

    egl_initialized = eglInitialize(xwl_screen->egl_display, &major, &minor);
    if (!egl_initialized) {
        ErrorF("eglInitialize() failed\n");
        goto error;
    }

    eglBindAPI(EGL_OPENGL_API);

    xwl_screen->egl_context = eglCreateContext(
        xwl_screen->egl_display, NULL, EGL_NO_CONTEXT, config_attribs_core);
    if (xwl_screen->egl_context == EGL_NO_CONTEXT) {
        xwl_screen->egl_context = eglCreateContext(
            xwl_screen->egl_display, NULL, EGL_NO_CONTEXT, NULL);
    }

    if (xwl_screen->egl_context == EGL_NO_CONTEXT) {
        ErrorF("Failed to create EGL context\n");
        goto error;
    }

    if (!eglMakeCurrent(xwl_screen->egl_display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        xwl_screen->egl_context)) {
        ErrorF("Failed to make EGL context current\n");
        goto error;
    }

    if (!epoxy_has_gl_extension("GL_OES_EGL_image"))
        ErrorF("GL_OES_EGL_image not available\n");

    return TRUE;
error:
    if (xwl_screen->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(xwl_screen->egl_display, xwl_screen->egl_context);
        xwl_screen->egl_context = EGL_NO_CONTEXT;
    }

    if (xwl_screen->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(xwl_screen->egl_display);
        xwl_screen->egl_display = EGL_NO_DISPLAY;
    }

    xwl_glamor_gbm_cleanup(xwl_screen);
    return FALSE;
}

static Bool
xwl_glamor_gbm_init_screen(struct xwl_screen *xwl_screen)
{
    if (!dri3_screen_init(xwl_screen->screen, &xwl_dri3_info)) {
        ErrorF("Failed to initialize dri3\n");
        goto error;
    }

    if (!dixRegisterPrivateKey(&xwl_auth_state_private_key, PRIVATE_CLIENT,
                               0)) {
        ErrorF("Failed to register private key\n");
        goto error;
    }

    if (!AddCallback(&ClientStateCallback, xwl_auth_state_client_callback,
                     NULL)) {
        ErrorF("Failed to add client state callback\n");
        goto error;
    }

    xwl_screen->screen->CreatePixmap = xwl_glamor_gbm_create_pixmap;
    xwl_screen->screen->DestroyPixmap = xwl_glamor_gbm_destroy_pixmap;

    return TRUE;
error:
    xwl_glamor_gbm_cleanup(xwl_screen);
    return FALSE;
}

/* TODO: Actually probe for something that tells us this device can use gbm */
static Bool
xwl_glamor_gbm_get_device(struct xwl_screen *xwl_screen)
{
    void **devices;
    int num_devices;

    /* Make sure we're the default backend on systems without EGLDevice
     * probing
     */
    if (!xwl_glamor_egl_supports_device_probing())
        return TRUE;

    /* The user specified a device */
    if (xwl_screen->egl_device) {
        return TRUE;
    }

    /* No device provided, probe for one */
    devices = xwl_glamor_egl_get_devices(&num_devices);
    if (!devices) {
        ErrorF("glamor: No GBM capable devices found, disabling GBM\n");
        return FALSE;
    }

    xwl_screen->egl_device = devices[0];
    free(devices);

    return TRUE;
}

Bool
xwl_glamor_init_gbm(struct xwl_screen *xwl_screen)
{
    struct xwl_gbm_private *xwl_gbm;

    if (!xwl_glamor_gbm_get_device(xwl_screen))
        return FALSE;

    xwl_gbm = calloc(sizeof(*xwl_gbm), 1);
    if (!xwl_gbm) {
        ErrorF("glamor: Not enough memory to setup GBM, disabling\n");
        return FALSE;
    }

    xwl_screen->egl_backend.priv = xwl_gbm;
    xwl_screen->egl_backend.init_wl_registry = xwl_glamor_gbm_init_wl_registry;
    xwl_screen->egl_backend.init_egl = xwl_glamor_gbm_init_egl;
    xwl_screen->egl_backend.init_screen = xwl_glamor_gbm_init_screen;
    xwl_screen->egl_backend.get_wl_buffer_for_pixmap = xwl_glamor_gbm_get_wl_buffer_for_pixmap;
    xwl_screen->egl_backend.post_damage = xwl_glamor_gbm_post_damage;

    ErrorF("glamor: Using GBM backend, just like the cool kids\n");

    return TRUE;
}
