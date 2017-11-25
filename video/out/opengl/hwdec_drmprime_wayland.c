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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include <libavutil/hwcontext_drm.h>

#include "common.h"
#include "video/hwdec.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "libmpv/opengl_cb.h"
#include "video/out/gpu/hwdec.h"
#include "video/mp_image.h"

#include "ra_gl.h"

struct priv {
    struct mp_log *log;
};

static int overlay_frame(struct ra_hwdec *hw, struct mp_image *hw_image,
                         struct mp_rect *src, struct mp_rect *dst, bool newframe)
{
    struct priv *p = hw->priv;
    GL *gl = ra_gl_get(hw->ra);
    AVDRMFrameDescriptor *desc = NULL;
    int ret;

    MP_VERBOSE(p, "overlay_frame (%p) (%p)\n", gl, desc);
    return 0;
    
 fail:
    return ret;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;

    MP_VERBOSE(p, "iunnit\n");
}

static int init(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;
    p->log = hw->log;

    MP_VERBOSE(p, "init\n");
    return 0;

err:
    uninit(hw);
    return -1;
}

const struct ra_hwdec_driver ra_hwdec_drmprime_wayland = {
    .name = "drmprime-wayland",
    .api = HWDEC_RKMPP,
    .priv_size = sizeof(struct priv),
    .imgfmts = {IMGFMT_DRMPRIME, 0},
    .init = init,
    .overlay_frame = overlay_frame,
    .uninit = uninit,
};
