# CMake build for libtiff
#
# Copyright © 2015 Open Microscopy Environment / University of Dundee
# Written by Roger Leigh <rleigh@codelibre.net>
#
# Permission to use, copy, modify, distribute, and sell this software and
# its documentation for any purpose is hereby granted without fee, provided
# that (i) the above copyright notices and this permission notice appear in
# all copies of the software and related documentation, and (ii) the names of
# Sam Leffler and Silicon Graphics may not be used in any advertising or
# publicity relating to the software without the specific, prior written
# permission of Sam Leffler and Silicon Graphics.
#
# THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
# EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
# WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
#
# IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
# ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
# OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
# WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
# LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
# OF THIS SOFTWARE.

# Generate headers
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tif_config.h.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/tif_config.h
               @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tiffconf.h.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/tiffconf.h
               @ONLY)

set(tiff_public_HEADERS
        tiff.h
        tiffio.h
        tiffvers.h
        ${CMAKE_CURRENT_BINARY_DIR}/tiffconf.h)

set(tiff_private_HEADERS
        t4.h
        tif_dir.h
        tif_predict.h
        tiffiop.h
        uvcode.h
        ${CMAKE_CURRENT_BINARY_DIR}/tif_config.h)


add_library(tiff libtiff.def)

target_sources(tiff PRIVATE
        ${tiff_public_HEADERS}
        ${tiff_private_HEADERS}
        tif_aux.c
        tif_close.c
        tif_codec.c
        tif_color.c
        tif_compress.c
        tif_dir.c
        tif_dirinfo.c
        tif_dirread.c
        tif_dirwrite.c
        tif_dumpmode.c
        tif_error.c
        tif_extension.c
        tif_fax3.c
        tif_fax3sm.c
        tif_flush.c
        tif_getimage.c
        tif_jbig.c
        tif_jpeg.c
        tif_jpeg_12.c
        tif_lerc.c
        tif_luv.c
        tif_lzma.c
        tif_lzw.c
        tif_next.c
        tif_ojpeg.c
        tif_open.c
        tif_packbits.c
        tif_pixarlog.c
        tif_predict.c
        tif_print.c
        tif_read.c
        tif_strip.c
        tif_swab.c
        tif_thunder.c
        tif_tile.c
        tif_version.c
        tif_warning.c
        tif_webp.c
        tif_write.c
        tif_zip.c
        tif_zstd.c)

if(USE_WIN32_FILEIO)
  target_sources(tiff PRIVATE tif_win32.c)
else()
  target_sources(tiff PRIVATE tif_unix.c)
endif()

target_include_directories(tiff
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    PRIVATE
        ${TIFF_INCLUDES}
)

set(tiff_libs_private_list "") # cmake list
set(tiff_requires_private "") # cmake string

if(ZIP_SUPPORT)
  target_link_libraries(tiff PRIVATE ZLIB::ZLIB)
  string(APPEND tiff_requires_private " zlib")
endif()
if(ZIP_SUPPORT AND LIBDEFLATE_SUPPORT)
  target_link_libraries(tiff PRIVATE Deflate::Deflate)
  list(APPEND tiff_libs_private_list "${Deflate_LIBRARY}")
endif()
if(JPEG_SUPPORT)
  target_link_libraries(tiff PRIVATE JPEG::JPEG)
  string(APPEND tiff_requires_private " libjpeg")
  if(JPEG_DUAL_MODE_8_12)
    target_include_directories(tiff PRIVATE ${JPEG12_INCLUDE_DIR})
    target_link_libraries(tiff PRIVATE ${JPEG12_LIBRARIES})
  endif()
endif()
if(JBIG_SUPPORT)
  target_link_libraries(tiff PRIVATE JBIG::JBIG)
  list(APPEND tiff_libs_private_list "${JBIG_LIBRARY}")
