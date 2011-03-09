/*
 * INTEL CONFIDENTIAL
 * Copyright 2007 Intel Corporation. All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its suppliers
 * or licensors. Title to the Material remains with Intel Corporation or its
 * suppliers and licensors. The Material may contain trade secrets and
 * proprietary and confidential information of Intel Corporation and its
 * suppliers and licensors, and is protected by worldwide copyright and trade
 * secret laws and treaty provisions. No part of the Material may be used,
 * copied, reproduced, modified, published, uploaded, posted, transmitted,
 * distributed, or disclosed in any way without Intel's prior express written
 * permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 */

#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>
#include <va/va_backend.h>
#include "psb_output.h"
#include "psb_surface.h"
#include "psb_buffer.h"
#include "psb_x11.h"
#include "psb_surface_ext.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "psb_surface_ext.h"
#include <wsbm/wsbm_manager.h>

#define INIT_DRIVER_DATA    psb_driver_data_p driver_data = (psb_driver_data_p) ctx->pDriverData
#define INIT_OUTPUT_PRIV    psb_x11_output_p output = (psb_x11_output_p)(((psb_driver_data_p)ctx->pDriverData)->ws_priv)

#define SURFACE(id)	((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))
#define IMAGE(id)  ((object_image_p) object_heap_lookup( &driver_data->image_heap, id ))
#define SUBPIC(id)  ((object_subpic_p) object_heap_lookup( &driver_data->subpic_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))


//X error trap
static int x11_error_code = 0;
static int (*old_error_handler)(Display *, XErrorEvent *);

static struct timeval inter_period = {0};
static void psb_doframerate(int fps)
{
    struct timeval time_deta;

    inter_period.tv_usec += 1000000 / fps;

    /*recording how long it passed*/
    if (inter_period.tv_usec >= 1000000) {
        inter_period.tv_usec -= 1000000;
        inter_period.tv_sec++;
    }

    gettimeofday(&time_deta, (struct timezone *)NULL);

    time_deta.tv_usec = inter_period.tv_usec - time_deta.tv_usec;
    time_deta.tv_sec  = inter_period.tv_sec  - time_deta.tv_sec;

    if (time_deta.tv_usec < 0) {
        time_deta.tv_usec += 1000000;
        time_deta.tv_sec--;
    }

    if (time_deta.tv_sec < 0 || (time_deta.tv_sec == 0 && time_deta.tv_usec <= 0))
        return;

    select(0, NULL, NULL, NULL, &time_deta);
}

static uint32_t mask2shift(uint32_t mask)
{
    uint32_t shift = 0;
    while ((mask & 0x1) == 0) {
        mask = mask >> 1;
        shift++;
    }
    return shift;
}

