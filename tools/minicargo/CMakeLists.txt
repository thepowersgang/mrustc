
# Build outline for minicargo:
#
# Sourcefiles:  All inside minicargo directory
# Dependencies: libCommon, threading (eg. pthread)
# Notes:        Needs compiler capable of C++14


###############################################################################
#                              Compile Minicargo                              #
###############################################################################
add_executable(minicargo
               build.h
               build.cpp
               main.cpp
               manifest.h
               manifest.cpp
               repository.h
               repository.cpp
               stringlist.h)

target_compile_features(minicargo PUBLIC cxx_std_14)

find_package(Threads REQUIRED)
target_link_libraries(minicargo Threads::Threads)
target_link_libraries(minicargo common)
