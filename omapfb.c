/*
 * Copyright (C) 2008-2010 Felipe Contreras
 *
 * Author: Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/gstvideosink.h>

#include <linux/fb.h>
#include <linux/omapfb.h>

#include "omapfb.h"
#include "log.h"
#include "image-format-conversions.h"

#define ROUND_UP(num, scale) (((num) + ((scale) - 1)) & ~((scale) - 1))

static GstElementClass *parent_class = NULL;

#ifndef GST_DISABLE_GST_DEBUG
GstDebugCategory *omapfb_debug;
#endif

enum
{
	PROP_RENDER_X = 1,
	PROP_RENDER_Y,
	PROP_RENDER_W,
	PROP_RENDER_H
};

static int fb_used = 0;
GMutex fb_used_lock;

#define FB_USED_MUTEX_LOCK() G_STMT_START {                               \
    GST_LOG ("locking fb_used from thread %p", g_thread_self ());         \
    g_mutex_lock (&fb_used_lock);					                      \
    GST_LOG ("locked fb_used from thread %p", g_thread_self ());          \
} G_STMT_END

#define FB_USED_MUTEX_UNLOCK() G_STMT_START {                             \
    GST_LOG ("unlocking fb_used from thread %p", g_thread_self ());		  \
    g_mutex_unlock (&fb_used_lock);                                       \
} G_STMT_END


struct gst_omapfb_sink {
	GstBaseSink parent;

	struct fb_var_screeninfo overlay_info;
	struct omapfb_mem_info mem_info;
	struct omapfb_plane_info plane_info;
	int par_n, par_d;
	int width, height;
	guint32 fourcc;

	int overlay_fd;
	short devid;
	const char *dev;
	unsigned char *framebuffer;
	bool enabled;
	bool manual_update;

	/* target video rectangle */
	GstVideoRectangle render_rect;
	gboolean have_render_rect;
	gboolean render_rect_changed;
};

struct gst_omapfb_sink_class {
	GstBaseSinkClass parent_class;
};

static struct fb_var_screeninfo _varinfo;

#define GST_IS_OMAPFBSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_OMAPFB_SINK_TYPE))
#define GST_OMAPFBSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_OMAPFB_SINK_TYPE, struct gst_omapfb_sink))

static void
gst_omapfb_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void
gst_omapfb_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-raw-yuv",
			"width", GST_TYPE_INT_RANGE, 16, 4096,
			"height", GST_TYPE_INT_RANGE, 16, 4096,
			"framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
			NULL);

	{
		GValue list;
		GValue val;

		list.g_type = val.g_type = 0;

		g_value_init(&list, GST_TYPE_LIST);
		g_value_init(&val, GST_TYPE_FOURCC);

		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('I', '4', '2', '0'));
		gst_value_list_append_value(&list, &val);

#if 0
		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('Y', 'U', 'Y', '2'));
		gst_value_list_append_value(&list, &val);

		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('U', 'Y', 'V', 'Y'));
		gst_value_list_append_value(&list, &val);
#else
		gst_value_set_fourcc(&val, GST_MAKE_FOURCC('U', 'Y', 'V', 'Y'));
		gst_value_list_append_value(&list, &val);
#endif

		gst_structure_set_value(struc, "format", &list);

		g_value_unset(&val);
		g_value_unset(&list);
	}

	gst_caps_append_structure(caps, struc);

	return caps;
}

static void
update(struct gst_omapfb_sink *self)
{
	struct omapfb_update_window update_window;
	unsigned x, y, w, h;

	x = y = 0;
	w = _varinfo.xres;
	h = _varinfo.yres;
	update_window.x = x;
	update_window.y = y;
	update_window.width = w;
	update_window.height = h;
	update_window.format = 0;
	update_window.out_x = 0;
	update_window.out_y = 0;
	update_window.out_width = w;
	update_window.out_height = h;

	ioctl(self->overlay_fd, OMAPFB_UPDATE_WINDOW, &update_window);
}

