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

python = import('python').find_installation('python3')

# TODO: add downloading rustc-source

subdir('tools/common')
subdir('tools/minicargo')
subdir('tools/standalone_miri')
subdir('tools/testrunner')

subdir('src')
