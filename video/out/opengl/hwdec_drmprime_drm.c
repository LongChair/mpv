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
#include "video/hwdec.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "video/out/drm_common.h"
#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"

#include "ra_gl.h"

typedef struct drm_frame
{
    uint32_t fb_id;         // drm framebuffer handle
    uint32_t gem_handles[AV_DRM_MAX_PLANES];     // GEM handle
    struct mp_image *image; // associated mpv image

} drm_frame;

struct priv {
    struct mp_image_params params;
  
    struct kms *kms;
    struct drm_frame current_frame, old_frame;

    struct mp_rect src, dst;
};

static void remove_overlay(struct ra_hwdec *hw, int fb_id)
{
    struct priv *p = hw->priv;

    if (fb_id)
        drmModeRmFB(p->kms->fd, fb_id);
}

static void set_current_frame(struct ra_hwdec *hw, drm_frame *frame)
{
    struct priv *p = hw->priv;
    int i;
    remove_overlay(hw, p->current_frame.fb_id);

    for (i = 0; i < AV_DRM_MAX_PLANES; i++)
        if (p->current_frame.gem_handles[i])
            drmIoctl(p->kms->fd, DRM_IOCTL_GEM_CLOSE, &p->current_frame.gem_handles[i]);
    mp_image_setrefp(&p->old_frame.image, p->current_frame.image);

    if (frame) {
        p->current_frame.fb_id = frame->fb_id;
        for (i = 0; i < AV_DRM_MAX_PLANES; i++)
            p->current_frame.gem_handles[i] = frame->gem_handles[i];
        mp_image_setrefp(&p->current_frame.image, frame->image);
    } else {
        p->current_frame.fb_id = 0;
        for (i = 0; i < AV_DRM_MAX_PLANES; i++)
            p->current_frame.gem_handles[i] = 0;
        mp_image_setrefp(&p->current_frame.image, NULL);
    }
}

static void scale_dst_rect(struct ra_hwdec *hw, int source_w, int source_h ,struct mp_rect *src, struct mp_rect *dst)
{
    struct priv *p = hw->priv;
    drmModeCrtcPtr crtc;
    double hratio, vratio, ratio;
    int display_w = 0, display_h = 0;

    // drm can allow to have a layer that has a different size from framebuffer
    // we scale here the destination size to video mode
    hratio = vratio = ratio = 1.0;

    crtc = drmModeGetCrtc(p->kms->fd, p->kms->crtc_id);
    if (crtc) {
        display_w = crtc->mode.hdisplay;
        display_h = crtc->mode.vdisplay;
        drmModeFreeCrtc(crtc);
    }

    hratio = (double)display_w / (double)source_w;
    vratio = (double)display_h / (double)source_h;
    ratio = hratio <= vratio ? hratio : vratio;

    dst->x0 = src->x0 * ratio;
    dst->x1 = src->x1 * ratio;
    dst->y0 = src->y0 * ratio;
    dst->y1 = src->y1 * ratio;

    int offset_x = (display_w - ratio * source_w) / 2;
    int offset_y = (display_h - ratio * source_h) / 2;

    dst->x0 += offset_x;
    dst->x1 += offset_x;
    dst->y0 += offset_y;
    dst->y1 += offset_y;
}

static int overlay_frame(struct ra_hwdec *hw, struct mp_image *hw_image,
                         struct mp_rect *src, struct mp_rect *dst, bool newframe)
{
    struct priv *p = hw->priv;
    GL *gl = ra_gl_get(hw->ra);
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    int ret, fd;
    struct drm_frame next_frame;

    memset(&next_frame, 0, sizeof(drm_frame));

