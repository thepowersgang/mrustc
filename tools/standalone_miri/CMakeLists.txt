
# Build outline for Standalone MIRI:
#
# Sourcefiles:  All inside standalone_miri directory
# Dependencies: libCommon, threading (eg. pthread), mrustc includes
# Notes:        Needs compiler capable of C++14. Disables Warnings.


###############################################################################
#                              Compile Standalone MIRI                        #
###############################################################################
add_executable(standalone_miri
              hir/type.hpp
              CMakeLists.txt
              debug.cpp
              debug.hpp
              hir_sim.cpp
              hir_sim.hpp
              lex.cpp
              lex.hpp
              main.cpp
              mir.cpp
              miri.cpp
              miri.hpp
              module_tree.cpp
              module_tree.hpp
              value.cpp
              value.hpp)

target_compile_features(standalone_miri PUBLIC cxx_std_14)

find_package(Threads REQUIRED)

target_link_libraries(standalone_miri
  PUBLIC
   Threads::Threads
   common)

target_include_directories(standalone_miri
  PUBLIC
   .
   ../../src/include)
