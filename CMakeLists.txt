project(mrustc)
cmake_minimum_required(VERSION 3.4)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ExternalProject)
include(GetGitRevisionDescription)

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


#! download_rustc_source : Downloads the rustc-source from the location specified inside
#                          MRUSTC_RUSTC_SOURCE_URL
#
# Defines ${TARGET}_DIRECTORY.
#
# \param:TARGET Set the name of the target which will trigger the download.
# \group:DEPENDS List of targets the download should depend upon
#
function(download_rustc_source)
  set(options "")
  set(oneValueArgs TARGET )
  set(multiValueArgs DEPENDS)
  cmake_parse_arguments(DL_RUSTC
                        "${options}"
                        "${oneValueArgs}"
                        "${multiValueArgs}"
                        ${ARGN})

  if (NOT MRUSTC_RUSTC_SOURCE_URL)
    message(FATAL_ERROR "MRUSTC_RUSTC_SOURCE_URL is not set! Please specify where the rustc-sources are!")
  endif()

  if (NOT DL_RUSTC_TARGET)
    message(FATAL_ERROR "TARGET property of download_rustc_source must be set!")
  endif()

  set(${DL_RUSTC_TARGET}_DIRECTORY ${CMAKE_BINARY_DIR}/rustc-source PARENT_SCOPE)

  ExternalProject_Add(${DL_RUSTC_TARGET}
    PREFIX            ${${DL_RUSTC_TARGET}_DIRECTORY}
    URL               ${MRUSTC_RUSTC_SOURCE_URL}
    URL_HASH          ${MRUSTC_RUSTC_SOURCE_HASH}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   ""
    DEPENDS ${DL_RUSTC_DEPENDS})
endfunction()

download_rustc_source(TARGET rustc_source)
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

# Set up some meta-information
# $V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep
# -D VERSION_GIT_FULLHASH=\"$(shell git show --pretty=%H -s)\"
# -D VERSION_GIT_BRANCH="\"$(shell git symbolic-ref -q --short HEAD || git describe --tags --exact-match)\""
# -D VERSION_GIT_SHORTHASH=\"$(shell git show -s --pretty=%h)\"
# -D VERSION_BUILDTIME="\"$(shell date -uR)\""
# -D VERSION_GIT_ISDIRTY=$(shell git diff-index --quiet HEAD; echo $$?)

#! gather_git_info : Populates variables with the current commit hash, branch, ...
#
# Defines the following variables:
#
#  GIT_IS_DIRTY - Truthful if there are uncommited changes
#  GIT_HEAD_REF - Head ref., eg. "refs/head/master"
#  GIT_HEAD_HASH - HEADs commit hash (long)
#  GIT_HEAD_HASH_SHORT - HEADs commit hash (short - 7 chars)
#  GIT_BRANCH - Current branch
#
function(gather_git_info)
  git_local_changes(GIT_IS_DIRTY)
  get_git_head_revision(GIT_HEAD_REF GIT_HEAD_HASH "--pretty=%h") # Get short hash
  string(SUBSTRING ${GIT_HEAD_HASH} 0 7 GIT_HEAD_HASH_SHORT)
  string(REPLACE "/" ";" GIT_HEAD_REF_AS_LIST ${GIT_HEAD_REF}) # Split eg. refs/head/master
  list(REVERSE GIT_HEAD_REF_AS_LIST)
  list(GET GIT_HEAD_REF_AS_LIST 0 GIT_BRANCH)

  set(GIT_IS_DIRTY ${GIT_IS_DIRTY} PARENT_SCOPE)
  set(GIT_HEAD_REF ${GIT_HEAD_REF} PARENT_SCOPE)
  set(GIT_HEAD_HASH ${GIT_HEAD_HASH} PARENT_SCOPE)
  set(GIT_HEAD_HASH_SHORT ${GIT_HEAD_HASH_SHORT} PARENT_SCOPE)
  set(GIT_BRANCH ${GIT_BRANCH} PARENT_SCOPE)
endfunction()

gather_git_info()
string(TIMESTAMP BUILD_TIMESTAMP )
message(STATUS "Git status: ${GIT_IS_DIRTY}")
message(STATUS "Git ref...: ${GIT_HEAD_REF}")
message(STATUS "Git hash..: ${GIT_HEAD_HASH}")
message(STATUS "Git shash.: ${GIT_HEAD_HASH_SHORT}")
message(STATUS "Git branch: ${GIT_BRANCH}")
message(STATUS "Buildtime.: ${BUILD_TIMESTAMP}")


add_subdirectory(src)
