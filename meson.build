project('Textractor-TCPSender', 'cpp',
  meson_version : '>=0.56.0',
  default_options : ['cpp_std=c++17', 'warning_level=3'])

add_project_arguments('-DUNICODE', language : 'cpp')

compiler = meson.get_compiler('cpp')

src = files(
  'TCPSender/TCPSender.cpp',
  'TCPSender/ExtensionImpl.cpp'
)

windows = import('windows')
src += windows.compile_resources('TCPSender/resource.rc')

deps = []
deps +=  compiler.find_library('ws2_32')

tcpsender = library('TCPSender', src,
  name_prefix : '', name_suffix : 'xdll',
  dependencies : deps)

if get_option('build_mock').enabled()
  subdir('mock')
endif
