libtinyalsa_util_sources = [
'tinyalsa-source.c',
'tinyalsa-sink.c',
]

libtinyalsa_util_headers = [
'tinyalsa-source.h',
'tinyalsa-sink.h',
]


libtinyalsa_util = shared_library('tinyalsa-util',
  libtinyalsa_util_sources,
  libtinyalsa_util_headers,
  c_args : [pa_c_args, server_c_args],
  link_args : [nodelete_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, alsa_dep, dbus_dep, libatomic_ops_dep, libm_dep, udev_dep, libintl_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)

