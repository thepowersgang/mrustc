
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


#! build_rust_std_lib : Builds a rust standard library
#
# \param:TARGET Set the name of the target which will trigger the build.
# \param:CRATE_TO_BUILD Path to the crate to build
# \group:DEPENDS List of targets the download should depend upon
#
function(build_rust_std_lib)
  set(options "")
  set(oneValueArgs
    TARGET
    CRATE_TO_BUILD)

  set(multiValueArgs
    DEPENDS)

  cmake_parse_arguments(
    BRL # Build Rustc Lib
    "${options}"
    "${oneValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  message(STATUS "Building minicargo-target ${BRL_TARGET} at ${BRL_CRATE_TO_BUILD}")
  message(STATUS "Executable output path: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

  if(UNIX AND NOT APPLE)
    # for Linux, BSD, Solaris, Minix. FIXME: More cases to support these platforms
    set(OVERRIDE_SUFFIX "-linux")
  elseif(APPLE)
    set(OVERRIDE_SUFFIX "-macos")
  elseif(MSVC)
    set(OVERRIDE_SUFFIX "") # Nothing for Win32?
  else()
    set(OVERRIDE_SUFFIX "")
  endif()

  set(OVERRIDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/script-overrides/${MRUSTC_RUSTC_CHANNEL}-${MRUSTC_RUSTC_VERSION}${OVERRIDE_SUFFIX}")

  # Path to minicargo executable
  set(MINICARGO ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/minicargo)

  add_custom_target(${BRL_TARGET}
    COMMAND ${MINICARGO} ${BRL_CRATE_TO_BUILD}
              --script-overrides ${OVERRIDE_DIR}
              --output-dir output
    DEPENDS minicargo mrustc ${BRL_DEPENDS}
    )
endfunction()
