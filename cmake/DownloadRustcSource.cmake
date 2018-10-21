
#! download_rustc_source : Downloads the rustc-source from the location specified inside
#                          MRUSTC_RUSTC_SOURCE_URL
#
# Defines ${TARGET}_DIRECTORY.
#
# \param:TARGET Set the name of the target which will trigger the download.
# \param:URL URL or Path to download the sources from
# \param:HASH (optional) Hash of the downloaded archive (ie. SHA256=...)
# \group:DEPENDS List of targets the download should depend upon
#
function(download_rustc_source)
  set(options "")
  set(oneValueArgs TARGET URL HASH)
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
    URL               ${DL_RUSTC_URL}
    URL_HASH          ${DL_RUSTC_HASH}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   ""
    DEPENDS ${DL_RUSTC_DEPENDS})
endfunction()
