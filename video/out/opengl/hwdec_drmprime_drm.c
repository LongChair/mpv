/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <libavutil/hwcontext_drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "common.h"
#include "hwdec.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "video/out/drm_common.h"
#include "video/mp_image.h"

typedef struct drm_frame
{
    uint32_t fb_id;         // drm framebuffer handle
    uint32_t gem_handle;     // GEM handle
    struct mp_image *image; // associated mpv image

} drm_frame;

struct priv {
    struct mp_log *log;

    struct mp_image_params params;
  
    struct kms *kms;
    struct drm_frame current_frame, old_frame;

    int w, h;
    struct mp_rect src, dst;
};

static void remove_overlay(struct gl_hwdec *hw, int fb_id)
{
    struct priv *p = hw->priv;

    if (fb_id)
        drmModeRmFB(p->kms->fd, fb_id);
}

static void set_current_frame(struct gl_hwdec *hw, drm_frame *frame)
{
    struct priv *p = hw->priv;

    remove_overlay(hw, p->current_frame.fb_id);
    drmIoctl(p->kms->fd, DRM_IOCTL_GEM_CLOSE, &p->current_frame.gem_handle);
    mp_image_setrefp(&p->old_frame.image, p->current_frame.image);

    if (frame) {
        p->current_frame.fb_id = frame->fb_id;
        p->current_frame.gem_handle = frame->gem_handle;
        mp_image_setrefp(&p->current_frame.image, frame->image);
    } else {
        p->current_frame.fb_id = 0;
        p->current_frame.gem_handle = 0;
        mp_image_setrefp(&p->current_frame.image, NULL);
    }
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;

    p->params = *params;
    *params = (struct mp_image_params){0};

    set_current_frame(hw, NULL);
    return 0;
}


static int overlay_frame(struct gl_hwdec *hw, struct mp_image *hw_image)
{
    struct priv *p = hw->priv;
    AVDRMFrameDescriptor *desc = NULL;
    int ret;
    struct drm_frame next_frame = { 0, 0, NULL };

    if (hw_image) {
        desc = (AVDRMFrameDescriptor *)hw_image->planes[3];

        if (desc) {
            ret = drmPrimeFDToHandle(p->kms->fd, desc->fd[0], &next_frame.gem_handle);
            if (ret < 0) {
                MP_ERR(p, "Failed to retrieve the Prime Handle.\n");
                goto err;
            }

            uint32_t pitches[4] = { desc->pitch[0],
                                    desc->pitch[1],
                                    0, 0};

            uint32_t offsets[4] = { desc->offset[0],
                                    desc->offset[1],
                                    0, 0};

            uint32_t handles[4] = { next_frame.gem_handle,
                                    next_frame.gem_handle,
                                    0, 0};

            int srcw = p->src.x1 - p->src.x0;
            int srch = p->src.y1 - p->src.y0;
            int dstw = MP_ALIGN_UP(p->dst.x1 - p->dst.x0, 16);
            int dsth = MP_ALIGN_UP(p->dst.y1 - p->dst.y0, 16);


            ret = drmModeAddFB2(p->kms->fd, hw_image->w, hw_image->h, desc->format,
                                handles, pitches, offsets, &next_frame.fb_id, 0);

            if (ret < 0) {
                MP_ERR(p, "Failed to add drm layer %d.\n", next_frame.fb_id);
                goto err;
            }

            ret = drmModeSetPlane(p->kms->fd, p->kms->plane_id, p->kms->crtc_id, next_frame.fb_id, 0,
                                  MP_ALIGN_UP(p->dst.x0, 2), MP_ALIGN_UP(p->dst.y0, 2), dstw, dsth,
                                  p->src.x0 << 16, p->src.y0 << 16 , srcw << 16, srch << 16);
            if (ret < 0) {
                MP_ERR(p, "Failed to set the plane %d (buffer %d).\n", p->kms->plane_id,
                            next_frame.fb_id);
                goto err;
            }

            next_frame.image = hw_image;
        }
    }

    set_current_frame(hw, &next_frame);

    return 0;
    
 err:
    if (next_frame.gem_handle)
        drmIoctl(p->kms->fd, DRM_IOCTL_GEM_CLOSE, &next_frame.gem_handle);

    if (next_frame.fb_id)
         remove_overlay(hw, next_frame.fb_id);

    return ret;
}

static void overlay_adjust(struct gl_hwdec *hw, int w, int h,
                           struct mp_rect *src, struct mp_rect *dst)
{
    struct priv *p = hw->priv;
    drmModeCrtcPtr crtc;
    double hratio, vratio;

    p->w = w;
    p->h = h;
    p->src = *src;
    p->dst = *dst;

    // drm can allow to have a layer that has a different size from framebuffer
    // we scale here the destination size to video mode
    hratio = vratio = 1.0;
    crtc = drmModeGetCrtc(p->kms->fd, p->kms->crtc_id);
    if (crtc) {
        hratio = ((double)crtc->mode.hdisplay / (double)(p->dst.x1 - p->dst.x0));
        vratio = ((double)crtc->mode.vdisplay / (double)(p->dst.y1 - p->dst.y0));
        drmModeFreeCrtc(crtc);
    }

    p->dst.x0 *= hratio;
    p->dst.x1 *= hratio;
    p->dst.y0 *= vratio;
    p->dst.y1 *= vratio;

    overlay_frame(hw, p->current_frame.image);
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;

    set_current_frame(hw, NULL);

    if (p->kms) {
        kms_destroy(p->kms);
        p->kms = NULL;
    }
}

static int create(struct gl_hwdec *hw)
{
    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;
    p->log = hw->log;

    char *connector_spec;
    int drm_mode, drm_layer;

    mp_read_option_raw(hw->global, "drm-connector", &m_option_type_string, &connector_spec);
    mp_read_option_raw(hw->global, "drm-mode", &m_option_type_int, &drm_mode);
    mp_read_option_raw(hw->global, "drm-layer", &m_option_type_int, &drm_layer);

    talloc_free(connector_spec);

    p->kms = kms_create(hw->log, connector_spec, drm_mode, drm_layer);
    if (!p->kms) {
        MP_ERR(p, "Failed to create KMS.\n");
        goto err;
    }

    uint64_t has_prime;
    if (drmGetCap(p->kms->fd, DRM_CAP_PRIME, &has_prime) < 0) {
        MP_ERR(p, "Card \"%d\" does not support prime handles.\n",
               p->kms->card_no);
        goto err;
    }

    return 0;

err:
    destroy(hw);
    return -1;
}

static bool test_format(struct gl_hwdec *hw, int imgfmt)
{
    return imgfmt == IMGFMT_DRM;
}

const struct gl_hwdec_driver gl_hwdec_drmprime_drm = {
    .name = "drm-drm",
    .api = HWDEC_RKMPP,
    .test_format = test_format,
    .create = create,
    .reinit = reinit,
    .overlay_frame = overlay_frame,
    .overlay_adjust = overlay_adjust,
    .destroy = destroy,
};
