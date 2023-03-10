/* GStreamer Wayland video sink
 *
 * Copyright (C) 2016 STMicroelectronics SA
 * Copyright (C) 2016 Fabien Dessenne <fabien.dessenne@st.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/allocators/gstdmabuf.h>

#include "wllinuxdmabuf.h"
#include "wlvideoformat.h"

GST_DEBUG_CATEGORY_EXTERN (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

typedef struct
{
  struct wl_buffer *wbuf;
} ConstructBufferData;

struct wl_buffer *
gst_wl_linux_dmabuf_construct_wl_buffer (GstBuffer * buf,
    GstWlDisplay * display, const GstVideoInfo * info)
{
  GstMemory *mem;
  int format;
  guint i, width, height;
  guint nplanes, flags = 0;
  gfloat stride_scale = 1.0f;
  struct zwp_linux_buffer_params_v1 *params;
  ConstructBufferData data;
  guint64 modifier = GST_VIDEO_INFO_IS_AFBC (info) ? DRM_AFBC_MODIFIER : 0;

  g_return_val_if_fail (gst_wl_display_check_format_for_dmabuf (display,
          GST_VIDEO_INFO_FORMAT (info)), NULL);

  mem = gst_buffer_peek_memory (buf, 0);
  format = gst_video_format_to_wl_dmabuf_format (GST_VIDEO_INFO_FORMAT (info));

  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);
  nplanes = GST_VIDEO_INFO_N_PLANES (info);

  GST_DEBUG_OBJECT (display, "Creating wl_buffer from DMABuf of size %"
      G_GSSIZE_FORMAT " (%d x %d), format %s", info->size, width, height,
      gst_wl_dmabuf_format_to_string (format));

  if (GST_VIDEO_INFO_IS_AFBC (info)) {
    /* Mali uses these formats instead */
    if (format == DRM_FORMAT_NV12) {
      format = DRM_FORMAT_YUV420_8BIT;
      nplanes = 1;
      stride_scale = 1.5;
    } else if (format == DRM_FORMAT_NV15) {
      format = DRM_FORMAT_YUV420_10BIT;
      nplanes = 1;
      stride_scale = 1.5;
    } else if (format == DRM_FORMAT_NV16) {
      format = DRM_FORMAT_YUYV;
      nplanes = 1;
      stride_scale = 2;
    } else {
      GST_ERROR_OBJECT (mem->allocator, "unsupported format for AFBC");
      data.wbuf = NULL;
      goto out;
    }
  }

  /* Creation and configuration of planes  */
  params = zwp_linux_dmabuf_v1_create_params (display->dmabuf);

  for (i = 0; i < nplanes; i++) {
    guint offset, stride, mem_idx, length;
    gsize skip;

    offset = GST_VIDEO_INFO_PLANE_OFFSET (info, i);
    stride = GST_VIDEO_INFO_PLANE_STRIDE (info, i);
    stride *= stride_scale;
    if (gst_buffer_find_memory (buf, offset, 1, &mem_idx, &length, &skip)) {
      GstMemory *m = gst_buffer_peek_memory (buf, mem_idx);
      gint fd = gst_dmabuf_memory_get_fd (m);
      zwp_linux_buffer_params_v1_add (params, fd, i, m->offset + skip,
          stride, modifier >> 32, modifier & 0xFFFFFFFF);
    } else {
      GST_ERROR_OBJECT (mem->allocator, "memory does not seem to contain "
          "enough data for the specified format");
      zwp_linux_buffer_params_v1_destroy (params);
      data.wbuf = NULL;
      goto out;
    }
  }

  if (GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_INTERLACED)) {
    GST_DEBUG_OBJECT (mem->allocator, "interlaced buffer");
    flags = ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED;

    if (!GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF)) {
      GST_DEBUG_OBJECT (mem->allocator, "with bottom field first");
      flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;
    }
  }

  data.wbuf =
      zwp_linux_buffer_params_v1_create_immed (params, width, height, format,
      flags);
  zwp_linux_buffer_params_v1_destroy (params);

out:
  if (!data.wbuf) {
    GST_ERROR_OBJECT (mem->allocator, "can't create linux-dmabuf buffer");
  } else {
    GST_DEBUG_OBJECT (mem->allocator, "created linux_dmabuf wl_buffer (%p):"
        "%dx%d, fmt=%.4s, %d planes",
        data.wbuf, width, height, (char *) &format, nplanes);
  }

  return data.wbuf;
}
