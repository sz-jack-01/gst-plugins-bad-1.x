/* GStreamer Wayland video sink
 *
 * Copyright (C) 2014 Collabora Ltd.
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

#include "wldisplay.h"
#include "wlbuffer.h"
#include "wlwindow.h"
#include "wlvideoformat.h"

#include <errno.h>
#include <linux/input.h>

GST_DEBUG_CATEGORY_EXTERN (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

G_DEFINE_TYPE (GstWlDisplay, gst_wl_display, G_TYPE_OBJECT);

static void gst_wl_display_finalize (GObject * gobject);

struct input
{
  GstWlDisplay *display;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_touch *touch;

  void *pointer_focus;

  struct wl_list link;
};

static void
gst_wl_display_class_init (GstWlDisplayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gst_wl_display_finalize;
}

static void
gst_wl_display_init (GstWlDisplay * self)
{
  self->shm_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  self->dmabuf_formats = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  self->wl_fd_poll = gst_poll_new (TRUE);
  self->buffers = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_mutex_init (&self->buffers_mutex);
}

static void
gst_wl_ref_wl_buffer (gpointer key, gpointer value, gpointer user_data)
{
  g_object_ref (value);
}

static void
input_destroy (struct input *input)
{
  if (input->touch)
    wl_touch_destroy (input->touch);
  if (input->pointer)
    wl_pointer_destroy (input->pointer);

  wl_list_remove (&input->link);
  wl_seat_destroy (input->seat);
  free (input);
}

static void
display_destroy_inputs (GstWlDisplay * self)
{
  struct input *tmp;
  struct input *input;

  wl_list_for_each_safe (input, tmp, &self->input_list, link)
      input_destroy (input);
}

static void
gst_wl_display_finalize (GObject * gobject)
{
  GstWlDisplay *self = GST_WL_DISPLAY (gobject);

  gst_poll_set_flushing (self->wl_fd_poll, TRUE);
  if (self->thread)
    g_thread_join (self->thread);

  display_destroy_inputs (self);

  if (self->cursor_surface)
    wl_surface_destroy (self->cursor_surface);

  if (self->cursor_theme)
    wl_cursor_theme_destroy (self->cursor_theme);

  /* to avoid buffers being unregistered from another thread
   * at the same time, take their ownership */
  g_mutex_lock (&self->buffers_mutex);
  self->shutting_down = TRUE;
  g_hash_table_foreach (self->buffers, gst_wl_ref_wl_buffer, NULL);
  g_mutex_unlock (&self->buffers_mutex);

  g_hash_table_foreach (self->buffers,
      (GHFunc) gst_wl_buffer_force_release_and_unref, NULL);
  g_hash_table_remove_all (self->buffers);

  g_array_unref (self->shm_formats);
  g_array_unref (self->dmabuf_formats);
  gst_poll_free (self->wl_fd_poll);
  g_hash_table_unref (self->buffers);
  g_mutex_clear (&self->buffers_mutex);

  if (self->viewporter)
    wp_viewporter_destroy (self->viewporter);

  if (self->shm)
    wl_shm_destroy (self->shm);

  if (self->dmabuf)
    zwp_linux_dmabuf_v1_destroy (self->dmabuf);

  if (self->wl_shell)
    wl_shell_destroy (self->wl_shell);

  if (self->xdg_wm_base)
    xdg_wm_base_destroy (self->xdg_wm_base);

  if (self->fullscreen_shell)
    zwp_fullscreen_shell_v1_release (self->fullscreen_shell);

  if (self->compositor)
    wl_compositor_destroy (self->compositor);

  if (self->subcompositor)
    wl_subcompositor_destroy (self->subcompositor);

  if (self->registry)
    wl_registry_destroy (self->registry);

  if (self->display_wrapper)
    wl_proxy_wrapper_destroy (self->display_wrapper);

  if (self->queue)
    wl_event_queue_destroy (self->queue);

  if (self->own_display) {
    wl_display_flush (self->display);
    wl_display_disconnect (self->display);
  }

  G_OBJECT_CLASS (gst_wl_display_parent_class)->finalize (gobject);
}

