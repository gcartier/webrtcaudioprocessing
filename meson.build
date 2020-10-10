project('webrtcaudioprocessing', ['c', 'cpp'], version : '1.0', license : 'LGPL')

cc = meson.get_compiler('c')

gst_version = meson.project_version()

api_version = '1.0'

gst_dep = dependency('gstreamer-1.0',
    fallback : ['gstreamer', 'gst_dep'])

subdir('plugin')