static bool
check_render_rect(struct gst_omapfb_sink *self)
{
  if (self->have_render_rect) {
	if ((guint) self->render_rect.x > _varinfo.xres-16)
	  self->render_rect.x = _varinfo.xres-16;
	if ((guint) self->render_rect.y > _varinfo.yres-16)
	  self->render_rect.y = _varinfo.yres-16;
	if ((guint) self->render_rect.x + (guint) self->render_rect.w > _varinfo.xres)
	  self->render_rect.w = _varinfo.xres - (guint) self->render_rect.x;
	if ((guint) self->render_rect.y + (guint) self->render_rect.h > _varinfo.yres)
	  self->render_rect.h = _varinfo.yres - (guint) self->render_rect.y;

	if (!self->render_rect.w || !self->render_rect.h)
		self->have_render_rect = false;
  }

  return self->have_render_rect;
}

static gboolean
setup_plane(struct gst_omapfb_sink *self)
{
	int update_mode;
	unsigned rx, ry, rw, rh;
	unsigned out_width, out_height;
/*    struct omapfb_color_key color_key;*/
	size_t framesize;

	if (self->mem_info.size && munmap(self->framebuffer, self->mem_info.size)) {
		pr_err(self, "could not unmap %s", strerror(errno));
	}

	self->plane_info.enabled = 0;
	if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
		pr_err(self, "could not disable plane");
		return false;
	}

	framesize = GST_ROUND_UP_2(self->width) * self->height * 2;

	self->mem_info.type = OMAPFB_MEMTYPE_SDRAM;
	self->mem_info.size = framesize;

	if (ioctl(self->overlay_fd, OMAPFB_SETUP_MEM, &self->mem_info)) {
		self->mem_info.size = 0;
		pr_err(self, "could not setup memory info %dx%d", self->width, self->height);
		return false;
	}

	self->framebuffer = mmap(NULL, self->mem_info.size, PROT_WRITE, MAP_SHARED, self->overlay_fd, 0);
	if (self->framebuffer == MAP_FAILED) {
		self->mem_info.size = 0;
		pr_err(self, "memory map failed");
		return false;
	}

	self->overlay_info.xres = self->width;
	self->overlay_info.yres = self->height;
	self->overlay_info.xres_virtual = self->overlay_info.xres;
	self->overlay_info.yres_virtual = self->overlay_info.yres;

	self->overlay_info.xoffset = 0;
	self->overlay_info.yoffset = 0;
	self->overlay_info.nonstd = OMAPFB_COLOR_YUV422;

	pr_info(self, "vscreen info: width=%u, height=%u",
			self->overlay_info.xres, self->overlay_info.yres);

	if (ioctl(self->overlay_fd, FBIOPUT_VSCREENINFO, &self->overlay_info)) {
		pr_err(self, "could not set screen info");
		return false;
	}

/*    color_key.key_type = OMAPFB_COLOR_KEY_DISABLED;*/
/*    if (ioctl(self->overlay_fd, OMAPFB_SET_COLOR_KEY, &color_key))*/
/*        pr_err(self, "could not disable color key");*/

	if (self->have_render_rect && check_render_rect(self)) {
	  rw = self->render_rect.w & ~0xf;
	  rh = self->render_rect.h & ~0xf;
	  rx = self->render_rect.x + (self->render_rect.w-rw)/2;
	  ry = self->render_rect.y + (self->render_rect.h-rh)/2;
	} else  {
	  rx = 0;
	  ry = 0;
	  rw = _varinfo.xres;
	  rh = _varinfo.yres;
	}
	/* scale to width */
	out_width = rw;
	out_height =
		(self->height * self->par_d * rw + self->width * self->par_n/2)
		/ (self->width * self->par_n);
	if (out_height > rh) {
		/* scale to height */
		out_height = rh;
		out_width =
			(self->width * self->par_n * rh + self->height * self->par_d/2)
			/ (self->height * self->par_d);
	}
	out_width = ROUND_UP(out_width, 2);
	out_height = ROUND_UP(out_height, 2);

	self->plane_info.enabled = 1;
	self->plane_info.pos_x = rx + (rw - out_width) / 2;
	self->plane_info.pos_y = ry + (rh - out_height) / 2;
	self->plane_info.out_width = out_width;
	self->plane_info.out_height = out_height;

	printf("plane info: %dx%d, offset: %d,%d\n",
			self->plane_info.out_width, self->plane_info.out_height,
			self->plane_info.pos_x, self->plane_info.pos_y);
	printf("render rectangle: %ux%u, offset: %d,%d\n", rw, rh, rx, ry);

	if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
		pr_err(self, "could not setup plane");
		return false;
	}

	self->enabled = true;

	update_mode = OMAPFB_MANUAL_UPDATE;
	ioctl(self->overlay_fd, OMAPFB_SET_UPDATE_MODE, &update_mode);
	self->manual_update = (update_mode == OMAPFB_MANUAL_UPDATE);

	return true;
}

