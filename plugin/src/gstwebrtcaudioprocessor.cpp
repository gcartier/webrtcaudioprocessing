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
 * The probe is found by the processor element using it's object name. By default,
 * webrtcaudioprocessor looks for webrtcaudioprobe0, which means it just work if you have
 * a single probe and the processor.
 *
 * The probe can only be used within the same top level GstPipeline.
 * Additionally, to simplify the code, the probe element must be created
 * before the processor sink pad is activated. It does not need to be in any
 * particular state and does not even need to be added to the pipeline yet.
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
#include <stdio.h>

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
#define LS_NONE 0
#define LS_ERROR 1
#define LS_WARNING 2
#define LS_INFO 3
#define LS_VERBOSE 4
extern "C" SHARED_PUBLIC int  ap_setup(int, bool, bool, int, int);
extern "C" SHARED_PUBLIC void ap_delete();
extern "C" SHARED_PUBLIC void ap_delay(int);
extern "C" SHARED_PUBLIC int  ap_process_reverse(int, int, int16_t*);
extern "C" SHARED_PUBLIC int  ap_process(int, int, int16_t*);

GST_DEBUG_CATEGORY (webrtc_audio_processor_debug);
#define GST_CAT_DEFAULT (webrtc_audio_processor_debug)

#define DEFAULT_PROCESSING_RATE 32000
#define DEFAULT_TARGET_LEVEL_DBFS 3
#define DEFAULT_COMPRESSION_GAIN_DB 9
#define DEFAULT_STARTUP_MIN_VOLUME 12
#define DEFAULT_LIMITER FALSE
#ifdef _WAIT
#define DEFAULT_GAIN_CONTROL_MODE webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital
#endif
#define DEFAULT_VOICE_DETECTION FALSE
#define DEFAULT_VOICE_DETECTION_FRAME_SIZE_MS 10

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

#ifdef _WAIT
typedef webrtc::AudioProcessing::Config::GainController1::Mode GstWebrtcAudioProcessingGainControlMode;
#define GST_TYPE_WEBRTC_GAIN_CONTROL_MODE \
    (gst_webrtc_gain_control_mode_get_type ())
static GType
gst_webrtc_gain_control_mode_get_type (void)
{
  static GType gain_control_mode_type = 0;
  static const GEnumValue mode_types[] = {
    {webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital, "Adaptive Digital", "adaptive-digital"},
    {webrtc::AudioProcessing::Config::GainController1::kFixedDigital, "Fixed Digital", "fixed-digital"},
    {0, NULL, NULL}
  };

  if (!gain_control_mode_type) {
    gain_control_mode_type =
        g_enum_register_static ("GstWebrtcAudioProcessingGainControlMode", mode_types);
  }
  return gain_control_mode_type;
}
#endif

enum
{
  PROP_0,
  PROP_PROBE,
  PROP_LOGGING_SEVERITY,
  PROP_PROCESSING_RATE,
  PROP_HIGH_PASS_FILTER,
  PROP_ECHO_CANCEL,
  PROP_NOISE_SUPPRESSION,
  PROP_NOISE_SUPPRESSION_LEVEL,
  PROP_GAIN_CONTROL,
  PROP_TARGET_LEVEL_DBFS,
  PROP_COMPRESSION_GAIN_DB,
  PROP_STARTUP_MIN_VOLUME,
  PROP_LIMITER,
  PROP_GAIN_CONTROL_MODE,
  PROP_VOICE_DETECTION,
  PROP_VOICE_DETECTION_FRAME_SIZE_MS,
};

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

  /* Protected by the object lock */
  gchar *probe_name;
  GstWebrtcAudioProbe *probe;

  /* Properties */
  int logging_severity;
  int processing_rate;
  gboolean high_pass_filter;
  gboolean echo_cancel;
  gboolean noise_suppression;
  int noise_suppression_level;
  gboolean gain_control;
  gint target_level_dbfs;
  gint compression_gain_db;
  gint startup_min_volume;
  gboolean limiter;
#ifdef _WAIT
  webrtc::AudioProcessing::Config::GainController1::Mode gain_control_mode;
#endif
  gboolean voice_detection;
  gint voice_detection_frame_size_ms;
};

G_DEFINE_TYPE (GstWebrtcAudioProcessor, gst_webrtc_audio_processor, GST_TYPE_AUDIO_FILTER);

