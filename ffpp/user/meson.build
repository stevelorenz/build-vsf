project('ffpp', 'c', 'cpp',
  version: run_command(find_program('cat', 'more'),files('../VERSION')).stdout().strip(),
  license: 'MIT',
  meson_version: '>= 0.53.0',
  default_options: [
    'c_std=gnu18',
    # C++20 is used for C-style designated initializers.
    'cpp_std=c++2a',
    'buildtype=release',
    'warning_level=2',
    'default_library=both',
    'werror=true',
    'b_lundef=true',
  ],
)

host_system = host_machine.system()

cc = meson.get_compiler('c')
cppc = meson.get_compiler('cpp')

c_flags = [
    '-Wlogical-op',
    '-Werror=missing-include-dirs',
    '-Werror=pointer-arith',
    '-Werror=init-self',
    '-Wfloat-equal',
    '-Wsuggest-attribute=noreturn',
    # '-Werror=missing-prototypes',
    '-Werror=implicit-function-declaration',
    '-Werror=return-type',
    '-Werror=incompatible-pointer-types',
    '-Werror=format=2',
    # '-Wstrict-prototypes',
    # '-Wredundant-decls',
    # '-Wimplicit-fallthrough=5',
    '-Wendif-labels',
    '-Wstrict-aliasing=3',
    '-Wwrite-strings',
    '-Werror=overflow',
    '-Werror=shift-count-overflow',
    '-Werror=shift-overflow=2',
    '-Wdate-time',
    '-Wnested-externs',
    '-Wunused',
    '-Wduplicated-branches',
    '-Wmisleading-indentation',

    '-Wno-sign-compare',
    '-Wno-unused-parameter',

    ## '-fvisibility=hidden',
    '-ffast-math',
    '-fdiagnostics-show-option',
    '-fstack-protector',
    '-fstack-protector-strong',
    '--param=ssp-buffer-size=4',

    # ABI stability of DPDK is not considered here.
    # Because the test container always uses a specific DPDK version.
    '-Wno-deprecated-declarations',
    # They will be used in 6G and beyond :)
    '-Wno-unused-value',
    '-Wno-unused-variable',
    '-Wno-unused-but-set-variable',
]

cpp_flags = [
    '-Wlogical-op',
    '-Werror=missing-include-dirs',
    '-Werror=pointer-arith',
    '-Werror=init-self',
    '-Wfloat-equal',
    '-Wsuggest-attribute=noreturn',
    # '-Werror=missing-declarations',
    '-Werror=return-type',
    '-Werror=format=2',
    '-Wredundant-decls',
    '-Wimplicit-fallthrough=5',
    '-Wendif-labels',
    '-Wstrict-aliasing=3',
    '-Wwrite-strings',
    '-Werror=overflow',
    '-Werror=shift-count-overflow',
    '-Werror=shift-overflow=2',
    '-Wdate-time',
    '-Wunused',
    '-Wduplicated-branches',
    '-Wmisleading-indentation',

    '-Wno-sign-compare',
    '-Wno-unused-parameter',

    ## '-fvisibility=hidden',
    '-ffast-math',
    '-fdiagnostics-show-option',
    '-fstack-protector',
    '-fstack-protector-strong',
    '--param=ssp-buffer-size=4',

    # When including DPDK headers, the following warnings will be disabled to
    # allow the code to compile...
    '-Wno-implicit-fallthrough',
    # ABI stability of DPDK is not considered here.
    # Because the test container always uses a specific DPDK version.
    '-Wno-deprecated-declarations',

    # They will be used in 6G and beyond :)
    '-Wno-unused-value',
    '-Wno-unused-variable',
]

add_project_arguments(
    c_flags,
    language: 'c',
)
add_project_arguments(
    cpp_flags,
    language: 'cpp',
)

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : ['c', 'cpp'])
endif

inc = include_directories('include')

math_dep = cc.find_library('m', required: true)

# Libraries with support for pkg-config
bpf_dep = dependency('libbpf', required: true)
dpdk_dep = dependency('libdpdk', required: true)
json_dep = dependency('jansson', required: true)
python_embed_dep = dependency('python3-embed', required: true)
zmq_dep = dependency('libzmq', required: true)
libtins_dep = dependency('libtins', required: true)

# Header-only libraries.
pybind11_inc = include_directories('/usr/include/pybind11')
pybind11_dep = declare_dependency(include_directories : pybind11_inc)

ffpp_dep_list = [
  bpf_dep,
  dpdk_dep,
  json_dep,
  math_dep,
  python_embed_dep,
  zmq_dep,
  pybind11_dep,
  libtins_dep,
]

ffpp_deps = declare_dependency(
  dependencies: ffpp_dep_list,
)

subdir('include')
subdir('src')
subdir('third_party')

if get_option('examples')
  subdir('examples')
endif

if get_option('tests')
  subdir('tests')
endif

if get_option('related_works')
  subdir('related_works')
endif

pkg_mod = import('pkgconfig')
pkg_mod.generate(
  name: meson.project_name(),
  filebase: 'lib' + meson.project_name().to_lower(),
  libraries : [ffpplib_shared, ffpplib_static],
  version: meson.project_version(),
  description : 'A Library for Fast Functional Packet Processing.'
)

run_target('cppcheck', command: [
  'cppcheck','--enable=warning,style,performance,information',
  '--project=' + join_paths(meson.source_root(), 'build', 'compile_commands.json')
]) 

message('\n'.join([
  '',
  '        Host system:                  ' + host_system,
  '        Source code location:         ' + meson.source_root(),
  '        C compiler:                   ' + cc.get_id(),
  '        CPP compiler:                 ' + cppc.get_id(),
  '        Debugging support:            ' + get_option('buildtype'),
  '',
]))
