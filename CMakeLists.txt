cmake_minimum_required(VERSION 3.16)
project(virgil VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(_alsa_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_alsa_default ON)
endif()
option(VIRGIL_BUILD_ALSA "Build the ALSA pcm plugin (Linux)" ${_alsa_default})
option(VIRGIL_BUILD_COREAUDIO "Build the CoreAudio AudioServerPlugIn (macOS)" ${APPLE})
option(VIRGIL_BUILD_ASIO "Build the ASIO driver (Windows; needs ASIO_SDK_DIR)" ${WIN32})
option(VIRGIL_BUILD_TESTS "Build unit and loopback tests" ON)
set(ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (contains common/asio.h)")

find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W4 /permissive- /wd4324)  # C4324: intentional alignas padding
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00)
endif()

# ---- client library: everything a driver needs to attach to virgild ----------
add_library(virgil_client STATIC
  src/common/platform.cpp
  src/common/shm.cpp
  src/common/client.cpp)
target_include_directories(virgil_client PUBLIC include)
set_target_properties(virgil_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(virgil_client PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(virgil_client PUBLIC avrt winmm advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(virgil_client PUBLIC rt)
endif()

# ---- network library: PTP + Dante (Inferno) --------------------------------------
add_library(virgil_net STATIC
  src/common/log.cpp
  src/common/net.cpp
  src/common/ptp.cpp
  src/common/config.cpp
  src/daemon/engine.cpp)
include(cmake/rust_bridge.cmake)
target_link_libraries(virgil_net PUBLIC virgil_client virgil_dante)
if(WIN32)
  target_link_libraries(virgil_net PUBLIC ws2_32 iphlpapi)
endif()

include(GNUInstallDirs)

# Control panel page, embedded into virgild.
set(VIRGIL_UI_HEADER ${CMAKE_BINARY_DIR}/generated/ui_html.h)
add_custom_command(
  OUTPUT ${VIRGIL_UI_HEADER}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/src/daemon/ui/index.html
          -DOUT=${VIRGIL_UI_HEADER} -DNAME=kUiHtml -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/src/daemon/ui/index.html ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding control panel UI")

# App icon (branding/make_logo.py generates the files).
set(VIRGIL_ICON_SOURCES "")
if(WIN32)
  enable_language(RC)
  # Forward slashes work for both windres and rc.exe.
  file(WRITE ${CMAKE_BINARY_DIR}/generated/virgil_icon.rc
       "1 ICON \"${CMAKE_SOURCE_DIR}/branding/virgil.ico\"\n")
  set(VIRGIL_ICON_SOURCES ${CMAKE_BINARY_DIR}/generated/virgil_icon.rc)
endif()

add_executable(virgild src/daemon/main.cpp src/daemon/control.cpp src/daemon/http_server.cpp
                    ${VIRGIL_UI_HEADER} ${VIRGIL_ICON_SOURCES})
target_include_directories(virgild PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(virgild PRIVATE VIRGIL_VERSION="${PROJECT_VERSION}")
target_link_libraries(virgild PRIVATE virgil_net)

add_executable(virgil-latency-probe tools/virgil_latency_probe.cpp)
target_link_libraries(virgil-latency-probe PRIVATE virgil_client)

# Virgil Control: opens the control panel (starts a portable virgild if needed).
if(APPLE)
  # Native menu bar app (AppKit) with the same window as on Windows/Linux.
  enable_language(OBJCXX)
  set_source_files_properties(branding/virgil.icns branding/virgil-256.png PROPERTIES
                              MACOSX_PACKAGE_LOCATION Resources)
  add_executable(virgil-control MACOSX_BUNDLE tools/control_mac/main.mm tools/control_common/api.cpp
                 tools/control_common/settings_model.cpp tools/control_common/view.cpp
                 branding/virgil.icns branding/virgil-256.png)
  target_include_directories(virgil-control PRIVATE tools/control_common)
  target_compile_definitions(virgil-control PRIVATE VIRGIL_VERSION="${PROJECT_VERSION}")
  target_compile_options(virgil-control PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(virgil-control PRIVATE "-framework AppKit")
  set_target_properties(virgil-control PROPERTIES
    MACOSX_BUNDLE_ICON_FILE virgil.icns
    OUTPUT_NAME "Virgil Control"
    MACOSX_BUNDLE_BUNDLE_NAME "Virgil Control"
    MACOSX_BUNDLE_GUI_IDENTIFIER org.virgil.control
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(WIN32)
  # Native tray app with a Direct2D status window and a settings dialog.
  add_executable(virgil-control WIN32 tools/control_win/main.cpp tools/control_common/api.cpp
                 tools/control_win/control.rc)
  target_include_directories(virgil-control PRIVATE tools/control_win tools/control_common branding)
  if(MINGW)
    target_link_options(virgil-control PRIVATE -municode)
  elseif(MSVC)
    target_link_options(virgil-control PRIVATE /MANIFEST:NO)  # control.rc embeds ours
  endif()
  target_link_libraries(virgil-control PRIVATE d2d1 dwrite windowscodecs dwmapi comctl32 ole32
                        ws2_32 shell32 user32 gdi32 advapi32)
else()
  # Native GTK 3 window with tray icon; without GTK (headless builds) the
  # small launcher that opens the browser panel instead.
  option(VIRGIL_BUILD_GTK_CONTROL "Build the GTK 3 Virgil Control app (Linux)" ON)
  if(VIRGIL_BUILD_GTK_CONTROL)
    find_package(PkgConfig)
  endif()
  if(VIRGIL_BUILD_GTK_CONTROL AND PKG_CONFIG_FOUND)
    pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
  endif()
  if(GTK3_FOUND)
    add_executable(virgil-control tools/control_linux/main.cpp tools/control_common/api.cpp
                   tools/control_common/settings_model.cpp tools/control_common/view.cpp)
    target_include_directories(virgil-control PRIVATE tools/control_common)
    target_compile_definitions(virgil-control PRIVATE VIRGIL_VERSION="${PROJECT_VERSION}")
    target_link_libraries(virgil-control PRIVATE PkgConfig::GTK3 ${CMAKE_DL_LIBS})
  else()
    message(STATUS "GTK 3 not found: virgil-control opens the browser control panel")
    add_executable(virgil-control tools/virgil_control.cpp)
  endif()
  install(FILES packaging/linux/virgil-control.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/linux/virgil-control-autostart.desktop DESTINATION /etc/xdg/autostart
          RENAME virgil-control.desktop)
  install(FILES branding/virgil.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  install(FILES branding/virgil-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
          RENAME virgil.png)
endif()
if(NOT WIN32)
  target_link_libraries(virgil-control PRIVATE virgil_client)
endif()

install(TARGETS virgild virgil-latency-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS virgil-control RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
install(FILES README.md LICENSE NOTICE.md config/virgil.conf.example DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  configure_file(packaging/linux/virgild.service.in ${CMAKE_BINARY_DIR}/virgild.service @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/virgild.service DESTINATION lib/systemd/system)
  install(FILES packaging/virgil.conf DESTINATION /etc/virgil)
endif()

# ---- drivers -----------------------------------------------------------------
if(VIRGIL_BUILD_ALSA)
  add_subdirectory(drivers/alsa)
endif()
if(VIRGIL_BUILD_COREAUDIO AND APPLE)
  add_subdirectory(drivers/coreaudio)
endif()
if(VIRGIL_BUILD_ASIO AND WIN32)
  add_subdirectory(drivers/asio)
endif()

if(VIRGIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- Linux packages (packaging/linux/build-packages.sh) ----------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(VIRGIL_PACKAGE_MAINTAINER "Virgil maintainers <virgil@example.invalid>" CACHE STRING
      "Maintainer field of .deb/.rpm packages")
  set(VIRGIL_PACKAGE_LICENSE "GPL-3.0-or-later" CACHE STRING "License field of .rpm packages")
  set(CPACK_PACKAGE_NAME virgil)
  set(CPACK_PACKAGE_VENDOR "Virgil Project")
  set(CPACK_PACKAGE_CONTACT "${VIRGIL_PACKAGE_MAINTAINER}")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Low-latency Dante-compatible virtual soundcard (unofficial)")
  set(CPACK_PACKAGE_DESCRIPTION
      "Virgil joins Dante networks as an audio device and exposes its channels as an ALSA soundcard. Network compatibility comes from Inferno, an independent open-source project. Virgil is not affiliated with or endorsed by Audinate; Dante is a registered trademark of Audinate Pty Ltd.")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Audbol/Virgil-Virtual-Soundcard")

  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      ${CMAKE_SOURCE_DIR}/packaging/linux/deb/postinst
      ${CMAKE_SOURCE_DIR}/packaging/linux/deb/prerm
      ${CMAKE_SOURCE_DIR}/packaging/linux/deb/postrm
      ${CMAKE_SOURCE_DIR}/packaging/linux/deb/conffiles)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

  set(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
  set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
  set(CPACK_RPM_PACKAGE_LICENSE "${VIRGIL_PACKAGE_LICENSE}")
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/post)
  set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/preun)
  set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/postun)
  set(CPACK_RPM_USER_FILELIST "%config(noreplace) /etc/virgil/virgil.conf"
                              "%config(noreplace) /etc/alsa/conf.d/50-virgil.conf")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /etc/alsa /etc/alsa/conf.d /usr/lib/systemd /usr/lib/systemd/system
      /usr/lib64/alsa-lib /usr/share/doc)
  include(CPack)
endif()
