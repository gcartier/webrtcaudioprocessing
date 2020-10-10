plugin_cpp_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'webrtcaudioprocessing plugin')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'WebRTC Audio Processing plugin')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://github.com/gcartier/webrtcaudioprocessing')
cdata.set_quoted('VERSION', gst_version)
configure_file(output : 'config.h', configuration : cdata)

gstaudio_dep = dependency('gstreamer-audio-1.0')
gstbadaudio_dep = dependency('gstreamer-bad-audio-1.0')

#webrtcaudioprocessing_inc = include_directories('../rnnoise/include')
webrtcaudioprocessing_inc = []

webrtcaudioprocessing_sources = [
  'src/gstwebrtcaudioprocessor.cpp',
  'src/gstwebrtcaudioprobe.cpp'
]

webrtc_dep = dependency('webrtc')

gstwebrtcaudioprocessing = library('gstwebrtcaudioprocessing',
  webrtcaudioprocessing_sources,
  cpp_args: plugin_cpp_args,
  dependencies : [gst_dep, gstaudio_dep, gstbadaudio_dep, webrtc_dep],
  include_directories : [webrtcaudioprocessing_inc],
  override_options : ['cpp_std=c++11'],
)
