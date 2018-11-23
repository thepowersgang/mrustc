
# Build outline for common:
#
# Sourcefiles:  All inside common directory
# Dependencies: None
# Notes:        Needs compiler capable of C++14


###############################################################################
#                              Compile libCommon                              #
###############################################################################
add_library(common STATIC
            debug.h
            debug.cpp
            helpers.h
            path.h
            path.cpp
            target_detect.h
            toml.h
            toml.cpp)

target_compile_features(common PUBLIC cxx_std_14)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