endif()
if(LERC_SUPPORT)
  target_link_libraries(tiff PRIVATE LERC::LERC)
  list(APPEND tiff_libs_private_list "${LERC_LIBRARY}")
  if(NOT BUILD_SHARED_LIBS)
    set_target_properties(tiff PROPERTIES COMPILE_DEFINITIONS LERC_STATIC)
  endif()
endif()
if(LZMA_SUPPORT)
  target_link_libraries(tiff PRIVATE LibLZMA::LibLZMA)
  string(APPEND tiff_requires_private " liblzma")
endif()
if(ZSTD_SUPPORT)
  target_link_libraries(tiff PRIVATE ZSTD::ZSTD)
  string(APPEND tiff_requires_private " libzstd")
endif()
if(WEBP_SUPPORT)
  target_link_libraries(tiff PRIVATE WebP::WebP)
  string(APPEND tiff_requires_private " libwebp")
endif()
target_link_libraries(tiff PRIVATE CMath::CMath)
if(CMath_LIBRARY)
  list(APPEND tiff_libs_private_list "${CMath_LIBRARY}")
endif()

set(tiff_libs_private_list "${tiff_libs_private_list}" PARENT_SCOPE)
set(tiff_requires_private "${tiff_requires_private}" PARENT_SCOPE)

set_target_properties(tiff PROPERTIES SOVERSION ${SO_COMPATVERSION})
if(NOT CYGWIN)
    # This property causes shared libraries on Linux to have the full version
    # encoded into their final filename.  We disable this on Cygwin because
    # it causes cygz-${TIFF_FULL_VERSION}.dll to be created when cygz.dll
    # seems to be the default.
    set_target_properties(tiff PROPERTIES VERSION ${SO_VERSION})
endif()
if(HAVE_LD_VERSION_SCRIPT)
  set_target_properties(tiff PROPERTIES LINK_FLAGS
                        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libtiff.map")
endif()

#[===[ Inviwo modification: disable unused export and install
export(TARGETS tiff NAMESPACE TIFF:: FILE "${CMAKE_CURRENT_BINARY_DIR}/libtiffTargets.cmake")
install(TARGETS tiff EXPORT TiffTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES ${tiff_public_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(TIFF_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/tiff)
install(EXPORT TiffTargets
        FILE TiffTargets.cmake
        NAMESPACE TIFF::
        DESTINATION ${TIFF_CONFIGDIR}
)
]===]

if(CXX_SUPPORT)
  set(tiffxx_HEADERS
          tiffio.hxx)

  add_library(tiffxx ../placeholder.h)
  target_sources(tiffxx PRIVATE
          ${tiffxx_HEADERS}
          tif_stream.cxx)
  target_link_libraries(tiffxx tiff)
  set_target_properties(tiffxx PROPERTIES SOVERSION ${SO_COMPATVERSION})
  if(NOT CYGWIN)
    # This property causes shared libraries on Linux to have the full version
    # encoded into their final filename.  We disable this on Cygwin because
    # it causes cygz-${TIFF_FULL_VERSION}.dll to be created when cygz.dll
    # seems to be the default.
    set_target_properties(tiffxx PROPERTIES VERSION ${SO_VERSION})
  endif()
  if(HAVE_LD_VERSION_SCRIPT)
    set_target_properties(tiffxx PROPERTIES LINK_FLAGS
                          "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libtiffxx.map")
  endif()

#[===[ Inviwo modification: disable install in case tiffxx is enabled
  install(TARGETS tiffxx
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  install(FILES ${tiffxx_HEADERS}
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
]===]
endif()

#[===[ Inviwo modification: mkg3states and faxtable are not part of libtiff library
add_executable(mkg3states ../placeholder.h)
target_sources(mkg3states PRIVATE mkg3states.c tif_fax3.h)
target_link_libraries(mkg3states tiff port)

add_custom_target(faxtable
        DEPENDS mkg3states
        COMMAND ${CMAKE_COMMAND} -E rm "tif_fax3sm.c"
        COMMAND mkg3states -b -c const "tif_fax3sm.c"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
]===]