static const gchar *
webrtc_error_to_string (gint err)
{
#ifdef _WAIT
  const gchar *str = "unknown error";

  switch (err) {
    case webrtc::AudioProcessing::kNoError:
      str = "success";
      break;
    case webrtc::AudioProcessing::kUnspecifiedError:
      str = "unspecified error";
      break;
    case webrtc::AudioProcessing::kCreationFailedError:
      str = "creating failed";
      break;
    case webrtc::AudioProcessing::kUnsupportedComponentError:
      str = "unsupported component";
      break;
    case webrtc::AudioProcessing::kUnsupportedFunctionError:
      str = "unsupported function";
      break;
    case webrtc::AudioProcessing::kNullPointerError:
      str = "null pointer";
      break;
    case webrtc::AudioProcessing::kBadParameterError:
      str = "bad parameter";
      break;
    case webrtc::AudioProcessing::kBadSampleRateError:
      str = "bad sample rate";
      break;
    case webrtc::AudioProcessing::kBadDataLengthError:
      str = "bad data length";
      break;
    case webrtc::AudioProcessing::kBadNumberChannelsError:
      str = "bad number of channels";
      break;
    case webrtc::AudioProcessing::kFileError:
      str = "file IO error";
      break;
    case webrtc::AudioProcessing::kStreamParameterNotSetError:
      str = "stream parameter not set";
      break;
    case webrtc::AudioProcessing::kNotEnabledError:
      str = "not enabled";
      break;
    case webrtc::AudioProcessing::kBadStreamParameterWarning:
      str = "bad stream parameter warning";
      break;
    default:
      break;
  }

  return str;
#else
  return "ap_error";
#endif
}

static GstBuffer *
gst_webrtc_audio_processor_take_buffer (GstWebrtcAudioProcessor * self)
{
  GstBuffer *buffer;
  GstClockTime timestamp;
  guint64 distance;
  gboolean at_discont;

  timestamp = gst_adapter_prev_pts (self->adapter, &distance);
  distance /= self->info.bpf;

  timestamp += gst_util_uint64_scale_int (distance, GST_SECOND, self->info.rate);

  buffer = gst_adapter_take_buffer (self->adapter, self->period_size);
  at_discont = (gst_adapter_pts_at_discont (self->adapter) == timestamp);

  GST_BUFFER_PTS (buffer) = timestamp;
  GST_BUFFER_DURATION (buffer) = 10 * GST_MSECOND;

  if (at_discont && distance == 0) {
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
  } else {
    GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DISCONT);
  }

  return buffer;
}

static GstFlowReturn
gst_webrtc_audio_processor_analyze_reverse_stream (GstWebrtcAudioProcessor * self,
    GstClockTime rec_time)
{
  GstWebrtcAudioProbe *probe = NULL;
  int16_t data[kMaxDataSizeSamples];
  GstFlowReturn ret = GST_FLOW_OK;
  gint probe_rate;
  gint err, delay;

  GST_OBJECT_LOCK (self);
  if (self->echo_cancel)
    probe = GST_WEBRTC_AUDIO_PROBE (g_object_ref (self->probe));
  GST_OBJECT_UNLOCK (self);

  /* If echo cancellation is disabled */
  if (!probe)
    return GST_FLOW_OK;

  delay = gst_webrtc_audio_probe_read (probe, rec_time, &probe_rate, data);
  ap_delay(delay);

  if (delay < 0)
    goto done;

  if (probe_rate != self->info.rate) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT,
        ("Audio probe has rate %i , while the processor is running at rate %i,"
         " use a caps filter to ensure those are the same.",
         probe_rate, self->info.rate), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }

  err = ap_process_reverse(self->info.rate, self->info.channels, data);
  if (err < 0)
    GST_WARNING_OBJECT (self, "Reverse stream analyses failed: %s.",
        webrtc_error_to_string (err));

done:
  gst_object_unref (probe);

  return ret;
}

