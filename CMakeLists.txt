project(mrustc)
cmake_minimum_required(VERSION 3.4)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ExternalProject)
include(GatherGitInfo)

# - [x] Find exact rustc version to be downloaded
# - [x] Download rustc-source
# - [ ] Unpack to some location
# - [ ] Original makefile patches rustc-source? Find out why and whether that's needed!


###############################################################################
#                               Cache Variables                               #
###############################################################################

set(MRUSTC_RUSTC_VERSION
    "1.19.0"
    CACHE STRING
    "Version of rustc to expect. Must match the downloaded sourcecode!")

set(MRUSTC_RUSTC_CHANNEL
    "stable"
    CACHE STRING
    "Channel of rustc to expect. Can be 'stable' or 'nightly'.
     Must match the downloaded sourcecode!")

set(MRUSTC_RUSTC_SOURCE_URL
    "https://static.rust-lang.org/dist/rustc-1.19.0-src.tar.gz"
    CACHE STRING
    "URL or local path of where rustc's source should be downloaded from.
     This can also be a local file!")

set(MRUSTC_RUSTC_SOURCE_HASH
    "SHA256=15231f5053fb72ad82be91f5abfd6aa60cb7898c5089e4f1ac5910a731090c51"
    CACHE STRING
    "SHA256 of the file downloaded from MRUSTC_RUSTC_SOURCE_URL.
    You can leave this empty to skip verifying whether the downloaded archive is correct.")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

#############################################################################
#                           Download rustc-source                           #
#############################################################################

# Specify where to put or to find the rustc sources
set(RUSTC_SOURCE_DIR ${CMAKE_BINARY_DIR}/rustc-source)

ExternalProject_Add(get_rustc_source
  SOURCE_DIR        ${CMAKE_BINARY_DIR}/rustc-source
  URL               ${MRUSTC_RUSTC_SOURCE_URL}
  URL_HASH          ${MRUSTC_RUSTC_SOURCE_HASH}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  )

###############################################################################
#                        Compile Tools (Minicargo, ...)                       #
###############################################################################
add_subdirectory(tools/common)
add_subdirectory(tools/minicargo)
add_subdirectory(tools/standalone_miri)
add_subdirectory(tools/testrunner)


###############################################################################
#                                Compile mrustc                               #
###############################################################################

gather_git_info()
string(TIMESTAMP BUILD_TIMESTAMP UTC)
message(STATUS "Git status: ${GIT_IS_DIRTY}")
message(STATUS "Git ref...: ${GIT_HEAD_REF}")
message(STATUS "Git hash..: ${GIT_HEAD_HASH}")
message(STATUS "Git shash.: ${GIT_HEAD_HASH_SHORT}")
message(STATUS "Git branch: ${GIT_BRANCH}")
message(STATUS "Buildtime.: ${BUILD_TIMESTAMP}")


add_subdirectory(src)
