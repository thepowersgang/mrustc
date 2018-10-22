project(
    'mrustc',
    'cpp',
    version: '0.8.0',
    license: 'MIT',
    default_options: [
        'cpp_std=c++14',
        'werror=false',
        'warning_level=2',
    ],
)

major_version = 0
minor_version = 8
patch_version = 0

compiler = meson.get_compiler('cpp').get_id()
if compiler == 'msvc'
  add_project_arguments(
        '-wd4267',
        '-wd4244',
        '-wd4099',
        '-wd4996',
        '-wd4146',
        language: 'cpp',
    )
endif

pymod = import('python')
python = pymod.find_installation('python3')
version_target = custom_target(
    'version',
    input: 'version.py',
    output: 'version.cpp',
    build_always_stale: true,
    install: false,
    command: [
        python, '@INPUT@',
        '--output-file', '@OUTPUT@',
        '--major-version', major_version.to_string(),
        '--minor-version', minor_version.to_string(),
        '--patch-version', patch_version.to_string(),
    ],
)
version_file = version_target[0]

# TODO: add downloading rustc-source

subdir('tools/common')
subdir('tools/minicargo')
subdir('tools/standalone_miri')
subdir('tools/testrunner')

subdir('src')