/* CONVERT
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
*/

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
    GST_WARNING_OBJECT (self, "Failed to filter the audio: %s.",
        webrtc_error_to_string (err));
  } else {
  /* CONVERT
    if (self->voice_detection) {
      gboolean stream_has_voice = apm->voice_detection ()->stream_has_voice ();

      if (stream_has_voice != self->stream_has_voice)
        gst_webrtc_vad_post_message (self, GST_BUFFER_PTS (buffer), stream_has_voice);

      self->stream_has_voice = stream_has_voice;
    }
    */
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
  GST_BUFFER_PTS (buffer) = gst_segment_to_running_time (&btrans->segment,
      GST_FORMAT_TIME, GST_BUFFER_PTS (buffer));

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

  *outbuf = gst_webrtc_audio_processor_take_buffer (self);
  ret = gst_webrtc_audio_processor_analyze_reverse_stream (self, GST_BUFFER_PTS (*outbuf));

  if (ret == GST_FLOW_OK)
    ret = gst_webrtc_audio_processor_process_stream (self, *outbuf);

  return ret;
}

extern
void gst_webrtc_audio_processor_set_probe ()
{
  // printf("SET PROBE\n");
}

static gboolean
gst_webrtc_audio_processor_start (GstBaseTransform * btrans)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);
#ifdef _WAIT
  webrtc::AudioProcessing::Config config;
  gint err = 0;
  
  GST_OBJECT_LOCK (self);
  
  GST_DEBUG_OBJECT (self, "Setting processing rate to %d",
        self->processing_rate);
  config.pipeline.maximum_internal_processing_rate = self->processing_rate;
  
  rtc::LogMessage::LogToDebug(self->logging_severity);

  if (self->high_pass_filter) {
    GST_DEBUG_OBJECT (self, "Enabling High Pass filter");
    config.high_pass_filter.enabled = true;
  }

  if (self->echo_cancel) {
    GST_DEBUG_OBJECT (self, "Enabling Echo Cancellation");
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false;
  }

  if (self->noise_suppression) {
    GST_DEBUG_OBJECT (self, "Enabling Noise Suppression");
    config.noise_suppression.enabled = true;
    config.noise_suppression.level = self->noise_suppression_level;
  }
  
  if (self->gain_control) {
    GEnumClass *mode_class = (GEnumClass *)
        g_type_class_ref (GST_TYPE_WEBRTC_GAIN_CONTROL_MODE);

    GST_DEBUG_OBJECT (self, "Enabling Digital Gain Control, target level "
        "dBFS %d, compression gain dB %d, limiter %senabled, mode: %s",
        self->target_level_dbfs, self->compression_gain_db,
        self->limiter ? "" : "NOT ",
        g_enum_get_value (mode_class, self->gain_control_mode)->value_name);

    g_type_class_unref (mode_class);

    // apm->gain_control ()->set_mode (self->gain_control_mode);
    // apm->gain_control ()->set_target_level_dbfs (self->target_level_dbfs);
    // apm->gain_control ()->set_compression_gain_db (self->compression_gain_db);
    // apm->gain_control ()->enable_limiter (self->limiter);
    config.gain_controller1.enabled = true;
  }

  self->apm = webrtc::AudioProcessingBuilder().Create();

  self->apm->ApplyConfig(config);
  
  err = self->apm->Initialize();
#else
  gint err = 0;
  GST_OBJECT_LOCK (self);
  err = ap_setup(32000, true, true, self->noise_suppression_level, self->logging_severity);
#endif

  if (err < 0)
    goto initialize_failed;
  
  if (self->echo_cancel) {
    self->probe = gst_webrtc_acquire_audio_probe (self->probe_name);

    if (self->probe == NULL) {
      GST_OBJECT_UNLOCK (self);
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("No audio probe with name %s found.", self->probe_name), (NULL));
      return FALSE;
    }
  }

  GST_OBJECT_UNLOCK (self);

  return TRUE;
  
initialize_failed:
  GST_OBJECT_UNLOCK (self);
#ifdef _WAIT
  GST_ELEMENT_ERROR (self, LIBRARY, INIT,
      ("Failed to initialize WebRTC Audio Processing library"),
      ("webrtc::AudioProcessing::Initialize() failed: %s",
          webrtc_error_to_string (err)));
#endif
  return FALSE;
}

static gboolean
gst_webrtc_audio_processor_setup (GstAudioFilter * filter, const GstAudioInfo * info)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (filter);
  GstAudioInfo probe_info = *info;

  GST_LOG_OBJECT (self, "setting format to %s with %i Hz and %i channels",
      info->finfo->description, info->rate, info->channels);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);

  self->info = *info;

  /* WebRTC library works with 10ms buffers, compute once this size */
  self->period_samples = info->rate / 100;
  self->period_size = self->period_samples * info->bpf;

