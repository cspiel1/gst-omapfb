/*
 * Copyright (C) 2008 Felipe Contreras <felipe.contreras@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#include "omapfb.h"

static GstVideoSinkClass *parent_class;

GST_DEBUG_CATEGORY (omapfb_debug);
#define GST_CAT_DEFAULT omapfb_debug

static GstCaps *
generate_sink_template (void)
{
    GstCaps *caps;
    GstStructure *struc;

    caps = gst_caps_new_empty ();

    struc = gst_structure_new ("video/x-raw-yuv",
                               "width", GST_TYPE_INT_RANGE, 16, 4096,
                               "height", GST_TYPE_INT_RANGE, 16, 4096,
                               "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
                               NULL);

    {
        GValue list;
        GValue val;

        list.g_type = val.g_type = 0;

        g_value_init (&list, GST_TYPE_LIST);
        g_value_init (&val, GST_TYPE_FOURCC);

#if 0
        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('I', '4', '2', '0'));
        gst_value_list_append_value (&list, &val);

        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'));
        gst_value_list_append_value (&list, &val);

        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'));
        gst_value_list_append_value (&list, &val);
#else
        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'));
        gst_value_list_append_value (&list, &val);
#endif

        gst_structure_set_value (struc, "format", &list);

        g_value_unset (&val);
        g_value_unset (&list);
    }

    gst_caps_append_structure (caps, struc);

    return caps;
}

static GstFlowReturn
buffer_alloc (GstBaseSink *bsink,
              guint64 offset,
              guint size,
              GstCaps *caps,
              GstBuffer **buf)
{
    GstOmapFbSink *self;
    GstBuffer *buffer;
    GstFlowReturn ret = GST_FLOW_OK;

    self = GST_OMAPFB_SINK (bsink);

    buffer = gst_buffer_new ();
    GST_BUFFER_DATA (buffer) = self->framebuffer;
    GST_BUFFER_SIZE (buffer) = size;
    gst_buffer_set_caps (buffer, caps);

    *buf = buffer;

    return ret;
}

static gboolean
setcaps (GstBaseSink *bsink,
         GstCaps *vscapslist)
{
    GstOmapFbSink *self;
    GstStructure *structure;
    gint width, height;

    self = GST_OMAPFB_SINK (bsink);

    structure = gst_caps_get_structure (vscapslist, 0);

    gst_structure_get_int (structure, "width", &width);
    gst_structure_get_int (structure, "height", &height);

    self->overlay_info.xres = MIN (self->varinfo.xres, width) & ~15;
    self->overlay_info.yres = MIN (self->varinfo.yres, height) & ~15;
    self->overlay_info.xres_virtual = self->overlay_info.xres;
    self->overlay_info.yres_virtual = self->overlay_info.yres;

    self->overlay_info.xoffset = 0;
    self->overlay_info.yoffset = 0;
    self->overlay_info.nonstd = OMAPFB_COLOR_YUV422;

    GST_INFO_OBJECT (self, "vscreen info: width=%u, height=%u",
                     self->overlay_info.xres, self->overlay_info.yres);

    if (ioctl (self->overlay_fd, FBIOPUT_VSCREENINFO, &self->overlay_info))
    {
        GST_ERROR_OBJECT (self, "could not get screen info");
        return FALSE;
    }

    self->plane_info.enabled = 1;
    self->plane_info.pos_x = 0;
    self->plane_info.pos_y = 0;
    self->plane_info.out_width = self->varinfo.xres;
    self->plane_info.out_height = self->varinfo.yres;

    GST_INFO_OBJECT (self, "plane info: width=%u, height=%u",
                     self->varinfo.xres, self->varinfo.yres);

    if (ioctl (self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info))
    {
        GST_ERROR_OBJECT (self, "could not setup plane");
        return FALSE;
    }

    self->enabled = TRUE;

    return TRUE;
}

static gboolean
start (GstBaseSink *bsink)
{
    GstOmapFbSink *self;
    int fd;

    self = GST_OMAPFB_SINK (bsink);

    fd = open ("/dev/fb0", O_RDWR);

    if (fd == -1)
    {
        GST_ERROR_OBJECT (self, "could not open framebuffer");
        return FALSE;
    }

    if (ioctl (fd, FBIOGET_VSCREENINFO, &self->varinfo))
    {
        GST_ERROR_OBJECT (self, "could not get screen info");
        close (fd);
        return FALSE;
    }

    if (close (fd))
    {
        GST_ERROR_OBJECT (self, "could not close framebuffer");
        return FALSE;
    }

    self->overlay_fd = open ("/dev/fb1", O_RDWR);

    if (self->overlay_fd == -1)
    {
        GST_ERROR_OBJECT (self, "could not open overlay");
        return FALSE;
    }

    if (ioctl (self->overlay_fd, FBIOGET_VSCREENINFO, &self->overlay_info))
    {
        GST_ERROR_OBJECT (self, "could not get overlay screen info");
        return FALSE;
    }

    if (ioctl (self->overlay_fd, OMAPFB_QUERY_PLANE, &self->plane_info))
    {
        GST_ERROR_OBJECT (self, "could not query plane info");
        return FALSE;
    }

    if (ioctl (self->overlay_fd, OMAPFB_QUERY_MEM, &self->mem_info))
    {
        GST_ERROR_OBJECT (self, "could not query memory info");
        return FALSE;
    }

    self->framebuffer = mmap (NULL, self->mem_info.size, PROT_WRITE, MAP_SHARED, self->overlay_fd, 0);
    if (self->framebuffer == MAP_FAILED)
    {
        GST_ERROR_OBJECT (self, "memory map failed");
        return FALSE;
    }

    return TRUE;
}

static gboolean
stop (GstBaseSink *bsink)
{
    GstOmapFbSink *self;

    self = GST_OMAPFB_SINK (bsink);

    if (self->enabled)
    {
        self->plane_info.enabled = 0;

        if (ioctl (self->overlay_fd, OMAPFB_SETUP_PLANE, &self->plane_info))
        {
            GST_ERROR_OBJECT (self, "could not disable plane");
            return FALSE;
        }
    }

    if (munmap (self->framebuffer, self->mem_info.size))
    {
        GST_ERROR_OBJECT (self, "could not unmap");
        return FALSE;
    }

    if (close (self->overlay_fd))
    {
        GST_ERROR_OBJECT (self, "could not close overlay");
        return FALSE;
    }

    return TRUE;
}

static GstFlowReturn
render (GstBaseSink *bsink, GstBuffer *buffer)
{
    GstOmapFbSink *self;
    self = GST_OMAPFB_SINK (bsink);

    if (GST_BUFFER_DATA (buffer) == self->framebuffer)
    {
        return GST_FLOW_OK;
    }
    else
    {
        /* memcpy needed */
        if (memcpy (self->framebuffer, GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer)))
        {
            return GST_FLOW_OK;
        }
    }

    return GST_FLOW_ERROR;
}