static void
shm_format (void *data, struct wl_shm *wl_shm, uint32_t format)
{
  GstWlDisplay *self = data;

  g_array_append_val (self->shm_formats, format);
}

static const struct wl_shm_listener shm_listener = {
  shm_format
};

static void
dmabuf_format (void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
    uint32_t format)
{
  GstWlDisplay *self = data;

  if (gst_wl_dmabuf_format_to_video_format (format) != GST_VIDEO_FORMAT_UNKNOWN)
    g_array_append_val (self->dmabuf_formats, format);

  if (format == DRM_FORMAT_NV15)
    self->support_nv12_10le40 = TRUE;
}

static void
dmabuf_modifier (void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
    uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
  GstWlDisplay *self = data;
  uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;

  if (modifier == DRM_AFBC_MODIFIER)
    self->support_afbc = TRUE;

  dmabuf_format (data, zwp_linux_dmabuf, format);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
  dmabuf_format,
  dmabuf_modifier,
};

gboolean
gst_wl_display_check_format_for_shm (GstWlDisplay * display,
    GstVideoFormat format)
{
  enum wl_shm_format shm_fmt;
  GArray *formats;
  guint i;

  shm_fmt = gst_video_format_to_wl_shm_format (format);
  if (shm_fmt == (enum wl_shm_format) -1)
    return FALSE;

  formats = display->shm_formats;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == shm_fmt)
      return TRUE;
  }

  return FALSE;
}

gboolean
gst_wl_display_check_format_for_dmabuf (GstWlDisplay * display,
    GstVideoFormat format)
{
  GArray *formats;
  guint i, dmabuf_fmt;

  if (!display->dmabuf)
    return FALSE;

  dmabuf_fmt = gst_video_format_to_wl_dmabuf_format (format);
  if (dmabuf_fmt == (guint) - 1)
    return FALSE;

  formats = display->dmabuf_formats;
  for (i = 0; i < formats->len; i++) {
    if (g_array_index (formats, uint32_t, i) == dmabuf_fmt)
      return TRUE;
  }

  return FALSE;
}

