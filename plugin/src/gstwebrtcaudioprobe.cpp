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

#define kMaxDataSizeSamples 7680

GST_DEBUG_CATEGORY_EXTERN (webrtc_audio_processor_debug);
#define GST_CAT_DEFAULT (webrtc_audio_processor_debug)

#define MAX_ADAPTER_SIZE (1*1024*1024)

#define DEFAULT_EXPLICIT_LATENCY -1
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

G_LOCK_DEFINE_STATIC (gst_aec_probes);
static GList *gst_aec_probes = NULL;

G_DEFINE_TYPE (GstWebrtcAudioProbe, gst_webrtc_audio_probe,
    GST_TYPE_AUDIO_FILTER);

enum
{
  PROP_0,
  PROP_EXPLICIT_LATENCY,
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

  /* WebRTC library works with 10ms buffers, compute once this size */
  self->period_samples = info->rate / 100;
  self->period_size = self->period_samples * info->bpf;

  if ((kMaxDataSizeSamples * 2) < self->period_size)
    goto period_too_big;

  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return TRUE;

period_too_big:
  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);
  GST_WARNING_OBJECT (self, "webrtcaudioprocessor format produce too big period "
      "(maximum is %d samples and we have %u samples), "
      "reduce the number of channels or the rate.",
      kMaxDataSizeSamples, self->period_size / 2);
  return FALSE;
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
  GstClockTime latency;
  GstClockTime upstream_latency = 0;
  GstQuery *query;

  klass = GST_BASE_TRANSFORM_CLASS (gst_webrtc_audio_probe_parent_class);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_LATENCY:
      gst_event_parse_latency (event, &latency);
      query = gst_query_new_latency ();

      if (gst_pad_query (btrans->srcpad, query)) {
        gst_query_parse_latency (query, NULL, &upstream_latency, NULL);

        if (!GST_CLOCK_TIME_IS_VALID (upstream_latency))
          upstream_latency = 0;
      }

      GST_WEBRTC_AUDIO_PROBE_LOCK (self);
      self->latency = (self->explicit_latency != -1) ? self->explicit_latency * 1000000 : latency;
      self->delay = (self->explicit_delay != -1) ? self->explicit_delay : (upstream_latency / GST_MSECOND);
      GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);
      
      GST_DEBUG_OBJECT (self, "***Estimated*** latency of %" GST_TIME_FORMAT
          " and delay of %ims", GST_TIME_ARGS (latency),
          (gint) (upstream_latency / GST_MSECOND));

      GST_DEBUG_OBJECT (self, "Using a latency of %" GST_TIME_FORMAT
          " and delay of %ims", GST_TIME_ARGS (self->latency),
          self->delay);
      break;
    default:
      break;
  }

  return klass->src_event (btrans, event);
}

static GstFlowReturn
gst_webrtc_audio_probe_transform_ip (GstBaseTransform * btrans,
    GstBuffer * buffer)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (btrans);
  GstBuffer *newbuf = NULL;

  GST_WEBRTC_AUDIO_PROBE_LOCK (self);
  newbuf = gst_buffer_copy (buffer);
  /* Moves the buffer timestamp to be in Running time */
  GST_BUFFER_PTS (newbuf) = gst_segment_to_running_time (&btrans->segment,
      GST_FORMAT_TIME, GST_BUFFER_PTS (buffer));

  gst_adapter_push (self->adapter, newbuf);

  if (gst_adapter_available (self->adapter) > MAX_ADAPTER_SIZE)
    gst_adapter_flush (self->adapter,
        gst_adapter_available (self->adapter) - MAX_ADAPTER_SIZE);

  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return GST_FLOW_OK;
}

