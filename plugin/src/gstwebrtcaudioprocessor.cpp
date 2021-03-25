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
 * SECTION:element-webrtcaudioprocessor
 * @short_description: Audio Filter using WebRTC Audio Processing library
 *
 * A voice enhancement filter based on WebRTC Audio Processing library. This
 * library provides a whide variety of enhancement algorithms. This element
 * tries to enable as much as possible. The currently enabled enhancements are
 * High Pass Filter, Echo Canceller, Noise Suppression, Automatic Gain Control.
 *
 * While webrtcaudioprocessor element can be used alone, there is an exception for the
 * echo canceller. The audio canceller need to be aware of the far end streams
 * that are played to loud speakers. For this, you must place a webrtcaudioprobe
 * element at that far end. Note that the sample rate must match between
 * webrtcaudioprocessor and the webrtaudioprobe. Though, the number of channels can differ.
 *
 * # Example launch line
 *
 * As a convenience, the echo canceller can be tested using an echo loop. In
 * this configuration, one would expect a single echo to be heard.
 *
 * |[
 * gst-launch-1.0 pulsesrc ! webrtcaudioprocessor ! webrtcaudioprobe ! pulsesink
 * ]|
 *
 * In real environment, you'll place the probe before the playback, but only
 * process the far end streams. The processor should be placed as close as possible
 * to the audio capture. The following pipeline is astracted and does not
 * represent a real pipeline.
 *
 * |[
 * gst-launch-1.0 far-end-src ! audio/x-raw,rate=48000 ! webrtcaudioprobe ! pulsesink \
 *                pulsesrc ! audio/x-raw,rate=48000 ! webrtcaudioprocessor ! far-end-sink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>

#include "gst/webrtcaudioprocessing/gstwebrtcaudioprocessor.h"
#include "gst/webrtcaudioprocessing/gstwebrtcaudioprobe.h"

#ifdef _WIN32
  #define SHARED_PUBLIC __declspec(dllimport)
#else
  #define SHARED_PUBLIC __attribute__ ((visibility ("default")))
#endif

// should go in a webrtc.h
#define kMaxDataSizeSamples 7680
#define NSL_LOW 0
#define NSL_MODERATE 1
#define NSL_HIGH 2
#define NSL_VERYHIGH 3
#define LS_VERBOSE 0
#define LS_INFO 1
#define LS_WARNING 2
#define LS_ERROR 3
#define LS_NONE 4
extern "C" SHARED_PUBLIC const char* ap_error(int);
extern "C" SHARED_PUBLIC void ap_setup(int, bool, bool, int, bool, int);
extern "C" SHARED_PUBLIC void ap_delete();
extern "C" SHARED_PUBLIC void ap_delay(int);
extern "C" SHARED_PUBLIC int ap_process_reverse(int, int, int16_t*);
extern "C" SHARED_PUBLIC int ap_process(int, int, int16_t*);

GST_DEBUG_CATEGORY (webrtc_audio_processor_debug);
#define GST_CAT_DEFAULT (webrtc_audio_processor_debug)

#define DEFAULT_PROCESSING_RATE 32000
#define DEFAULT_VOICE_DETECTION FALSE
#define DEFAULT_GAIN_CONTROLLER TRUE

static GstStaticPadTemplate gst_webrtc_audio_processor_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

static GstStaticPadTemplate gst_webrtc_audio_processor_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 32000, 16000, 8000 }, "
        "channels = (int) [1, MAX]")
    );

typedef int GstWebrtcAudioProcessingLoggingSeverity;
#define GST_TYPE_WEBRTC_LOGGING_SEVERITY \
    (gst_webrtc_logging_severity_get_type ())
static GType
gst_webrtc_logging_severity_get_type (void)
{
  static GType logging_severity_type = 0;
  static const GEnumValue severity_types[] = {
    {LS_VERBOSE, "Verbose logging", "verbose"},
    {LS_INFO, "Info logging", "info"},
    {LS_WARNING, "Warning logging", "warning"},
    {LS_ERROR, "Error logging", "error"},
    {LS_NONE, "No logging", "none"},
    {0, NULL, NULL}
  };

  if (!logging_severity_type) {
    logging_severity_type =
        g_enum_register_static ("GstWebrtcAudioProcessingLoggingSeverity", severity_types);
  }
  return logging_severity_type;
}