static void
handle_xdg_wm_base_ping (void *user_data, struct xdg_wm_base *xdg_wm_base,
    uint32_t serial)
{
  xdg_wm_base_pong (xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  handle_xdg_wm_base_ping
};

static void
display_set_cursor (GstWlDisplay *self, struct wl_pointer *pointer,
    uint32_t serial)
{
  struct wl_buffer *buffer;
  struct wl_cursor_image *image;

  if (!self->default_cursor)
    return;

  if (!self->cursor_surface) {
      self->cursor_surface =
          wl_compositor_create_surface (self->compositor);
      if (!self->cursor_surface)
        return;
  }

  image = self->default_cursor->images[0];
  buffer = wl_cursor_image_get_buffer (image);
  if (!buffer)
    return;

  wl_pointer_set_cursor (pointer, serial,
      self->cursor_surface, image->hotspot_x, image->hotspot_y);
  wl_surface_attach (self->cursor_surface, buffer, 0, 0);
  wl_surface_damage (self->cursor_surface, 0, 0,
      image->width, image->height);
  wl_surface_commit (self->cursor_surface);
}

static void
pointer_handle_enter (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface,
    wl_fixed_t sx_w, wl_fixed_t sy_w)
{
  struct input *input = data;
  GstWlDisplay *display = input->display;
  GstWlWindow *window;

  if (!surface) {
    /* enter event for a window we've just destroyed */
    return;
  }

  if (surface != display->touch_surface) {
    /* Ignoring input event from other surfaces */
    return;
  }

  window = wl_surface_get_user_data (surface);
  if (!window || !gst_wl_window_is_toplevel (window)) {
    /* Ignoring input event from subsurface */
    return;
  }

  input->pointer_focus = window;
  display_set_cursor (window->display, pointer, serial);
}

static void
pointer_handle_leave (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface)
{
  struct input *input = data;

  if (input->pointer_focus) {
    input->pointer_focus = NULL;
    wl_pointer_set_cursor (pointer, serial, NULL, 0, 0);
  }
}

static void
pointer_handle_motion (void *data, struct wl_pointer *pointer,
    uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button (void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
  struct input *input = data;
  GstWlWindow *window;

  window = input->pointer_focus;
  if (!window)
    return;

  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (window->display->xdg_wm_base)
      xdg_toplevel_move (window->xdg_toplevel, input->seat, serial);
    else
      wl_shell_surface_move (window->wl_shell_surface, input->seat, serial);
  }
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
  pointer_handle_enter,
  pointer_handle_leave,
  pointer_handle_motion,
  pointer_handle_button,
  pointer_handle_axis,
};

static void
touch_handle_down (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, struct wl_surface *surface,
    int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
  struct input *input = data;
  GstWlDisplay *display = input->display;
  GstWlWindow *window;

  if (!surface) {
    /* enter event for a window we've just destroyed */
    return;
  }

  if (surface != display->touch_surface) {
    /* Ignoring input event from other surfaces */
    return;
  }

  window = wl_surface_get_user_data (surface);
  if (!window || !gst_wl_window_is_toplevel (window)) {
    /* Ignoring input event from subsurface */
    return;
  }

  if (window->display->xdg_wm_base)
    xdg_toplevel_move (window->xdg_toplevel, input->seat, serial);
  else
    wl_shell_surface_move (window->wl_shell_surface, input->seat, serial);
}

static void
touch_handle_up (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion (void *data, struct wl_touch *wl_touch,
    uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame (void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel (void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
  touch_handle_down,
  touch_handle_up,
  touch_handle_motion,
  touch_handle_frame,
  touch_handle_cancel,
};

static void
seat_handle_capabilities (void *data, struct wl_seat *seat,
    enum wl_seat_capability caps)
{
  struct input *input = data;

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
    input->pointer = wl_seat_get_pointer (seat);
    wl_pointer_add_listener (input->pointer, &pointer_listener, input);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer) {
    wl_pointer_destroy (input->pointer);
    input->pointer = NULL;
  }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch) {
    input->touch = wl_seat_get_touch (seat);
    wl_touch_add_listener (input->touch, &touch_listener, input);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch) {
    wl_touch_destroy (input->touch);
    input->touch = NULL;
  }
}

static const struct wl_seat_listener seat_listener = {
  seat_handle_capabilities,
};

static void
display_add_input (GstWlDisplay *self, uint32_t id)
{
  struct input *input;

  input = calloc (1, sizeof (*input));
  if (input == NULL) {
    GST_ERROR ("Error out of memory");
    return;
  }

  input->display = self;

  input->seat = wl_registry_bind (self->registry, id, &wl_seat_interface, 1);

  wl_seat_add_listener (input->seat, &seat_listener, input);
  wl_seat_set_user_data (input->seat, input);

  wl_list_insert(self->input_list.prev, &input->link);
}

static void
registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version)
{
  GstWlDisplay *self = data;

  if (g_strcmp0 (interface, "wl_compositor") == 0) {
    self->compositor = wl_registry_bind (registry, id, &wl_compositor_interface,
        MIN (version, 4));
  } else if (g_strcmp0 (interface, "wl_subcompositor") == 0) {
    self->subcompositor =
        wl_registry_bind (registry, id, &wl_subcompositor_interface, 1);
  } else if (g_strcmp0 (interface, "wl_shell") == 0) {
    self->wl_shell = wl_registry_bind (registry, id, &wl_shell_interface, 1);
  } else if (g_strcmp0 (interface, "xdg_wm_base") == 0) {
    self->xdg_wm_base =
        wl_registry_bind (registry, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener (self->xdg_wm_base, &xdg_wm_base_listener, self);
  } else if (g_strcmp0 (interface, "zwp_fullscreen_shell_v1") == 0) {
    self->fullscreen_shell = wl_registry_bind (registry, id,
        &zwp_fullscreen_shell_v1_interface, 1);
  } else if (g_strcmp0 (interface, "wl_shm") == 0) {
    self->shm = wl_registry_bind (registry, id, &wl_shm_interface, 1);
    wl_shm_add_listener (self->shm, &shm_listener, self);

    self->cursor_theme = wl_cursor_theme_load (NULL, 32, self->shm);
    if (!self->cursor_theme) {
      GST_ERROR ("Error loading default cursor theme");
    } else {
      self->default_cursor =
          wl_cursor_theme_get_cursor (self->cursor_theme, "left_ptr");
      if (!self->default_cursor)
        GST_ERROR ("Error loading default left cursor pointer");
    }
  } else if (g_strcmp0 (interface, "wl_seat") == 0) {
    display_add_input (self, id);
  } else if (g_strcmp0 (interface, "wp_viewporter") == 0) {
    self->viewporter =
        wl_registry_bind (registry, id, &wp_viewporter_interface, 1);
  } else if (g_strcmp0 (interface, "zwp_linux_dmabuf_v1") == 0) {
    self->dmabuf =
        wl_registry_bind (registry, id, &zwp_linux_dmabuf_v1_interface, 3);
    zwp_linux_dmabuf_v1_add_listener (self->dmabuf, &dmabuf_listener, self);
  }
}

static void
registry_handle_global_remove (void *data, struct wl_registry *registry,
    uint32_t name)
{
  /* temporarily do nothing */
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  registry_handle_global_remove
};

static gpointer
gst_wl_display_thread_run (gpointer data)
{
  GstWlDisplay *self = data;
  GstPollFD pollfd = GST_POLL_FD_INIT;

  pollfd.fd = wl_display_get_fd (self->display);
  gst_poll_add_fd (self->wl_fd_poll, &pollfd);
  gst_poll_fd_ctl_read (self->wl_fd_poll, &pollfd, TRUE);

  /* main loop */
  while (1) {
    while (wl_display_prepare_read_queue (self->display, self->queue) != 0)
      wl_display_dispatch_queue_pending (self->display, self->queue);
    wl_display_flush (self->display);

    if (gst_poll_wait (self->wl_fd_poll, GST_CLOCK_TIME_NONE) < 0) {
      gboolean normal = (errno == EBUSY);
      wl_display_cancel_read (self->display);
      if (normal)
        break;
      else
        goto error;
    }
    if (wl_display_read_events (self->display) == -1)
      goto error;
    wl_display_dispatch_queue_pending (self->display, self->queue);
  }

  return NULL;

error:
  GST_ERROR ("Error communicating with the wayland server");
  return NULL;
}

GstWlDisplay *
gst_wl_display_new (const gchar * name, GError ** error)
{
  struct wl_display *display;

  display = wl_display_connect (name);

  if (!display) {
    *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
        "Failed to connect to the wayland display '%s'",
        name ? name : "(default)");
    return NULL;
  } else {
    return gst_wl_display_new_existing (display, TRUE, error);
  }
}

GstWlDisplay *
gst_wl_display_new_existing (struct wl_display * display,
    gboolean take_ownership, GError ** error)
{
  GstWlDisplay *self;
  GError *err = NULL;
  gint i;

  g_return_val_if_fail (display != NULL, NULL);

  self = g_object_new (GST_TYPE_WL_DISPLAY, NULL);
  self->display = display;
  self->display_wrapper = wl_proxy_create_wrapper (display);
  self->own_display = take_ownership;

  wl_list_init (&self->input_list);

  self->queue = wl_display_create_queue (self->display);
  wl_proxy_set_queue ((struct wl_proxy *) self->display_wrapper, self->queue);
  self->registry = wl_display_get_registry (self->display_wrapper);
  wl_registry_add_listener (self->registry, &registry_listener, self);

  /* we need exactly 2 roundtrips to discover global objects and their state */
  for (i = 0; i < 2; i++) {
    if (wl_display_roundtrip_queue (self->display, self->queue) < 0) {
      *error = g_error_new (g_quark_from_static_string ("GstWlDisplay"), 0,
          "Error communicating with the wayland display");
      g_object_unref (self);
      return NULL;
    }
  }

  /* verify we got all the required interfaces */
#define VERIFY_INTERFACE_EXISTS(var, interface) \
  if (!self->var) { \
    g_set_error (error, g_quark_from_static_string ("GstWlDisplay"), 0, \
        "Could not bind to " interface ". Either it is not implemented in " \
        "the compositor, or the implemented version doesn't match"); \
    g_object_unref (self); \
    return NULL; \
  }

  VERIFY_INTERFACE_EXISTS (compositor, "wl_compositor");
  VERIFY_INTERFACE_EXISTS (subcompositor, "wl_subcompositor");
  VERIFY_INTERFACE_EXISTS (shm, "wl_shm");

#undef VERIFY_INTERFACE_EXISTS

  /* We make the viewporter optional even though it may cause bad display.
   * This is so one can test wayland display on older compositor or on
   * compositor that don't implement this extension. */
  if (!self->viewporter) {
    g_warning ("Wayland compositor is missing the ability to scale, video "
        "display may not work properly.");
  }

  if (!self->dmabuf) {
    g_warning ("Could not bind to zwp_linux_dmabuf_v1");
  }

  if (!self->wl_shell && !self->xdg_wm_base && !self->fullscreen_shell) {
    /* If wl_surface and wl_display are passed via GstContext
     * wl_shell, xdg_shell and zwp_fullscreen_shell are not used.
     * In this case is correct to continue.
     */
    g_warning ("Could not bind to either wl_shell, xdg_wm_base or "
        "zwp_fullscreen_shell, video display may not work properly.");
  }

  self->thread = g_thread_try_new ("GstWlDisplay", gst_wl_display_thread_run,
      self, &err);
  if (err) {
    g_propagate_prefixed_error (error, err,
        "Failed to start thread for the display's events");
    g_object_unref (self);
    return NULL;
  }

  return self;
}

void
gst_wl_display_register_buffer (GstWlDisplay * self, gpointer gstmem,
    gpointer wlbuffer)
{
  g_assert (!self->shutting_down);

  GST_TRACE_OBJECT (self, "registering GstWlBuffer %p to GstMem %p",
      wlbuffer, gstmem);

  g_mutex_lock (&self->buffers_mutex);
  g_hash_table_replace (self->buffers, gstmem, wlbuffer);
  g_mutex_unlock (&self->buffers_mutex);
}

gpointer
gst_wl_display_lookup_buffer (GstWlDisplay * self, gpointer gstmem)
{
  gpointer wlbuffer;
  g_mutex_lock (&self->buffers_mutex);
  wlbuffer = g_hash_table_lookup (self->buffers, gstmem);
  g_mutex_unlock (&self->buffers_mutex);
  return wlbuffer;
}

void
gst_wl_display_unregister_buffer (GstWlDisplay * self, gpointer gstmem)
{
  GST_TRACE_OBJECT (self, "unregistering GstWlBuffer owned by %p", gstmem);

  g_mutex_lock (&self->buffers_mutex);
  if (G_LIKELY (!self->shutting_down))
    g_hash_table_remove (self->buffers, gstmem);
  g_mutex_unlock (&self->buffers_mutex);
}