static VAStatus psb_putsurface_x11(
    VADriverContextP ctx,
    VASurfaceID surface,
    Drawable draw, /* X Drawable */
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_DRIVER_DATA;
    GC gc;
    XImage *ximg = NULL;
    Visual *visual;
    unsigned short width, height;
    int depth;
    int x = 0, y = 0;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    void *surface_data = NULL;
    int ret;

    uint32_t rmask = 0;
    uint32_t gmask = 0;
    uint32_t bmask = 0;

    uint32_t rshift = 0;
    uint32_t gshift = 0;
    uint32_t bshift = 0;


    if (srcw <= destw)
        width = srcw;
    else
        width = destw;

    if (srch <= desth)
        height = srch;
    else
        height = desth;

    object_surface_p obj_surface = SURFACE(surface);
    if (NULL == obj_surface) {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    psb_surface_p psb_surface = obj_surface->psb_surface;

    psb__information_message("PutSurface: src	  w x h = %d x %d\n", srcw, srch);
    psb__information_message("PutSurface: dest 	  w x h = %d x %d\n", destw, desth);
    psb__information_message("PutSurface: clipped w x h = %d x %d\n", width, height);

    visual = DefaultVisual((Display *)ctx->native_dpy, ctx->x11_screen);
    gc = XCreateGC((Display *)ctx->native_dpy, draw, 0, NULL);
    depth = DefaultDepth((Display *)ctx->native_dpy, ctx->x11_screen);

    if (TrueColor != visual->class) {
        psb__error_message("PutSurface: Default visual of X display must be TrueColor.\n");
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    ret = psb_buffer_map(&psb_surface->buf, &surface_data);
    if (ret) {
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    rmask = visual->red_mask;
    gmask = visual->green_mask;
    bmask = visual->blue_mask;

    rshift = mask2shift(rmask);
    gshift = mask2shift(gmask);
    bshift = mask2shift(bmask);

    psb__information_message("PutSurface: Pixel masks: R = %08x G = %08x B = %08x\n", rmask, gmask, bmask);
    psb__information_message("PutSurface: Pixel shifts: R = %d G = %d B = %d\n", rshift, gshift, bshift);

    ximg = XCreateImage((Display *)ctx->native_dpy, visual, depth, ZPixmap, 0, NULL, width, height, 32, 0);

    if (ximg->byte_order == MSBFirst)
        psb__information_message("PutSurface: XImage pixels has MSBFirst, %d bits / pixel\n", ximg->bits_per_pixel);
    else
        psb__information_message("PutSurface: XImage pixels has LSBFirst, %d bits / pixel\n", ximg->bits_per_pixel);

    if (ximg->bits_per_pixel != 32) {
        psb__error_message("PutSurface: Display uses %d bits/pixel which is not supported\n");
        vaStatus = VA_STATUS_ERROR_UNKNOWN;
        goto out;
    }

    void yuv2pixel(uint32_t *pixel, int y, int u, int v) {
        int r, g, b;
        /* Warning, magic values ahead */
        r = y + ((351 * (v - 128)) >> 8);
        g = y - (((179 * (v - 128)) + (86 * (u - 128))) >> 8);
        b = y + ((444 * (u - 128)) >> 8);

        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;

        *pixel = ((r << rshift) & rmask) | ((g << gshift) & gmask) | ((b << bshift) & bmask);
    }
    ximg->data = (char *) malloc(ximg->bytes_per_line * height);
    if (NULL == ximg->data) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        goto out;
    }

    uint8_t *src_y = surface_data + psb_surface->stride * srcy;
    uint8_t *src_uv = surface_data + psb_surface->stride * (obj_surface->height + srcy / 2);

    for (y = srcy; y < (srcy + height); y += 2) {
        uint32_t *dest_even = (uint32_t *)(ximg->data + y * ximg->bytes_per_line);
        uint32_t *dest_odd = (uint32_t *)(ximg->data + (y + 1) * ximg->bytes_per_line);
        for (x = srcx; x < (srcx + width); x += 2) {
            /* Y1 Y2 */
            /* Y3 Y4 */
            int y1 = *(src_y + x);
            int y2 = *(src_y + x + 1);
            int y3 = *(src_y + x + psb_surface->stride);
            int y4 = *(src_y + x + psb_surface->stride + 1);

            /* U V */
            int u = *(src_uv + x);
            int v = *(src_uv + x + 1);

            yuv2pixel(dest_even++, y1, u, v);
            yuv2pixel(dest_even++, y2, u, v);

            yuv2pixel(dest_odd++, y3, u, v);
            yuv2pixel(dest_odd++, y4, u, v);
        }
        src_y += psb_surface->stride * 2;
        src_uv += psb_surface->stride;
    }

    XPutImage((Display *)ctx->native_dpy, draw, gc, ximg, 0, 0, destx, desty, width, height);
    XFlush((Display *)ctx->native_dpy);

out:
    if (NULL != ximg)
        XDestroyImage(ximg);
    if (NULL != surface_data)
        psb_buffer_unmap(&psb_surface->buf);

    XFreeGC((Display *)ctx->native_dpy, gc);

    return vaStatus;
}

void *psb_x11_output_init(VADriverContextP ctx)
{
    INIT_DRIVER_DATA;
    psb_x11_output_p output = calloc(1, sizeof(psb_x11_output_s));

    if (output == NULL) {
        psb__error_message("Can't malloc memory\n");
        return NULL;
    }

    if (getenv("PSB_VIDEO_PUTSURFACE_X11")) {
        psb__information_message("Putsurface force to SW rendering\n");
        driver_data->output_method = PSB_PUTSURFACE_X11;

        return output;
    }

    psb_init_xvideo(ctx, output);

    driver_data->color_key = 0x11;
    if (IS_MFLD(driver_data) && /* force MFLD to use COVERLAY */
            (driver_data->output_method == PSB_PUTSURFACE_OVERLAY)) {
        psb__information_message("Use client overlay mode for post-processing\n");

        driver_data->coverlay = 1;
        driver_data->output_method = PSB_PUTSURFACE_COVERLAY;
    }

    /* always init CTEXTURE on MRST and MDFLD*/
    driver_data->ctexture = 1;

    if (getenv("PSB_VIDEO_TEXTURE") && output->textured_portID) {
        psb__information_message("Putsurface force to use Textured Xvideo\n");
        driver_data->output_method = PSB_PUTSURFACE_FORCE_TEXTURE;
    }

    if (getenv("PSB_VIDEO_OVERLAY") && output->overlay_portID) {
        psb__information_message("Putsurface force to use Overlay Xvideo\n");
        driver_data->output_method = PSB_PUTSURFACE_FORCE_OVERLAY;
    }

    if (getenv("PSB_VIDEO_CTEXTURE")) {
        psb__information_message("Putsurface force to use Client Texture\n");
        driver_data->output_method = PSB_PUTSURFACE_FORCE_CTEXTURE;
    }

    if (getenv("PSB_VIDEO_COVERLAY")) {
        psb__information_message("Putsurface force to use Client Overlay\n");

        driver_data->coverlay = 1;
        driver_data->output_method = PSB_PUTSURFACE_FORCE_COVERLAY;
    }

    return output;
}

static int
error_handler(Display *dpy, XErrorEvent *error)
{
    x11_error_code = error->error_code;
    return 0;
}

void psb_x11_output_deinit(VADriverContextP ctx)
{
    psb_deinit_xvideo(ctx);
}

static void
x11_trap_errors(void)
{
    x11_error_code = 0;
    old_error_handler = XSetErrorHandler(error_handler);
}

static int
x11_untrap_errors(void)
{
    XSetErrorHandler(old_error_handler);
    return x11_error_code;
}

static int
is_window(Display *dpy, Drawable drawable)
{
    XWindowAttributes wattr;

    x11_trap_errors();
    XGetWindowAttributes(dpy, drawable, &wattr);
    return x11_untrap_errors() == 0;
}

static int pnw_check_output_method(VADriverContextP ctx, object_surface_p obj_surface, int width, int height, int destw, int desth, Drawable draw)
{
    INIT_DRIVER_DATA;
    INIT_OUTPUT_PRIV;

    if (driver_data->output_method == PSB_PUTSURFACE_FORCE_TEXTURE ||
            driver_data->output_method == PSB_PUTSURFACE_FORCE_OVERLAY ||
            driver_data->output_method == PSB_PUTSURFACE_FORCE_CTEXTURE ||
            driver_data->output_method == PSB_PUTSURFACE_FORCE_COVERLAY) {
        psb__information_message("Force to use %08x for PutSurface\n", driver_data->output_method);
        return 0;
    }

    /* Avoid call is_window()/XGetWindowAttributes() every frame */
    if (output->output_drawable_save != draw) {
	output->output_drawable_save = draw;
	if (!is_window(ctx->native_dpy, draw))
	    output->is_pixmap = 1;
	else
	    output->is_pixmap = 0;
    }

    if (output->is_pixmap == 1 || (IS_MRST(driver_data) && obj_surface->subpic_count > 0) || width >= 2048 || height >= 2048 ||
            /*FIXME: overlay path can't handle subpicture scaling. when surface size > dest box, fallback to texblit.*/
            (IS_MFLD(driver_data) && obj_surface->subpic_count && ((width > destw) || (height > desth)))) {
        psb__information_message("Putsurface fall back to use Client Texture\n");

        driver_data->output_method = PSB_PUTSURFACE_CTEXTURE;
    }

    return 0;
}

VAStatus psb_PutSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    void *drawable, /* X Drawable */
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects, /* client supplied clip list */
    unsigned int number_cliprects, /* number of clip rects in the clip list */
    unsigned int flags /* de-interlacing flags */
)
{
    INIT_DRIVER_DATA;
    object_surface_p obj_surface;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Drawable draw = (Drawable)drawable;
    obj_surface = SURFACE(surface);

    if (NULL == obj_surface) {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    if (driver_data->dummy_putsurface) {
        psb__information_message("vaPutSurface: dummy mode, return directly\n");
        return VA_STATUS_SUCCESS;
    }

    if (driver_data->output_method == PSB_PUTSURFACE_X11) {
        psb_putsurface_x11(ctx, surface, draw, srcx, srcy, srcw, srch,
                           destx, desty, destw, desth, flags);
        return VA_STATUS_SUCCESS;
    }

    if (driver_data->fixed_fps > 0) {
        if ((inter_period.tv_sec == 0) && (inter_period.tv_usec == 0))
            gettimeofday(&inter_period, (struct timezone *)NULL);

        psb_doframerate(driver_data->fixed_fps);
    }

    pnw_check_output_method(ctx, obj_surface, srcw, srch, destw, desth, draw);

    pthread_mutex_lock(&driver_data->output_mutex);

    if ((driver_data->output_method == PSB_PUTSURFACE_CTEXTURE) ||
            (driver_data->output_method == PSB_PUTSURFACE_FORCE_CTEXTURE)) {
        psb__information_message("Using client Texture for PutSurface\n");
        psb_putsurface_ctexture(ctx, surface, draw,
                                srcx, srcy, srcw, srch,
                                destx, desty, destw, desth,
                                flags);
    } else if ((driver_data->output_method == PSB_PUTSURFACE_COVERLAY) ||
               (driver_data->output_method == PSB_PUTSURFACE_FORCE_COVERLAY)) {
        psb__information_message("Using client Overlay for PutSurface\n");

        srcw = srcw <= 1920 ? srcw : 1920;

        psb_putsurface_coverlay(
            ctx, surface, draw,
            srcx, srcy, srcw, srch,
            destx, desty, destw, desth,
            cliprects, number_cliprects, flags);
    } else
        psb_putsurface_xvideo(
            ctx, surface, draw,
            srcx, srcy, srcw, srch,
            destx, desty, destw, desth,
            cliprects, number_cliprects, flags);
    pthread_mutex_unlock(&driver_data->output_mutex);

    driver_data->frame_count++;

    return VA_STATUS_SUCCESS;
}