static gboolean
setup(struct gst_omapfb_sink *self, GstCaps *caps)
{
	GstStructure *structure;

	_varinfo = _varinfo;
	structure = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(structure, "width", &self->width);
	gst_structure_get_int(structure, "height", &self->height);
	if (!gst_structure_get_fraction(structure, "pixel-aspect-ratio", &self->par_n, &self->par_d))
		self->par_n = self->par_d = 1;

	gst_structure_get_fourcc(structure, "format", &self->fourcc);

	return setup_plane(self);
}

static GstFlowReturn
buffer_alloc(GstBaseSink *base, guint64 offset, guint size, GstCaps *caps, GstBuffer **buf)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	GstBuffer *buffer;

	if (!self->enabled && !setup(self, caps))
		goto missing;

	buffer = gst_buffer_new_and_alloc(size);
	gst_buffer_set_caps(buffer, caps);

	*buf = buffer;

/*    ioctl(self->overlay_fd, OMAPFB_WAITFORVSYNC);*/

	return GST_FLOW_OK;
missing:
	*buf = NULL;
	return GST_FLOW_OK;
}

static gboolean
setcaps(GstBaseSink *base, GstCaps *caps)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;
	if (self->enabled)
		return true;
	return setup(self, caps);
}

static gboolean
start(GstBaseSink *base)
{
	(void) base;
	return true;
}

static gboolean
stop(GstBaseSink *base)
{
	(void) base;
	return true;
}

static gboolean
start_video(struct gst_omapfb_sink *self)
{
	self->dev = NULL;

	self->mem_info.size = 0;

	FB_USED_MUTEX_LOCK();
	if ((fb_used & 1) == 0) {
		self->dev = "/dev/fb1";
		self->devid = 1;
	} else {
		self->dev = "/dev/fb2";
		self->devid = 2;
	}
	if (fb_used >=3)
		fb_used++;
	else
		fb_used |= self->devid;
	FB_USED_MUTEX_UNLOCK();

	if (fb_used > 3) {
		pr_err(self, "more than two video framebuffer are used");
		/* We hope this will be only for a short time and proceed. */
	}

	printf("%s We open %s.\n", __PRETTY_FUNCTION__, self->dev);
	self->overlay_fd = open(self->dev, O_RDWR);

	if (self->overlay_fd == -1) {
		pr_err(self, "could not open overlay");
		return false;
	}

	if (ioctl(self->overlay_fd, FBIOGET_VSCREENINFO, &self->overlay_info)) {
		pr_err(self, "could not get overlay screen info");
		return false;
	}

	if (ioctl(self->overlay_fd, OMAPFB_QUERY_PLANE, &self->plane_info)) {
		pr_err(self, "could not query plane info");
		return false;
	}

	return true;
}

