# Build outline for Standalone MIRI:
#
# Sourcefiles:  All inside testrunner directory
# Dependencies: libCommon
# Notes:        Needs compiler capable of C++14. Disables Warnings.


###############################################################################
#                              Compile TestRunner                             #
###############################################################################
add_executable(test_runner "main.cpp")

target_compile_features(test_runner PUBLIC cxx_std_14)

target_link_libraries(test_runner
  PUBLIC
   common)