typedef int GstWebrtcAudioProcessingNoiseSuppressionLevel;
#define GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL \
    (gst_webrtc_noise_suppression_level_get_type ())
static GType
gst_webrtc_noise_suppression_level_get_type (void)
{
  static GType suppression_level_type = 0;
  static const GEnumValue level_types[] = {
    {NSL_LOW, "Low Suppression", "low"},
    {NSL_MODERATE, "Moderate Suppression", "moderate"},
    {NSL_HIGH, "High Suppression", "high"},
    {NSL_VERYHIGH, "Very High Suppression", "very-high"},
    {0, NULL, NULL}
  };

  if (!suppression_level_type) {
    suppression_level_type =
        g_enum_register_static ("GstWebrtcAudioProcessingNoiseSuppressionLevel", level_types);
  }
  return suppression_level_type;
}

enum
{
  PROP_0,
  PROP_LOGGING_SEVERITY,
  PROP_PROCESSING_RATE,
  PROP_ECHO_CANCEL,
  PROP_NOISE_SUPPRESSION,
  PROP_NOISE_SUPPRESSION_LEVEL,
#ifdef _WAIT_VAD
  PROP_VOICE_DETECTION,
#endif
  PROP_GAIN_CONTROLLER,
};

GMutex webrtcaudioprocessing_mutex;

/**
 * GstWebrtcAudioProcessor:
 *
 * The adder object structure.
 */
struct _GstWebrtcAudioProcessor
{
  GstAudioFilter element;

  /* Protected by the object lock */
  GstAudioInfo info;
  guint period_size;
  guint period_samples;
  gboolean stream_has_voice;

  /* Protected by the stream lock */
  GstAdapter *adapter;

  /* Properties */
  int logging_severity;
  int processing_rate;
  gboolean echo_cancel;
  gboolean noise_suppression;
  int noise_suppression_level;
#ifdef _WAIT_VAD
  gboolean voice_detection;
#endif
  gboolean gain_controller;
};

G_DEFINE_TYPE (GstWebrtcAudioProcessor, gst_webrtc_audio_processor, GST_TYPE_AUDIO_FILTER);

#ifdef _WAIT_VAD
static void
gst_webrtc_vad_post_message (GstWebrtcAudioProcessor *self, GstClockTime timestamp,
    gboolean stream_has_voice)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM_CAST (self);
  GstStructure *s;
  GstClockTime stream_time;

  stream_time = gst_segment_to_stream_time (&trans->segment, GST_FORMAT_TIME,
      timestamp);

  s = gst_structure_new ("voice-activity",
      "stream-time", G_TYPE_UINT64, stream_time,
      "stream-has-voice", G_TYPE_BOOLEAN, stream_has_voice, NULL);

  GST_LOG_OBJECT (self, "Posting voice activity message, stream %s voice",
      stream_has_voice ? "now has" : "no longer has");

  gst_element_post_message (GST_ELEMENT (self),
      gst_message_new_element (GST_OBJECT (self), s));
}
#endif

static GstFlowReturn
gst_webrtc_audio_processor_process_stream (GstWebrtcAudioProcessor * self,
    GstBuffer * buffer)
{
  GstAudioBuffer abuf;
  gint err;

  if (!gst_audio_buffer_map (&abuf, &self->info, buffer,
          (GstMapFlags) GST_MAP_READWRITE)) {
    gst_buffer_unref (buffer);
    return GST_FLOW_ERROR;
  }

  int16_t * const data = (int16_t * const) abuf.planes[0];
  
  err = ap_process(self->info.rate, self->info.channels, data);

  if (err < 0) {
    GST_WARNING_OBJECT (self, "Failed to process audio: %s.",
        ap_error (err));
  } else {
#ifdef _WAIT_VAD
    if (self->voice_detection) {
      gboolean stream_has_voice = apm->voice_detection ()->stream_has_voice ();

      if (stream_has_voice != self->stream_has_voice)
        gst_webrtc_vad_post_message (self, GST_BUFFER_PTS (buffer), stream_has_voice);

      self->stream_has_voice = stream_has_voice;
    }
#endif
  }

  gst_audio_buffer_unmap (&abuf);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_webrtc_audio_processor_submit_input_buffer (GstBaseTransform * btrans,
    gboolean is_discont, GstBuffer * buffer)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);

  buffer = gst_buffer_make_writable (buffer);

  if (is_discont) {
    GST_DEBUG_OBJECT (self,
        "Received discont, clearing adapter.");
    gst_adapter_clear (self->adapter);
  }

  gst_adapter_push (self->adapter, buffer);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_webrtc_audio_processor_generate_output (GstBaseTransform * btrans, GstBuffer ** outbuf)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);
  GstFlowReturn ret;
  gboolean not_enough;

  not_enough = gst_adapter_available (self->adapter) < self->period_size;

  if (not_enough) {
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = gst_adapter_take_buffer (self->adapter, self->period_size);
  ret = gst_webrtc_audio_processor_process_stream (self, *outbuf);

  return ret;
}

