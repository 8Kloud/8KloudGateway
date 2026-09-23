cmake_minimum_required(VERSION 3.25)
project(kloudgateway VERSION 0.5.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(gateway_core
  src/core/config.cpp
  src/core/log.cpp)
target_include_directories(gateway_core PUBLIC src)
target_link_libraries(gateway_core PUBLIC nlohmann_json::nlohmann_json)

option(KLOUDGATEWAY_REQUIRE_APP
       "Fail configure unless the complete SRT-to-OMT daemon can be built" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
pkg_check_modules(SRT IMPORTED_TARGET srt)

set(OMT_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/omt CACHE PATH
    "OMT SDK root containing include/libomt.h and lib/libomt.so")
if(EXISTS ${OMT_SDK_DIR}/include/libomt.h AND
   EXISTS ${OMT_SDK_DIR}/lib/libomt.so AND
   EXISTS ${OMT_SDK_DIR}/lib/libvmx.so)
  add_library(OMT::omt SHARED IMPORTED)
  set_target_properties(OMT::omt PROPERTIES
    IMPORTED_LOCATION ${OMT_SDK_DIR}/lib/libomt.so
    IMPORTED_NO_SONAME TRUE
    INTERFACE_INCLUDE_DIRECTORIES ${OMT_SDK_DIR}/include)
  set(KG_HAVE_OMT ON)
  message(STATUS "OMT SDK found: ${OMT_SDK_DIR}")
else()
  set(KG_HAVE_OMT OFF)
  message(STATUS "OMT SDK not found: ${OMT_SDK_DIR}")
endif()

if(FFMPEG_FOUND AND SRT_FOUND AND KG_HAVE_OMT)
  add_executable(kloudgateway
    src/app/main.cpp
    src/media/channel.cpp
    src/media/srt_output.cpp
    src/web/web_server.cpp)
  target_include_directories(kloudgateway PRIVATE src)
  target_link_libraries(kloudgateway PRIVATE
    gateway_core PkgConfig::FFMPEG PkgConfig::SRT OMT::omt Threads::Threads)
  target_compile_definitions(kloudgateway PRIVATE
    KG_INSTALL_WEB_ROOT="${CMAKE_INSTALL_FULL_DATADIR}/kloudgateway/web")
  set_target_properties(kloudgateway PROPERTIES
    BUILD_RPATH "${OMT_SDK_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/kloudgateway")
  # Classic DT_RPATH also governs how the bundled FFmpeg libraries find each
  # other (libavcodec -> libswresample); DT_RUNPATH would not.
  target_link_options(kloudgateway PRIVATE -Wl,--disable-new-dtags)

  add_custom_target(webui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web
    DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html ${CMAKE_SOURCE_DIR}/web/style.css
            ${CMAKE_SOURCE_DIR}/web/app.js ${CMAKE_SOURCE_DIR}/web/logo.svg)
  add_dependencies(kloudgateway webui)

  install(TARGETS kloudgateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/kloudgateway/web)
  install(FILES ${OMT_SDK_DIR}/lib/libomt.so ${OMT_SDK_DIR}/lib/libvmx.so
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/kloudgateway)
  if(FFMPEG_LIBRARY_DIRS)
    list(GET FFMPEG_LIBRARY_DIRS 0 _ffmpeg_libdir)
    if(NOT _ffmpeg_libdir MATCHES "^(/usr/lib|/lib|/usr/local/lib)")
      message(STATUS "Bundling non-system FFmpeg from ${_ffmpeg_libdir}")
      install(DIRECTORY ${_ffmpeg_libdir}/
              DESTINATION ${CMAKE_INSTALL_LIBDIR}/kloudgateway
              FILES_MATCHING
              PATTERN "libavformat.so*"
              PATTERN "libavcodec.so*"
              PATTERN "libavutil.so*"
              PATTERN "libswscale.so*"
              PATTERN "libswresample.so*"
              PATTERN "libdav1d.so*")
    endif()
  endif()
else()
  set(_missing "")
  if(NOT FFMPEG_FOUND)
    string(APPEND _missing " FFmpeg")
  endif()
  if(NOT SRT_FOUND)
    string(APPEND _missing " libsrt")
  endif()
  if(NOT KG_HAVE_OMT)
    string(APPEND _missing " OMT")
  endif()
  if(KLOUDGATEWAY_REQUIRE_APP)
    message(FATAL_ERROR "Cannot build kloudgateway; missing:${_missing}")
  endif()
  message(WARNING "Daemon target disabled; missing:${_missing}. Core tests still build.")
endif()

# The service file expects /etc/kloudgateway, and systemd only reads units,
# sysusers, and tmpfiles from <prefix>/lib (never the multiarch libdir).
install(FILES config/kloudgateway.json DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/kloudgateway)
install(FILES packaging/kloudgateway.service DESTINATION lib/systemd/system)
install(FILES packaging/kloudgateway.sysusers
        DESTINATION lib/sysusers.d RENAME kloudgateway.conf)
install(FILES packaging/kloudgateway.tmpfiles
        DESTINATION lib/tmpfiles.d RENAME kloudgateway.conf)
install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/license/kloudgateway)
install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/kloudgateway RENAME copyright)

