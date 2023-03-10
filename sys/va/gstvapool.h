/* GStreamer
 * Copyright (C) 2020 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_VA_POOL (gst_va_pool_get_type())
G_DECLARE_FINAL_TYPE (GstVaPool, gst_va_pool, GST, VA_POOL, GstBufferPool)

GstBufferPool *      gst_va_pool_new                      (void);
gboolean             gst_va_pool_requires_video_meta      (GstBufferPool * pool);
void                 gst_buffer_pool_config_set_va_allocation_params (GstStructure * config,
                                                                      guint usage_hint);

void                 gst_buffer_pool_config_set_va_alignment (GstStructure * config,
                                                              const GstVideoAlignment * align);

GstBufferPool *      gst_va_pool_new_with_config          (GstCaps * caps,
                                                           guint size,
                                                           guint min_buffers,
                                                           guint max_buffers,
                                                           guint usage_hint,
                                                           GstAllocator * allocator,
                                                           GstAllocationParams * alloc_params);

G_END_DECLS
