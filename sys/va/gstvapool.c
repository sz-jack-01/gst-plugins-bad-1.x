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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvapool.h"

#include "gstvaallocator.h"
#include "gstvacaps.h"

GST_DEBUG_CATEGORY_STATIC (gst_va_pool_debug);
#define GST_CAT_DEFAULT gst_va_pool_debug

struct _GstVaPool
{
  GstBufferPool parent;

  GstVideoInfo alloc_info;
  GstVideoInfo caps_info;
  GstAllocator *allocator;
  gboolean force_videometa;
  gboolean add_videometa;
  gint crop_left;
  gint crop_top;

  gboolean starting;
};

#define gst_va_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVaPool, gst_va_pool, GST_TYPE_BUFFER_POOL,
    GST_DEBUG_CATEGORY_INIT (gst_va_pool_debug, "vapool", 0, "VA Pool"));

static const gchar **
gst_va_pool_get_options (GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  return options;
}

static inline gboolean
gst_buffer_pool_config_get_va_allocation_params (GstStructure * config,
    guint32 * usage_hint)
{
  if (!gst_structure_get (config, "usage-hint", G_TYPE_UINT, usage_hint, NULL))
    *usage_hint = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;

  return TRUE;
}

static inline gboolean
gst_buffer_pool_config_get_va_alignment (GstStructure * config,
    GstVideoAlignment * align)
{
  return gst_structure_get (config,
      "va-padding-top", G_TYPE_UINT, &align->padding_top,
      "va-padding-bottom", G_TYPE_UINT, &align->padding_bottom,
      "va-padding-left", G_TYPE_UINT, &align->padding_left,
      "va-padding-right", G_TYPE_UINT, &align->padding_right, NULL);
}

static gboolean
gst_va_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstAllocator *allocator;
  GstCaps *caps;
  GstVaPool *vpool = GST_VA_POOL (pool);
  GstVideoAlignment video_align = { 0, };
  GstVideoInfo caps_info, alloc_info;
  gint width, height;
  guint i, min_buffers, max_buffers;
  guint32 usage_hint;
  gboolean has_alignment;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, &min_buffers,
          &max_buffers))
    goto wrong_config;

  if (!caps)
    goto no_caps;

  if (!gst_video_info_from_caps (&caps_info, caps))
    goto wrong_caps;

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, NULL))
    goto wrong_config;

  if (!(allocator && (GST_IS_VA_DMABUF_ALLOCATOR (allocator)
              || GST_IS_VA_ALLOCATOR (allocator))))
    goto wrong_config;

  if (!gst_buffer_pool_config_get_va_allocation_params (config, &usage_hint))
    goto wrong_config;

  width = GST_VIDEO_INFO_WIDTH (&caps_info);
  height = GST_VIDEO_INFO_HEIGHT (&caps_info);

  GST_LOG_OBJECT (vpool, "%dx%d | %" GST_PTR_FORMAT, width, height, caps);

  /* enable metadata based on config of the pool */
  vpool->add_videometa = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  /* parse extra alignment info */
  has_alignment = gst_buffer_pool_config_get_va_alignment (config,
      &video_align);

  if (has_alignment) {
    width += video_align.padding_left + video_align.padding_right;
    height += video_align.padding_bottom + video_align.padding_top;

    if (video_align.padding_left > 0)
      vpool->crop_left = video_align.padding_left;
    if (video_align.padding_top > 0)
      vpool->crop_top = video_align.padding_top;
  }

  /* update allocation info with aligned size */
  alloc_info = caps_info;
  GST_VIDEO_INFO_WIDTH (&alloc_info) = width;
  GST_VIDEO_INFO_HEIGHT (&alloc_info) = height;

  if (GST_IS_VA_DMABUF_ALLOCATOR (allocator)) {
    if (!gst_va_dmabuf_allocator_set_format (allocator, &alloc_info,
            usage_hint))
      goto failed_allocator;
  } else if (GST_IS_VA_ALLOCATOR (allocator)) {
    if (!gst_va_allocator_set_format (allocator, &alloc_info, usage_hint))
      goto failed_allocator;
  }

  gst_object_replace ((GstObject **) & vpool->allocator,
      GST_OBJECT (allocator));

  vpool->caps_info = caps_info;
  vpool->alloc_info = alloc_info;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (&caps_info); i++) {
    if (GST_VIDEO_INFO_PLANE_STRIDE (&caps_info, i) !=
        GST_VIDEO_INFO_PLANE_STRIDE (&alloc_info, i) ||
        GST_VIDEO_INFO_PLANE_OFFSET (&caps_info, i) !=
        GST_VIDEO_INFO_PLANE_OFFSET (&alloc_info, i)) {
      GST_INFO_OBJECT (vpool, "Video meta is required in buffer.");
      vpool->force_videometa = TRUE;
      break;
    }
  }

  gst_buffer_pool_config_set_params (config, caps,
      GST_VIDEO_INFO_SIZE (&alloc_info), min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);

  /* ERRORS */