static gboolean
stop_video(struct gst_omapfb_sink* self)
{

	if (self->enabled) {
		self->enabled = false;
		self->plane_info.enabled = 0;

		if (ioctl(self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info)) {
			pr_err(self, "could not disable plane");
			return false;
		}
	}

	if (munmap(self->framebuffer, self->mem_info.size)) {
		pr_err(self, "could not unmap %s", strerror(errno));
	}

	if (close(self->overlay_fd)) {
		pr_err(self, "could not close overlay");
		return false;
	}

	FB_USED_MUTEX_LOCK();
	if (fb_used>3)
		fb_used--;
	else
		fb_used -= self->devid;

	printf("%s We close %s. fb_used=%d\n", __PRETTY_FUNCTION__, self->dev, fb_used);
	FB_USED_MUTEX_UNLOCK();
	return true;
}

static GstStateChangeReturn
change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)element;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
	  start_video(self);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
	  stop_video(self);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }
  return ret;
}

static GstFlowReturn
render(GstBaseSink *base, GstBuffer *buffer)
{
	struct gst_omapfb_sink *self = (struct gst_omapfb_sink *)base;

	if (self->fourcc==GST_MAKE_FOURCC('I', '4', '2', '0')) {
		int src_y_pitch = (self->width + 3) & ~3;
		int src_uv_pitch = (((src_y_pitch >> 1) + 3) & ~3);
		guint8 *yb = GST_BUFFER_DATA(buffer);
		guint8 *ub = yb + (src_y_pitch * self->height);
		guint8 *vb = ub + (src_uv_pitch * (self->height / 2));
		uv12_to_uyvy(self->width & ~15,
				self->height & ~15,
				src_y_pitch,
				src_uv_pitch,
				yb, ub, vb,
				(guint8*) self->framebuffer);
	} else {
		memcpy(self->framebuffer, GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
	}

	if (self->render_rect_changed) {
		self->render_rect_changed = false;
		setup_plane(self);
	}

	if (self->manual_update)
		update(self);

	return GST_FLOW_OK;
}

static bool
init_varinfo()
{
	int fd;
	fd = open("/dev/fb0", O_RDWR);

	_varinfo.xres = G_MAXUINT;
	_varinfo.yres = G_MAXUINT;
	if (fd == -1) {
		fprintf(stderr, "omapfbsink: could not open framebuffer\n");
		return false;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &_varinfo)) {
		fprintf(stderr, "omapfbsink: could not get screen info\n");
		close(fd);
		return false;
	}

	if (close(fd)) {
		fprintf(stderr, "omapfbsink: could not close framebuffer\n");
		return false;
	}

	return true;
}