static void
gst_webrtc_audio_probe_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWebrtcAudioProbe *self = GST_WEBRTC_AUDIO_PROBE (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_EXPLICIT_LATENCY:
      self->explicit_latency =
          g_value_get_int (value);
      break;
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
    case PROP_EXPLICIT_LATENCY:
      g_value_set_int (value, self->explicit_latency);
      break;
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

  G_LOCK (gst_aec_probes);
  gst_aec_probes = g_list_remove (gst_aec_probes, self);
  G_UNLOCK (gst_aec_probes);

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

  self->latency = GST_CLOCK_TIME_NONE;
      self->latency = (self->explicit_latency != -1) ? self->explicit_latency * 1000000 : 0;
      self->delay = (self->explicit_delay != -1) ? self->explicit_delay : 0;

  G_LOCK (gst_aec_probes);
  gst_aec_probes = g_list_prepend (gst_aec_probes, self);
  G_UNLOCK (gst_aec_probes);
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
      PROP_EXPLICIT_LATENCY,
      g_param_spec_int ("latency", "Explicit Latency",
          "Explicit latency in ms.",
          -1, 1500, DEFAULT_EXPLICIT_LATENCY, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

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
      "Nicolas Dufresne <nicolas.dufrsesne@collabora.com>");
}

GstWebrtcAudioProbe *
gst_webrtc_acquire_audio_probe (const gchar * name)
{
  GstWebrtcAudioProbe *ret = NULL;
  GList *l;

  G_LOCK (gst_aec_probes);
  for (l = gst_aec_probes; l; l = l->next) {
    GstWebrtcAudioProbe *probe = GST_WEBRTC_AUDIO_PROBE (l->data);

    GST_WEBRTC_AUDIO_PROBE_LOCK (probe);
    if (!probe->acquired && g_strcmp0 (GST_OBJECT_NAME (probe), name) == 0) {
      probe->acquired = TRUE;
      ret = GST_WEBRTC_AUDIO_PROBE (gst_object_ref (probe));
      GST_WEBRTC_AUDIO_PROBE_UNLOCK (probe);
      break;
    }
    GST_WEBRTC_AUDIO_PROBE_UNLOCK (probe);
  }
  G_UNLOCK (gst_aec_probes);

  return ret;
}

void
gst_webrtc_release_audio_probe (GstWebrtcAudioProbe * probe)
{
  GST_WEBRTC_AUDIO_PROBE_LOCK (probe);
  probe->acquired = FALSE;
  GST_WEBRTC_AUDIO_PROBE_UNLOCK (probe);
  gst_object_unref (probe);
}

gint
gst_webrtc_audio_probe_read (GstWebrtcAudioProbe * self, GstClockTime rec_time, gint * rate, int16_t * data)
{
  GstClockTimeDiff diff;
  gsize avail, skip, offset, size;
  gint delay = -1;

  GST_WEBRTC_AUDIO_PROBE_LOCK (self);

  if (!GST_CLOCK_TIME_IS_VALID (self->latency) ||
      !GST_AUDIO_INFO_IS_VALID (&self->info))
    goto done;

  avail = gst_adapter_available (self->adapter) / self->info.bpf;

  if (avail == 0) {
    diff = G_MAXINT64;
  } else {
    GstClockTime play_time;
    guint64 distance;

    play_time = gst_adapter_prev_pts (self->adapter, &distance);
    distance /= self->info.bpf;

    if (GST_CLOCK_TIME_IS_VALID (play_time)) {
      play_time += gst_util_uint64_scale_int (distance, GST_SECOND, self->info.rate);
      play_time += self->latency;

      diff = GST_CLOCK_DIFF (rec_time, play_time) / GST_MSECOND;
    } else {
      /* We have no timestamp, assume perfect delay */
      diff = self->delay;
    }
  }

  if (diff > self->delay) {
    skip = (diff - self->delay) * self->info.rate / 1000;
    skip = MIN (self->period_samples, skip);
    offset = 0;
  } else {
    skip = 0;
    offset = (self->delay - diff) * self->info.rate / 1000;
    offset = MIN (avail, offset);
  }

  size = MIN (avail - offset, self->period_samples - skip);

  skip *= self->info.bpf;
  offset *= self->info.bpf;
  size *= self->info.bpf;

  if (size < self->period_size)
    memset (data, 0, self->period_size);

  if (size) {
    gst_adapter_copy (self->adapter, (guint8 *) data + skip, offset, size);
    gst_adapter_flush (self->adapter, offset + size);
  }
    
  *rate = self->info.rate;

  delay = self->delay;

done:
  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self);

  return delay;
}