wrong_config:
  {
    GST_WARNING_OBJECT (vpool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_WARNING_OBJECT (vpool, "no caps in config");
    return FALSE;
  }
wrong_caps:
  {
    GST_WARNING_OBJECT (vpool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
failed_allocator:
  {
    GST_WARNING_OBJECT (vpool, "Failed to set format to allocator");
    return FALSE;
  }
}

static GstFlowReturn
gst_va_pool_alloc (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  GstBuffer *buf;
  GstVaPool *vpool = GST_VA_POOL (pool);

  buf = gst_buffer_new ();

  if (GST_IS_VA_DMABUF_ALLOCATOR (vpool->allocator)) {
    if (vpool->starting) {
      if (!gst_va_dmabuf_allocator_setup_buffer (vpool->allocator, buf))
        goto no_memory;
    } else if (!gst_va_dmabuf_allocator_prepare_buffer (vpool->allocator, buf)) {
      if (!gst_va_dmabuf_allocator_setup_buffer (vpool->allocator, buf))
        goto no_memory;
    }
  } else if (GST_IS_VA_ALLOCATOR (vpool->allocator)) {
    if (vpool->starting) {
      if (!gst_va_allocator_setup_buffer (vpool->allocator, buf))
        goto no_memory;
    } else if (!gst_va_allocator_prepare_buffer (vpool->allocator, buf)) {
      if (!gst_va_allocator_setup_buffer (vpool->allocator, buf))
        goto no_memory;
    }
  } else
    goto no_memory;

  if (vpool->add_videometa) {
    if (vpool->crop_left > 0 || vpool->crop_top > 0) {
      GstVideoCropMeta *crop_meta;

      /* For video crop, its video meta's width and height should be
         the full size of uncropped resolution. */
      gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&vpool->alloc_info),
          GST_VIDEO_INFO_WIDTH (&vpool->alloc_info),
          GST_VIDEO_INFO_HEIGHT (&vpool->alloc_info),
          GST_VIDEO_INFO_N_PLANES (&vpool->alloc_info),
          vpool->alloc_info.offset, vpool->alloc_info.stride);

      crop_meta = gst_buffer_add_video_crop_meta (buf);
      crop_meta->x = vpool->crop_left;
      crop_meta->y = vpool->crop_top;
      crop_meta->width = GST_VIDEO_INFO_WIDTH (&vpool->caps_info);
      crop_meta->height = GST_VIDEO_INFO_HEIGHT (&vpool->caps_info);
    } else {
      /* GstVaAllocator may update offset/stride given the physical
       * memory */
      gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&vpool->caps_info),
          GST_VIDEO_INFO_WIDTH (&vpool->caps_info),
          GST_VIDEO_INFO_HEIGHT (&vpool->caps_info),
          GST_VIDEO_INFO_N_PLANES (&vpool->caps_info),
          vpool->alloc_info.offset, vpool->alloc_info.stride);
    }
  }

  *buffer = buf;

  return GST_FLOW_OK;

  /* ERROR */
no_memory:
  {
    gst_buffer_unref (buf);
    GST_WARNING_OBJECT (vpool, "can't create memory");
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_va_pool_start (GstBufferPool * pool)
{
  GstVaPool *vpool = GST_VA_POOL (pool);
  gboolean ret;

  vpool->starting = TRUE;
  ret = GST_BUFFER_POOL_CLASS (parent_class)->start (pool);
  vpool->starting = FALSE;

  return ret;
}

static gboolean
gst_va_pool_stop (GstBufferPool * pool)
{
  GstVaPool *vpool = GST_VA_POOL (pool);
  gboolean ret;

  ret = GST_BUFFER_POOL_CLASS (parent_class)->stop (pool);

  if (GST_IS_VA_DMABUF_ALLOCATOR (vpool->allocator))
    gst_va_dmabuf_allocator_flush (vpool->allocator);
  else if (GST_IS_VA_ALLOCATOR (vpool->allocator))
    gst_va_allocator_flush (vpool->allocator);

  return ret;
}

static void
gst_va_pool_dispose (GObject * object)
{
  GstVaPool *pool = GST_VA_POOL (object);

  GST_LOG_OBJECT (pool, "finalize video buffer pool %p", pool);

  gst_clear_object (&pool->allocator);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_va_pool_class_init (GstVaPoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *gstbufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->dispose = gst_va_pool_dispose;

  gstbufferpool_class->get_options = gst_va_pool_get_options;
  gstbufferpool_class->set_config = gst_va_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_va_pool_alloc;
  gstbufferpool_class->start = gst_va_pool_start;
  gstbufferpool_class->stop = gst_va_pool_stop;
}

static void
gst_va_pool_init (GstVaPool * self)
{
}

GstBufferPool *
gst_va_pool_new (void)
{
  GstVaPool *pool;

  pool = g_object_new (GST_TYPE_VA_POOL, NULL);
  gst_object_ref_sink (pool);

  GST_LOG_OBJECT (pool, "new va video buffer pool %p", pool);

  return GST_BUFFER_POOL_CAST (pool);
}

void
gst_buffer_pool_config_set_va_allocation_params (GstStructure * config,
    guint usage_hint)
{
  gst_structure_set (config, "usage-hint", G_TYPE_UINT, usage_hint, NULL);
}

void
gst_buffer_pool_config_set_va_alignment (GstStructure * config,
    const GstVideoAlignment * align)
{
  gst_structure_set (config,
      "va-padding-top", G_TYPE_UINT, align->padding_top,
      "va-padding-bottom", G_TYPE_UINT, align->padding_bottom,
      "va-padding-left", G_TYPE_UINT, align->padding_left,
      "va-padding-right", G_TYPE_UINT, align->padding_right, NULL);
}

gboolean
gst_va_pool_requires_video_meta (GstBufferPool * pool)
{
  return GST_VA_POOL (pool)->force_videometa;
}

GstBufferPool *
gst_va_pool_new_with_config (GstCaps * caps, guint size, guint min_buffers,
    guint max_buffers, guint usage_hint, GstAllocator * allocator,
    GstAllocationParams * alloc_params)
{
  GstBufferPool *pool;
  GstStructure *config;

  pool = gst_va_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min_buffers,
      max_buffers);
  gst_buffer_pool_config_set_va_allocation_params (config, usage_hint);
  gst_buffer_pool_config_set_allocator (config, allocator, alloc_params);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (!gst_buffer_pool_set_config (pool, config))
    gst_clear_object (&pool);

  return pool;
}
