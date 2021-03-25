/*
 * WebRTC Audio Processing Elements
 *
 *  Copyright 2016 Collabora Ltd
 *    @author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *  Copyright 2020
 *    @author: Guillaume Cartier <gucartier@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

/**
 * SECTION:element-webrtcaudioprobe
 *
 * This audio probe is to be used with the webrtcaudioprocessor element. See #webrtcaudioprocessor
 * documentation for more details.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst/webrtcaudioprocessing/gstwebrtcaudioprobe.h"

#ifdef _WIN32
  #define SHARED_PUBLIC __declspec(dllimport)
#else
  #define SHARED_PUBLIC __attribute__ ((visibility ("default")))
#endif

// should go in a webrtc.h
#define kMaxDataSizeSamples 7680
extern "C" SHARED_PUBLIC const char* ap_error(int);
extern "C" SHARED_PUBLIC void ap_setup(int, bool, bool, int, bool, int);
extern "C" SHARED_PUBLIC void ap_delete();
extern "C" SHARED_PUBLIC void ap_delay(int);
extern "C" SHARED_PUBLIC int ap_process_reverse(int, int, int16_t*);
extern "C" SHARED_PUBLIC int ap_process(int, int, int16_t*);

GST_DEBUG_CATEGORY_EXTERN (webrtc_audio_processor_debug);
#define GST_CAT_DEFAULT (webrtc_audio_processor_debug)

#define MAX_ADAPTER_SIZE (1*1024*1024)

#define DEFAULT_EXPLICIT_DELAY -1

static GstStaticPadTemplate gst_webrtc_audio_probe_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

static GstStaticPadTemplate gst_webrtc_audio_probe_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

G_DEFINE_TYPE (GstWebrtcAudioProbe, gst_webrtc_audio_probe,
    GST_TYPE_AUDIO_FILTER);

enum
{
  PROP_0,
  PROP_EXPLICIT_DELAY,
};

static gboolean
gst_webrtc_audio_probe_setup (GstAudioFilter * filter, const GstAudioInfo * info)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (filter);

  GST_LOG_OBJECT (self, "setting format to %s with %i Hz and %i channels",
      info->finfo->description, info->rate, info->channels);

  GST_WEBRTC_AUDIO_PROBE_LOCK (self);

  self->info = *info;

  /* WebRTC works with 10ms (.01s) buffers, compute period_size once */
  self->period_samples = info->rate / 100;
  self->period_size = self->period_samples * info->bpf;

  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_webrtc_audio_probe_stop (GstBaseTransform * btrans)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (btrans);

  GST_WEBRTC_AUDIO_PROBE_LOCK (self);
  gst_adapter_clear (self->adapter);
  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_webrtc_audio_probe_src_event (GstBaseTransform * btrans, GstEvent * event)
{
  GstBaseTransformClass *klass;
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (btrans);
  GstClockTime delay;

  klass = GST_BASE_TRANSFORM_CLASS (gst_webrtc_audio_probe_parent_class);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_LATENCY:
      gst_event_parse_latency (event, &delay);

      GST_WEBRTC_AUDIO_PROBE_LOCK (self);
      self->delay = (self->explicit_delay != -1) ? self->explicit_delay : (delay / GST_MSECOND);
      GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);
      
      GST_DEBUG_OBJECT (self, "***Estimated*** delay of %" GST_TIME_FORMAT, GST_TIME_ARGS (delay));
      GST_DEBUG_OBJECT (self, "Using a delay of %ims", self->delay);
      break;
    default:
      break;
  }

  return klass->src_event (btrans, event);
}

static void process_reverse(GstWebrtcAudioProbe * self, GstBuffer* buffer)
{
  GstAudioBuffer abuf;
  gst_audio_buffer_map (&abuf, &self->info, buffer, (GstMapFlags) GST_MAP_READWRITE);

  int16_t * const data = (int16_t * const) abuf.planes[0];
  int err;
  
  err = ap_process_reverse(self->info.rate, self->info.channels, data);

  if (err < 0)
    GST_WARNING_OBJECT (self, "Failed to reverse process audio: %s.",
        ap_error (err));

  gst_audio_buffer_unmap (&abuf);
}

static GstFlowReturn
gst_webrtc_audio_probe_transform_ip (GstBaseTransform * btrans,
    GstBuffer * buffer)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (btrans);

  GST_WEBRTC_AUDIO_PROBE_LOCK (self);

  gst_adapter_push (self->adapter, buffer);

  if (gst_adapter_available (self->adapter) > MAX_ADAPTER_SIZE)
    gst_adapter_flush (self->adapter, gst_adapter_available (self->adapter) - MAX_ADAPTER_SIZE);

  GstBuffer* buf;

  do {
    if (gst_adapter_available (self->adapter) < self->period_size)
      buf = NULL;
    else
    {
      buf = gst_adapter_take_buffer (self->adapter, self->period_size);
      ap_delay (self->delay);
      process_reverse(self, buf);
    }
  }
  while (buf);

  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return GST_FLOW_OK;
}

GstBuffer*
gst_webrtc_audio_probe_read (GstWebrtcAudioProbe * self, guint * delay)
{
  GstBuffer* buffer;
  
  GST_WEBRTC_AUDIO_PROBE_LOCK (self);
  
  if (gst_adapter_available (self->adapter) < self->period_size)
    buffer = NULL;
  else
  {
    buffer = gst_adapter_take_buffer (self->adapter, self->period_size);;
    * delay = self->delay;
  }

  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return buffer;
}

static void
gst_webrtc_audio_probe_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_EXPLICIT_DELAY:
      self->explicit_delay =
          g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_webrtc_audio_probe_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_EXPLICIT_DELAY:
      g_value_set_int (value, self->explicit_delay);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_webrtc_audio_probe_finalize (GObject * object)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (object);

  gst_object_unref (self->adapter);
  self->adapter = NULL;

  G_OBJECT_CLASS (gst_webrtc_audio_probe_parent_class)->finalize (object);
}

static void
gst_webrtc_audio_probe_init (GstWebrtcAudioProbe * self)
{
  self->adapter = gst_adapter_new ();
  gst_audio_info_init (&self->info);
  g_mutex_init (&self->lock);

  self->delay = (self->explicit_delay != -1) ? self->explicit_delay : 0;
}

static void
gst_webrtc_audio_probe_class_init (GstWebrtcAudioProbeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *btrans_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstAudioFilterClass *audiofilter_class = GST_AUDIO_FILTER_CLASS (klass);

  gobject_class->finalize = gst_webrtc_audio_probe_finalize;
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_get_property);

  btrans_class->passthrough_on_same_caps = TRUE;
  btrans_class->src_event = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_src_event);
  btrans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_transform_ip);
  btrans_class->stop = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_stop);

  audiofilter_class->setup = GST_DEBUG_FUNCPTR (gst_webrtc_audio_probe_setup);

  gst_element_class_add_static_pad_template (element_class, &gst_webrtc_audio_probe_src_template);
  gst_element_class_add_static_pad_template (element_class, &gst_webrtc_audio_probe_sink_template);

  g_object_class_install_property (gobject_class,
      PROP_EXPLICIT_DELAY,
      g_param_spec_int ("delay", "Explicit Delay",
          "Explicit delay between far end and near end in ms.",
          -1, 1500, DEFAULT_EXPLICIT_DELAY, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  gst_element_class_set_static_metadata (element_class,
      "Audio probe",
      "Generic/Audio",
      "Gathers playback buffers for webrtcaudioprocessor",
      "Guillaume Cartier <gucartier@gmail.com>");
}