#ifdef _WAIT
  if ((kMaxDataSizeSamples * 2) < self->period_size)
    goto period_too_big;
#endif

  if (self->probe) {
    GST_WEBRTC_AUDIO_PROBE_LOCK (self->probe);

    if (self->probe->info.rate != 0) {
      if (self->probe->info.rate != info->rate)
        goto probe_has_wrong_rate;
      probe_info = self->probe->info;
    }

    GST_WEBRTC_AUDIO_PROBE_UNLOCK (self->probe);
  }

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

  /* CONVERT
  if (self->voice_detection) {
    GST_DEBUG_OBJECT (self, "Enabling Voice Activity Detection, frame size "
      "%d milliseconds", self->voice_detection_frame_size_ms);

    self->stream_has_voice = FALSE;

    apm->voice_detection ()->Enable (true);
    apm->voice_detection ()->set_frame_size_ms (
        self->voice_detection_frame_size_ms);
  }
  */

  GST_OBJECT_UNLOCK (self);

  return TRUE;

#ifdef _WAIT
period_too_big:
#endif
  GST_OBJECT_UNLOCK (self);
#ifdef _WAIT
  GST_WARNING_OBJECT (self, "webrtcaudioprocessor format produce too big period "
      "(maximum is %" G_GSIZE_FORMAT " samples and we have %u samples), "
      "reduce the number of channels or the rate.",
      kMaxDataSizeSamples, self->period_size / 2);
#endif
  return FALSE;

probe_has_wrong_rate:
  GST_WEBRTC_AUDIO_PROBE_UNLOCK (self->probe);
  GST_OBJECT_UNLOCK (self);
  GST_ELEMENT_ERROR (self, STREAM, FORMAT,
      ("Audio probe has rate %i , while the processor is running at rate %i,"
          " use a caps filter to ensure those are the same.",
          probe_info.rate, info->rate), (NULL));
  return FALSE;
}

