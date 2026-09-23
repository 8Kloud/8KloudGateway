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

include(CTest)
if(BUILD_TESTING)
  add_executable(config_test tests/config_test.cpp)
  target_link_libraries(config_test PRIVATE gateway_core)
  add_test(NAME config COMMAND config_test)
  set_tests_properties(config PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
