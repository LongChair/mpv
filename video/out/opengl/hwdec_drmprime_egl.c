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

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "hwdec.h"
#include "libavcodec/drmprime.h"

#ifndef GL_OES_EGL_image
typedef void* GLeglImageOES;
#endif

#define MAX_NUM_PLANES  4

static const EGLint egl_dmabuf_plane_fd_attr[MAX_NUM_PLANES] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
};
static const EGLint egl_dmabuf_plane_offset_attr[MAX_NUM_PLANES] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
static const EGLint egl_dmabuf_plane_pitch_attr[MAX_NUM_PLANES] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
};

struct priv {
    struct mp_log *log;

    GLuint gl_texture;
    EGLImageKHR image;

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(EGLDisplay, EGLContext,
                                              EGLenum, EGLClientBuffer,
                                              const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static void unmap_frame(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;

    if (p->image)
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->image);
    p->image = 0;
}

static void destroy_textures(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;
}

static void destroy(struct gl_hwdec *hw)
{
    unmap_frame(hw);
    destroy_textures(hw);
}

static int create(struct gl_hwdec *hw)
{
//    GL *gl = hw->gl;

    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;

    p->log = hw->log;

    if (!eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!exts)
        return -1;

// TODO fix this when mali driver returns the proper extension list
//    if (!strstr(exts, "EXT_image_dma_buf_import") ||
//        !strstr(exts, "EGL_KHR_image_base") ||
//        !strstr(gl->extensions, "GL_OES_EGL_image"))
//        return -1;

    static const char *gles_exts[] = {"GL_OES_EGL_image_external", 0};
    hw->glsl_extensions = gles_exts;

    // EGL_KHR_image_base
    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    // GL_OES_EGL_image
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->EGLImageTargetTexture2DOES)
        return -1;

    MP_VERBOSE(p, "using RKMPP EGL interop\n");

    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    params->imgfmt = IMGFMT_RGB0;

    // Recreate them to get rid of all previous image data (possibly).
    destroy_textures(hw);

    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    return 0;
}

#define ADD_ATTRIB(name, value)                         \
    do {                                                \
    assert(num_attribs + 3 < MP_ARRAY_SIZE(attribs));   \
    attribs[num_attribs++] = (name);                    \
    attribs[num_attribs++] = (value);                   \
    attribs[num_attribs] = EGL_NONE;                    \
    } while(0)

static int map_frame(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    av_drmprime *primedata = (av_drmprime *)hw_image->planes[3];

    if (!primedata)
        goto err;

    unmap_frame(hw);

    int attribs[26] = {EGL_NONE};
    int num_attribs = 0;

    ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, primedata->format);
    ADD_ATTRIB(EGL_WIDTH, hw_image->w);
    ADD_ATTRIB(EGL_HEIGHT, hw_image->h);
    for (int i=0; i < AV_DRMPRIME_NUM_PLANES; i++) {
        if (primedata->fds[0]) {
            ADD_ATTRIB(egl_dmabuf_plane_fd_attr[i], primedata->fds[i]);
            ADD_ATTRIB(egl_dmabuf_plane_offset_attr[i], primedata->offsets[i]);
            ADD_ATTRIB(egl_dmabuf_plane_pitch_attr[i], primedata->strides[i]);
        }

    }

    ADD_ATTRIB(EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT);
    ADD_ATTRIB(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);

    p->image = p->CreateImageKHR(eglGetCurrentDisplay(),
            EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (p->image == EGL_NO_IMAGE_KHR)
        goto err;

    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->image);


    out_frame->planes[0] = (struct gl_hwdec_plane){
            .gl_texture = p->gl_texture,
            .gl_target = GL_TEXTURE_EXTERNAL_OES,
            .tex_w =  primedata->strides[0],
            .tex_h =  hw_image->h,
        };

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    return 0;

err:
    unmap_frame(hw);
    return -1;
}

const struct gl_hwdec_driver gl_hwdec_drmprime_egl = {
    .name = "drmprime-egl",
    .api = HWDEC_RKMPP,
    .imgfmt = IMGFMT_DRMPRIME,
    .create = create,
    .reinit = reinit,
    .map_frame = map_frame,
    .unmap = unmap_frame,
    .destroy = destroy,
};
