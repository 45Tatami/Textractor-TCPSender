project('Textractor-TCPSender', 'cpp',
  default_options : ['cpp_std=c++17'])

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

library('tcpsender', src,
  dependencies : deps)
