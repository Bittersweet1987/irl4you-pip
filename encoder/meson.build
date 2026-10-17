project('irl4you-encoder', 'c', version : '0.2.0', default_options : ['c_std=gnu17', 'warning_level=2', 'buildtype=release'])
gst = dependency('gstreamer-1.0', version : '>=1.24')
gstapp = dependency('gstreamer-app-1.0')
json = dependency('json-glib-1.0')
srt = dependency('srt', required : false)
if not srt.found()
  srt = dependency('srt-gnutls')
endif
m = meson.get_compiler('c').find_library('m')
ratectl = static_library('ratectl', 'src/ratectl.c', dependencies : m)
executable('irl4you-encoder', 'src/irl4you-encoder.c', link_with : ratectl,
           dependencies : [gst, gstapp, json, srt, dependency('glib-2.0'), dependency('gio-unix-2.0')], install : true)
test('ratectl', executable('test-ratectl', 'tests/test_ratectl.c', link_with : ratectl, dependencies : m))