static gboolean
gst_webrtc_audio_processor_start (GstBaseTransform * btrans)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);

  GST_OBJECT_LOCK (self);
  ap_setup(self->processing_rate, self->echo_cancel, self->noise_suppression, self->noise_suppression_level, self->gain_controller, self->logging_severity);
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_webrtc_audio_processor_setup (GstAudioFilter * filter, const GstAudioInfo * info)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (filter);

  GST_LOG_OBJECT (self, "setting format to %s with %i Hz and %i channels",
      info->finfo->description, info->rate, info->channels);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);

  self->info = *info;

  /* WebRTC works with 10ms (.01s) buffers, compute period_size once */
  self->period_samples = info->rate / 100;
  self->period_size = self->period_samples * info->bpf;

#ifdef _WAIT
  /* input stream */
  pconfig.streams[webrtc::ProcessingConfig::kInputStream] =
      webrtc::StreamConfig (info->rate, info->channels, false);
  /* output stream */
  pconfig.streams[webrtc::ProcessingConfig::kOutputStream] =
      webrtc::StreamConfig (info->rate, info->channels, false);
  /* reverse input stream */
  pconfig.streams[webrtc::ProcessingConfig::kReverseInputStream] =
      webrtc::StreamConfig (probe_info.rate, probe_info.channels, false);
  /* reverse output stream */
  pconfig.streams[webrtc::ProcessingConfig::kReverseOutputStream] =
      webrtc::StreamConfig (probe_info.rate, probe_info.channels, false);
#endif

#ifdef _WAIT_VAD
  if (self->voice_detection) {
    self->stream_has_voice = FALSE;

    apm->voice_detection ()->Enable (true);
  }
#endif

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_webrtc_audio_processor_stop (GstBaseTransform * btrans)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);

  ap_delete();

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static void
gst_webrtc_audio_processor_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LOGGING_SEVERITY:
      self->logging_severity =
          (GstWebrtcAudioProcessingLoggingSeverity) g_value_get_enum (value);
      break;
    case PROP_PROCESSING_RATE:
      self->processing_rate = g_value_get_int (value);
      break;
    case PROP_ECHO_CANCEL:
      self->echo_cancel = g_value_get_boolean (value);
      break;
    case PROP_NOISE_SUPPRESSION:
      self->noise_suppression = g_value_get_boolean (value);
      break;
    case PROP_NOISE_SUPPRESSION_LEVEL:
      self->noise_suppression_level =
          (GstWebrtcAudioProcessingNoiseSuppressionLevel) g_value_get_enum (value);
      break;
#ifdef _WAIT_VAD
    case PROP_VOICE_DETECTION:
      self->voice_detection = g_value_get_boolean (value);
      break;
#endif
    case PROP_GAIN_CONTROLLER:
      self->gain_controller = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_webrtc_audio_processor_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LOGGING_SEVERITY:
      g_value_set_enum (value, self->logging_severity);
      break;
    case PROP_PROCESSING_RATE:
      g_value_set_int (value, self->processing_rate);
      break;
    case PROP_ECHO_CANCEL:
      g_value_set_boolean (value, self->echo_cancel);
      break;
    case PROP_NOISE_SUPPRESSION:
      g_value_set_boolean (value, self->noise_suppression);
      break;
    case PROP_NOISE_SUPPRESSION_LEVEL:
      g_value_set_enum (value, self->noise_suppression_level);
      break;