static void
type_class_init (gpointer g_class,
                 gpointer class_data)
{
    GstBaseSinkClass *base_sink_class;

    base_sink_class = (GstBaseSinkClass *) g_class;

    parent_class = g_type_class_ref (GST_OMAPFB_SINK_TYPE);

    base_sink_class->set_caps = GST_DEBUG_FUNCPTR (setcaps);
    base_sink_class->buffer_alloc = GST_DEBUG_FUNCPTR (buffer_alloc);
    base_sink_class->start = GST_DEBUG_FUNCPTR (start);
    base_sink_class->stop = GST_DEBUG_FUNCPTR (stop);
    base_sink_class->render = GST_DEBUG_FUNCPTR (render);
}

static void
type_base_init (gpointer g_class)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

    {
        GstElementDetails details;

        details.longname = "Linux OMAP framebuffer sink";
        details.klass = "Sink/Video";
        details.description = "Renders video with omapfb";
        details.author = "Felipe Contreras";

        gst_element_class_set_details (element_class, &details);
    }

    {
        GstPadTemplate *template;

        template = gst_pad_template_new ("sink", GST_PAD_SINK,
                                         GST_PAD_ALWAYS,
                                         generate_sink_template ());

        gst_element_class_add_pad_template (element_class, template);
    }
}

GType
gst_omapfbsink_get_type (void)
{
    static GType type = 0;

    if (G_UNLIKELY (type == 0))
    {
        GTypeInfo *type_info;

        type_info = g_new0 (GTypeInfo, 1);
        type_info->class_size = sizeof (GstOmapFbSinkClass);
        type_info->base_init = type_base_init;
        type_info->class_init = type_class_init;
        type_info->instance_size = sizeof (GstOmapFbSink);

        type = g_type_register_static (GST_TYPE_BASE_SINK, "GstOmapFbSink", type_info, 0);

        g_free (type_info);
    }

    return type;
}

static gboolean
plugin_init (GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT (omapfb_debug, "omapfb", 0, "omapfb");

    if (!gst_element_register (plugin, "omapfbsink", GST_RANK_NONE, GST_OMAPFB_SINK_TYPE))
        return FALSE;

    return TRUE;
}

GstPluginDesc gst_plugin_desc =
{
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "omapfb",
    (gchar *) "Linux OMAP framebuffer",
    plugin_init,
    "0.1",
    "LGPL",
    "source",
    "package",
    "origin",
    GST_PADDING_INIT
};