    if (hw_image) {
        p->src = *src;

        int *data = gl ? mpgl_get_native_display(gl, "opengl-cb") : NULL;
        if (data)
            scale_dst_rect(hw, data[2], data[3], dst, &p->dst);
        else
            p->dst = *dst;

        desc = (AVDRMFrameDescriptor *)hw_image->planes[0];

        uint32_t pitches[4], offsets[4];
        uint32_t handles[4] = { 0, 0 ,0, 0 };

        if ((desc) && (desc->nb_layers)) {

            for (int object=0; object < desc->nb_objects; object++) {
                ret = drmPrimeFDToHandle(p->kms->fd, desc->objects[object].fd, &next_frame.gem_handles[object]);
                if (ret < 0) {
                    MP_ERR(hw, "Failed to retrieve the Prime Handle from handle %d (%d).\n", object, desc->objects[object].fd);
                    goto err;
                }
            }

            for (int l=0; l < desc->nb_layers; l++) {
                layer = &desc->layers[l];

                for (int plane = 0; plane < AV_DRM_MAX_PLANES; plane++) {
                    fd = next_frame.gem_handles[layer->planes[plane].object_index];
                    if (fd) {
                        pitches[plane] = layer->planes[plane].pitch;
                        offsets[plane] = layer->planes[plane].offset;
                        handles[plane] = next_frame.gem_handles[layer->planes[plane].object_index];
                    }
                    else {
                        pitches[plane] = 0;
                        offsets[plane] = 0;
                        handles[plane] = 0;
                    }
                }

                int srcw = p->src.x1 - p->src.x0;
                int srch = p->src.y1 - p->src.y0;
                int dstw = MP_ALIGN_UP(p->dst.x1 - p->dst.x0, 2);
                int dsth = MP_ALIGN_UP(p->dst.y1 - p->dst.y0, 2);

                ret = drmModeAddFB2(p->kms->fd, hw_image->w, hw_image->h, layer->format,
                                    handles, pitches, offsets, &next_frame.fb_id, 0);

                if (ret < 0) {
                    MP_ERR(hw, "Failed to add drm layer %d.\n", next_frame.fb_id);
                    goto err;
                }

                ret = drmModeSetPlane(p->kms->fd, p->kms->plane_id, p->kms->crtc_id, next_frame.fb_id, 0,
                                      MP_ALIGN_DOWN(p->dst.x0, 2), MP_ALIGN_DOWN(p->dst.y0, 2), dstw, dsth,
                                      p->src.x0 << 16, p->src.y0 << 16 , srcw << 16, srch << 16);
                if (ret < 0) {
                    MP_ERR(hw, "Failed to set the plane %d (buffer %d).\n", p->kms->plane_id,
                                next_frame.fb_id);
                    goto err;
                }

                next_frame.image = hw_image;
            }
        }
    }

    set_current_frame(hw, &next_frame);
    return 0;
    
 err:
    for (int i = 0; i < AV_DRM_MAX_PLANES; i++)
        if (next_frame.gem_handles[i])
            drmIoctl(p->kms->fd, DRM_IOCTL_GEM_CLOSE, &next_frame.gem_handles[i]);

    if (next_frame.fb_id)
         remove_overlay(hw, next_frame.fb_id);

    return ret;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;

    set_current_frame(hw, NULL);

    if (p->kms) {
        kms_destroy(p->kms);
        p->kms = NULL;
    }
}

static int init(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;
    char *connector_spec;
    int drm_mode, drm_layer;

    mp_read_option_raw(hw->global, "drm-connector", &m_option_type_string, &connector_spec);
    mp_read_option_raw(hw->global, "drm-mode", &m_option_type_int, &drm_mode);
    mp_read_option_raw(hw->global, "drm-layer", &m_option_type_int, &drm_layer);

    talloc_free(connector_spec);

    p->kms = kms_create(hw->log, connector_spec, drm_mode, drm_layer);
    if (!p->kms) {
        MP_ERR(hw, "Failed to create KMS.\n");
        goto err;
    }

    uint64_t has_prime;
    if (drmGetCap(p->kms->fd, DRM_CAP_PRIME, &has_prime) < 0) {
        MP_ERR(hw, "Card \"%d\" does not support prime handles.\n",
               p->kms->card_no);
        goto err;
    }

    return 0;

err:
    uninit(hw);
    return -1;
}

const struct ra_hwdec_driver ra_hwdec_drmprime_drm = {
    .name = "drmprime-drm",
    .api = HWDEC_RKMPP,
    .priv_size = sizeof(struct priv),
    .imgfmts = {IMGFMT_DRMPRIME, 0},
    .init = init,
    .overlay_frame = overlay_frame,
    .uninit = uninit,
};