static gboolean
gst_webrtc_audio_processor_stop (GstBaseTransform * btrans)
{
  GstWebrtcAudioProcessor *self = GST_WEBRTC_AUDIO_PROCESSOR (btrans);

  GST_OBJECT_LOCK (self);

  gst_adapter_clear (self->adapter);

  if (self->probe) {
    gst_webrtc_release_audio_probe (self->probe);
    self->probe = NULL;
  }

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
    case PROP_PROBE:
      g_free (self->probe_name);
      self->probe_name = g_value_dup_string (value);
      break;
    case PROP_LOGGING_SEVERITY:
      self->logging_severity =
          (GstWebrtcAudioProcessingLoggingSeverity) g_value_get_enum (value);
      break;
    case PROP_PROCESSING_RATE:
      self->processing_rate = g_value_get_int (value);
      break;
    case PROP_HIGH_PASS_FILTER:
      self->high_pass_filter = g_value_get_boolean (value);
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
    case PROP_GAIN_CONTROL:
      self->gain_control = g_value_get_boolean (value);
      break;
    case PROP_TARGET_LEVEL_DBFS:
      self->target_level_dbfs = g_value_get_int (value);
      break;
    case PROP_COMPRESSION_GAIN_DB:
      self->compression_gain_db = g_value_get_int (value);
      break;
    case PROP_STARTUP_MIN_VOLUME:
      self->startup_min_volume = g_value_get_int (value);
      break;
    case PROP_LIMITER:
      self->limiter = g_value_get_boolean (value);
      break;
#ifdef _WAIT
    case PROP_GAIN_CONTROL_MODE:
      self->gain_control_mode =
          (GstWebrtcAudioProcessingGainControlMode) g_value_get_enum (value);
      break;
#endif
    case PROP_VOICE_DETECTION:
      self->voice_detection = g_value_get_boolean (value);
      break;
    case PROP_VOICE_DETECTION_FRAME_SIZE_MS:
      self->voice_detection_frame_size_ms = g_value_get_int (value);
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
    case PROP_PROBE:
      g_value_set_string (value, self->probe_name);
      break;
    case PROP_LOGGING_SEVERITY:
      g_value_set_enum (value, self->logging_severity);
      break;
    case PROP_PROCESSING_RATE:
      g_value_set_int (value, self->processing_rate);
      break;
    case PROP_HIGH_PASS_FILTER:
      g_value_set_boolean (value, self->high_pass_filter);
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
    case PROP_GAIN_CONTROL:
      g_value_set_boolean (value, self->gain_control);
      break;
    case PROP_TARGET_LEVEL_DBFS:
      g_value_set_int (value, self->target_level_dbfs);
      break;
    case PROP_COMPRESSION_GAIN_DB:
      g_value_set_int (value, self->compression_gain_db);
      break;
    case PROP_STARTUP_MIN_VOLUME:
      g_value_set_int (value, self->startup_min_volume);
      break;
    case PROP_LIMITER:
      g_value_set_boolean (value, self->limiter);
      break;
#ifdef _WAIT
    case PROP_GAIN_CONTROL_MODE:
      g_value_set_enum (value, self->gain_control_mode);
      break;
#endif
    case PROP_VOICE_DETECTION:
      g_value_set_boolean (value, self->voice_detection);
      break;
    case PROP_VOICE_DETECTION_FRAME_SIZE_MS:
      g_value_set_int (value, self->voice_detection_frame_size_ms);
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
  g_free (self->probe_name);

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
      PROP_PROBE,
      g_param_spec_string ("probe", "Audio Probe",
          "The name of the webrtcaudioprobe element that record the audio being "
          "played through loud speakers. Must be set before PAUSED state.",
          "webrtcaudioprobe0",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

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
      PROP_HIGH_PASS_FILTER,
      g_param_spec_boolean ("high-pass-filter", "High Pass Filter",
          "Enable or disable high pass filtering", FALSE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

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

  g_object_class_install_property (gobject_class,
      PROP_GAIN_CONTROL,
      g_param_spec_boolean ("gain-control", "Gain Control",
          "Enable or disable automatic digital gain control",
          FALSE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_TARGET_LEVEL_DBFS,
      g_param_spec_int ("target-level-dbfs", "Target Level dBFS",
          "Sets the target peak |level| (or envelope) of the gain control in "
          "dBFS (decibels from digital full-scale).",
          0, 31, DEFAULT_TARGET_LEVEL_DBFS, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_COMPRESSION_GAIN_DB,
      g_param_spec_int ("compression-gain-db", "Compression Gain dB",
          "Sets the maximum |gain| the digital compression stage may apply, "
					"in dB.",
          0, 90, DEFAULT_COMPRESSION_GAIN_DB, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_LIMITER,
      g_param_spec_boolean ("limiter", "Limiter",
          "When enabled, the compression stage will hard limit the signal to "
          "the target level. Otherwise, the signal will be compressed but not "
          "limited above the target level.",
          DEFAULT_LIMITER, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

#ifdef _WAIT
  g_object_class_install_property (gobject_class,
      PROP_GAIN_CONTROL_MODE,
      g_param_spec_enum ("gain-control-mode", "Gain Control Mode",
          "Controls the mode of the compression stage",
          GST_TYPE_WEBRTC_GAIN_CONTROL_MODE,
          DEFAULT_GAIN_CONTROL_MODE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));
#endif

  g_object_class_install_property (gobject_class,
      PROP_VOICE_DETECTION,
      g_param_spec_boolean ("voice-detection", "Voice Detection",
          "Enable or disable the voice activity detector",
          DEFAULT_VOICE_DETECTION, (GParamFlags) (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class,
      PROP_VOICE_DETECTION_FRAME_SIZE_MS,
      g_param_spec_int ("voice-detection-frame-size-ms",
          "Voice Detection Frame Size Milliseconds",
          "Sets the |size| of the frames in ms on which the VAD will operate. "
          "Larger frames will improve detection accuracy, but reduce the "
          "frequency of updates",
          10, 30, DEFAULT_VOICE_DETECTION_FRAME_SIZE_MS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              G_PARAM_CONSTRUCT)));

#ifdef _WAIT
  gst_type_mark_as_plugin_api (GST_TYPE_WEBRTC_GAIN_CONTROL_MODE, (GstPluginAPIFlags) 0);
  gst_type_mark_as_plugin_api (GST_TYPE_WEBRTC_NOISE_SUPPRESSION_LEVEL, (GstPluginAPIFlags) 0);
#endif
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