include(CTest)
if(BUILD_TESTING)
  add_executable(config_test tests/config_test.cpp)
  target_link_libraries(config_test PRIVATE gateway_core)
  add_test(NAME config COMMAND config_test)
  set_tests_properties(config PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Debian package: `cpack -G DEB` in a build configured with
# -DCMAKE_INSTALL_PREFIX=/usr. The maintainer scripts perform the sysusers,
# tmpfiles, and systemd steps that the README otherwise lists by hand.
if(TARGET kloudgateway)
  set(KLOUDGATEWAY_DEB_RELEASE "1" CACHE STRING
      "Debian package revision, e.g. 1~ubuntu26.04 for a distribution-specific build")
  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGE_NAME kloudgateway)
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_CONTACT "8Kloud <noreply@8kloud.com>")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/8Kloud/8KloudGateway")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "SRT to OMT gateway with SRT relay and MKV recording")
  set(CPACK_PACKAGE_DESCRIPTION
      "Receives up to four MPEG-TS contribution feeds over SRT and publishes each as an OMT source, relays it unchanged to downstream SRT callers, or both, with optional packet-copy MKV recording. Decodes H.264, HEVC, and AV1 with CUDA/NVDEC or in software. Bundles the OMT libraries and a pinned FFmpeg with AV1-in-MPEG-TS support.")
  set(CPACK_SET_DESTDIR ON)
  set(CPACK_DEBIAN_PACKAGE_RELEASE ${KLOUDGATEWAY_DEB_RELEASE})
  # The package version keeps the dpkg-correct "~", but GitHub release
  # assets cannot contain it, so the file name uses "." instead.
  execute_process(COMMAND dpkg --print-architecture
                  OUTPUT_VARIABLE _deb_arch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _deb_arch)
    set(_deb_arch amd64)
  endif()
  string(REPLACE "~" "." _deb_file_release "${KLOUDGATEWAY_DEB_RELEASE}")
  set(CPACK_DEBIAN_FILE_NAME
      "kloudgateway_${PROJECT_VERSION}-${_deb_file_release}_${_deb_arch}.deb")
  set(CPACK_DEBIAN_PACKAGE_SECTION video)
  set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
  # Dependencies are listed by hand: dpkg-shlibdeps maps the bundled OMT,
  # FFmpeg, and dav1d sonames onto the distribution's packages (with exact
  # version pins), which this package deliberately does not use. The floors
  # are what dpkg-shlibdeps reported for the non-bundled libraries on the
  # Ubuntu 26.04 build host.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "libc6 (>= 2.43), libstdc++6 (>= 13.1), libgcc-s1 (>= 3.0), libsrt1.5-gnutls (>= 1.5.4), systemd")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      ${CMAKE_SOURCE_DIR}/packaging/debian/conffiles
      ${CMAKE_SOURCE_DIR}/packaging/debian/postinst
      ${CMAKE_SOURCE_DIR}/packaging/debian/prerm
      ${CMAKE_SOURCE_DIR}/packaging/debian/postrm)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
  include(CPack)
endif()
