common_include_directories = include_directories('.')

common = declare_dependency(
    link_with: static_library(
        'common',
        'debug.cpp',
        'path.cpp',
        'toml.cpp',
        include_directories: common_include_directories,
    ),
    include_directories: common_include_directories,
)
