project(mrustc)
cmake_minimum_required(VERSION 3.4)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ExternalProject)
include(GatherGitInfo)
include(DownloadRustcSource)

# - [x] Find exact rustc version to be downloaded
# - [x] Download rustc-source
# - [ ] Unpack to some location
# - [ ] Original makefile patches rustc-source? Find out why and whether that's needed!


###############################################################################
#                               Cache Variables                               #
###############################################################################

set(MRUSTC_RUSTC_SOURCE_URL
    "https://github.com/rust-lang/rust/archive/1.19.0.tar.gz"
    CACHE STRING
    "URL or local path of where rustc's source should be downloaded from. ")

set(MRUSTC_RUSTC_SOURCE_HASH
    "SHA256=7e1ecb476118b79b5abed02bc7a724bb65413057e26f1d2b8538c572f7463be0"
    CACHE STRING
    "SHA256 of the file downloaded from MRUSTC_RUSTC_SOURCE_URL.
    You can leave this empty to skip verifying whether the downloaded archive is correct.")


#############################################################################
#                           Download rustc-source                           #
#############################################################################

download_rustc_source(
  TARGET get_rustc_source
  URL ${MRUSTC_RUSTC_SOURCE_URL}
  HASH ${MRUSTC_RUSTC_SOURCE_HASH})
# add_dependencies(hello_world rustc_source)


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
