mock_exe = executable('mock_textractor', 'mock.cpp',
  include_directories : '../TCPSender',
  win_subsystem : 'windows')

run_target('run_mock', command : [mock_exe, tcpsender.full_path()],
  depends : tcpsender)
