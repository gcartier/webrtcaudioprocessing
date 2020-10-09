plugin_c_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'webrtcdsp3 plugin')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'WebRTCDSP3 plugin')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://github.com/gcartier/webrtcdsp3')
configure_file(output : 'config.h', configuration : cdata)

gstaudio_dep = dependency('gstreamer-audio-1.0',
    fallback: ['gst-plugins-base', 'audio_dep'])

cc = meson.get_compiler('c')
# if (host_machine.system() == 'darwin')
    rnnoise_dep = cc.find_library('rnnoise.0',
                    dirs : [meson.current_source_dir() + '/../rnnoise/lib'])

webrtcdsp3_inc = include_directories('../rnnoise/include')

webrtcdsp3_sources = [
  'src/gstwebrtcdsp.cpp',
  'src/gstwebrtcechoprobe.cpp'
]

library('gstwebrtcdsp3',
    webrtcdsp3_sources,
    c_args: plugin_c_args,
    dependencies : [gst_dep, gstaudio_dep, webrtcdsp3_dep],
    include_directories : webrtcdsp3_inc
)