#ifdef _WAIT_VAD
    case PROP_VOICE_DETECTION:
      g_value_set_boolean (value, self->voice_detection);
      break;
#endif
    case PROP_GAIN_CONTROLLER:
      g_value_set_boolean (value, self->gain_controller);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}


static void
gst_webrtc_audio_processor_finalize (GObject * object)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (object);

  gst_object_unref (self->adapter);

  G_OBJECT_CLASS (gst_webrtc_audio_processor_parent_class)->finalize (object);
}

static void
gst_webrtc_audio_processor_init (GstWebrtcAudioProcessor * self)
{
  self->adapter = gst_adapter_new ();
  gst_audio_info_init (&self->info);
}

static void
gst_webrtc_audio_processor_class_init (GstWebrtcAudioProcessorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *btrans_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstAudioFilterClass *audiofilter_class = GST_AUDIO_FILTER_CLASS (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_finalize);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_get_property);

  btrans_class->passthrough_on_same_caps = FALSE;
  btrans_class->start = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_start);
  btrans_class->stop = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_stop);
  btrans_class->submit_input_buffer =
      GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_submit_input_buffer);
  btrans_class->generate_output =
      GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_generate_output);

  audiofilter_class->setup = GST_DEBUG_FUNCPTR (gst_webrtc_audio_processor_setup);

  gst_element_class_add_static_pad_template (element_class,
      &gst_webrtc_audio_processor_src_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_webrtc_audio_processor_sink_template);
  gst_element_class_set_static_metadata (element_class,
      "Voice Processor (AGC, AEC, filters, etc.)",
      "Generic/Audio",
      "Processes voice with WebRTC Audio Processing Library",
      "Nicolas Dufresne <nicolas.dufresne@collabora.com>");

  g_object_class_install_property (gobject_class,
      PROP_LOGGING_SEVERITY,
      g_param_spec_enum ("logging-severity", "Logging Severity",
          "Controls the amount of logging.", GST_TYPE_WEBRTC_LOGGING_SEVERITY,
          LS_WARNING,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_PROCESSING_RATE,
      g_param_spec_int ("processing-rate", "Maximum processing rate",
          "Maximum processing rate. May only be set to 32000 or 48000.",
          32000, 48000, DEFAULT_PROCESSING_RATE, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_ECHO_CANCEL,
      g_param_spec_boolean ("echo-cancel", "Echo Cancel",
          "Enable or disable echo canceller", FALSE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_NOISE_SUPPRESSION,
      g_param_spec_boolean ("noise-suppression", "Noise Suppression",
          "Enable or disable noise suppression", FALSE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_NOISE_SUPPRESSION_LEVEL,
      g_param_spec_enum ("noise-suppression-level", "Noise Suppression Level",
          "Controls the aggressiveness of the suppression. Increasing the "
          "level will reduce the noise level at the expense of a higher "
          "speech distortion.", GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL,
          NSL_MODERATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

#ifdef _WAIT_VAD
  g_object_class_install_property (gobject_class,
      PROP_VOICE_DETECTION,
      g_param_spec_boolean ("voice-detection", "Voice Detection",
          "Enable or disable the voice activity detector",
          DEFAULT_VOICE_DETECTION, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));
#endif

  g_object_class_install_property (gobject_class,
      PROP_GAIN_CONTROLLER,
      g_param_spec_boolean ("gain-controller", "Gain Controller",
          "Enable or disable the gain controller",
          DEFAULT_GAIN_CONTROLLER, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  gst_type_mark_as_plugin_api (GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL, (GstPluginAPIFlags) 0);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT
      (webrtc_audio_processor_debug, "webrtcaudioprocessor", 0, "libwebrtcaudioprocessor wrapping elements");

  if (!gst_element_register (plugin, "webrtcaudioprocessor", GST_RANK_NONE,
          GST_TYPE_WEBRTC_AUDIO_PROCESSOR)) {
    return FALSE;
  }
  if (!gst_element_register (plugin, "webrtcaudioprobe", GST_RANK_NONE,
          GST_TYPE_WEBRTC_AUDIO_PROBE)) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    webrtcaudioprocessing,
    "Voice processing using WebRTC Audio Processing Library",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