static void
class_init(void *g_class, void *class_data)
{
	GstBaseSinkClass *base_sink_class;
	GstElementClass *gstelement_class;
	GObjectClass *gobject_class;

	base_sink_class = g_class;

	parent_class = g_type_class_peek_parent (g_class);

	base_sink_class->set_caps = setcaps;
	base_sink_class->buffer_alloc = buffer_alloc;
    base_sink_class->start = start;
    base_sink_class->stop = stop;
	base_sink_class->render = render;
	base_sink_class->preroll = render;

	gstelement_class = (GstElementClass *) g_class;
	gstelement_class->change_state =
		GST_DEBUG_FUNCPTR (change_state);

	gobject_class = (GObjectClass *) g_class;
	gobject_class->set_property = gst_omapfb_sink_set_property;
	gobject_class->get_property = gst_omapfb_sink_get_property;

	init_varinfo();

	g_object_class_install_property (gobject_class, PROP_RENDER_X,
			g_param_spec_uint ("render-x", "Render X-pos.",
				"The X-Position of the render rectangle.",
				0, _varinfo.xres-8, 0,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_RENDER_Y,
			g_param_spec_uint ("render-y", "Render Y-pos.",
				"The Y-Position of the render rectangle.",
				0, _varinfo.yres-8, 0,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_RENDER_W,
			g_param_spec_uint ("render-width", "Render width.",
				"The width of the render rectangle.",
				0,  _varinfo.xres, 0,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_class, PROP_RENDER_H,
			g_param_spec_uint ("render-height", "Render height.",
				"The height of the render rectangle.",
				0, _varinfo.yres, 0,
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"Linux OMAP framebuffer sink",
			"Sink/Video",
			"Renders video with omapfb",
			"Felipe Contreras");

	template = gst_pad_template_new("sink", GST_PAD_SINK,
			GST_PAD_ALWAYS,
			generate_sink_template());

	gst_element_class_add_pad_template(element_class, template);

	gst_object_unref(template);
}

static void
gst_omapfb_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  struct gst_omapfb_sink *osink;
  GstVideoRectangle old_rect;
  int x, y, w, h;

  g_return_if_fail (GST_IS_OMAPFBSINK (object));
  osink = GST_OMAPFBSINK (object);
  old_rect = osink->render_rect;

  switch (prop_id) {
    case PROP_RENDER_X:
	  x = (int) g_value_get_uint (value);
	  osink->render_rect_changed |= (old_rect.x!=x);
      g_atomic_int_set (&osink->render_rect.x, x);
	  osink->have_render_rect = true;
      break;
    case PROP_RENDER_Y:
	  y = (int) g_value_get_uint (value);
	  osink->render_rect_changed |= (old_rect.y!=y);
      g_atomic_int_set (&osink->render_rect.y, y);
	  osink->have_render_rect = true;
      break;
    case PROP_RENDER_W:
	  w = (int) g_value_get_uint (value);
	  osink->render_rect_changed |= (old_rect.w!=w);
      g_atomic_int_set (&osink->render_rect.w, w);
	  osink->have_render_rect = true;
      break;
    case PROP_RENDER_H:
	  h = (int) g_value_get_uint (value);
	  osink->render_rect_changed |= (old_rect.h!=h);
      g_atomic_int_set (&osink->render_rect.h, h);
	  osink->have_render_rect = true;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  if (osink->render_rect_changed)
	check_render_rect(osink);
}

static void
gst_omapfb_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  struct gst_omapfb_sink *osink;

  g_return_if_fail (GST_IS_OMAPFBSINK (object));
  osink = GST_OMAPFBSINK (object);

  switch (prop_id) {
    case PROP_RENDER_X:
      g_value_set_boolean (value,
          g_atomic_int_get (&osink->render_rect.x));
      break;
    case PROP_RENDER_Y:
      g_value_set_boolean (value,
          g_atomic_int_get (&osink->render_rect.y));
      break;
    case PROP_RENDER_W:
      g_value_set_boolean (value,
          g_atomic_int_get (&osink->render_rect.w));
      break;
    case PROP_RENDER_H:
      g_value_set_boolean (value,
          g_atomic_int_get (&osink->render_rect.h));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omapfb_sink_init (struct gst_omapfb_sink * omapfbsink)
{
  omapfbsink->render_rect.x = 0;
  omapfbsink->render_rect.y = 0;
  omapfbsink->render_rect.w = 0;
  omapfbsink->render_rect.h = 0;
  omapfbsink->have_render_rect = false;
  omapfbsink->render_rect_changed = false;
}

GType
gst_omapfb_sink_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct gst_omapfb_sink_class),
			.class_init = class_init,
			.base_init = base_init,
			.instance_size = sizeof(struct gst_omapfb_sink),
			.n_preallocs = 0,
			.instance_init = (GInstanceInitFunc) gst_omapfb_sink_init,
		};

		type = g_type_register_static(GST_TYPE_BASE_SINK, "GstOmapFbSink", &type_info, 0);
	}

	return type;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
#ifndef GST_DISABLE_GST_DEBUG
	omapfb_debug = _gst_debug_category_new("omapfb", 0, "omapfb");
#endif

	if (!gst_element_register(plugin, "omapfbsink", GST_RANK_SECONDARY, GST_OMAPFB_SINK_TYPE))
		return false;

	return true;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = GST_VERSION_MAJOR,
	.minor_version = GST_VERSION_MINOR,
	.name = "omapfb",
	.description = (gchar *) "Linux OMAP framebuffer",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "LGPL",
	.source = "source",
	.package = "package",
	.origin = "origin",
};
