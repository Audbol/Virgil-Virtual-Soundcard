cmake_minimum_required(VERSION 3.16)
project(dsv VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(_alsa_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_alsa_default ON)
endif()
option(DSV_BUILD_ALSA "Build the ALSA pcm plugin (Linux)" ${_alsa_default})
option(DSV_BUILD_COREAUDIO "Build the CoreAudio AudioServerPlugIn (macOS)" ${APPLE})
option(DSV_BUILD_ASIO "Build the ASIO driver (Windows; needs ASIO_SDK_DIR)" ${WIN32})
option(DSV_BUILD_TESTS "Build unit and loopback tests" ON)
set(ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (contains common/asio.h)")

find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W4 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00)
endif()

# ---- client library: everything a driver needs to attach to dsvd ----------
add_library(dsv_client STATIC
  src/common/platform.cpp
  src/common/shm.cpp
  src/common/client.cpp)
target_include_directories(dsv_client PUBLIC include)
set_target_properties(dsv_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dsv_client PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(dsv_client PUBLIC avrt winmm advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(dsv_client PUBLIC rt)
endif()

# ---- network library: AES67 / PTP / SAP --------------------------------------
add_library(dsv_net STATIC
  src/common/log.cpp
  src/common/net.cpp
  src/common/sdp.cpp
  src/common/sap.cpp
  src/common/ptp.cpp
  src/common/config.cpp
  src/daemon/engine.cpp)
target_link_libraries(dsv_net PUBLIC dsv_client)
if(WIN32)
  target_link_libraries(dsv_net PUBLIC ws2_32 iphlpapi)
endif()

include(GNUInstallDirs)

add_executable(dsvd src/daemon/main.cpp)
target_link_libraries(dsvd PRIVATE dsv_net)

add_executable(dsv-latency-probe tools/dsv_latency_probe.cpp)
target_link_libraries(dsv-latency-probe PRIVATE dsv_client)

install(TARGETS dsvd dsv-latency-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md config/dsv.conf.example DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  configure_file(packaging/linux/dsvd.service.in ${CMAKE_BINARY_DIR}/dsvd.service @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/dsvd.service DESTINATION lib/systemd/system)
  install(FILES packaging/dsv.conf DESTINATION /etc/dsv)
endif()

# ---- drivers -----------------------------------------------------------------
if(DSV_BUILD_ALSA)
  add_subdirectory(drivers/alsa)
endif()
if(DSV_BUILD_COREAUDIO AND APPLE)
  add_subdirectory(drivers/coreaudio)
endif()
if(DSV_BUILD_ASIO AND WIN32)
  add_subdirectory(drivers/asio)
endif()

if(DSV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- Linux packages (packaging/linux/build-packages.sh) ----------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DSV_PACKAGE_MAINTAINER "DSV maintainers <dsv@example.invalid>" CACHE STRING
      "Maintainer field of .deb/.rpm packages")
  set(DSV_PACKAGE_LICENSE "Unspecified" CACHE STRING "License field of .rpm packages")
  set(CPACK_PACKAGE_NAME dsv)
  set(CPACK_PACKAGE_VENDOR "DSV Project")
  set(CPACK_PACKAGE_CONTACT "${DSV_PACKAGE_MAINTAINER}")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Low-latency AES67 virtual soundcard, compatible with Dante devices in AES67 mode")
  set(CPACK_PACKAGE_DESCRIPTION
      "DSV exposes AES67 network audio as an ALSA device. The dsvd daemon follows the PTPv2 grandmaster, sends and receives AES67 RTP flows and announces them via SAP.")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/audbol/dsv-dante-soundcard-virtual")

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
  set(CPACK_RPM_PACKAGE_LICENSE "${DSV_PACKAGE_LICENSE}")
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/post)
  set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/preun)
  set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/rpm/postun)
  set(CPACK_RPM_USER_FILELIST "%config(noreplace) /etc/dsv/dsv.conf"
                              "%config(noreplace) /etc/alsa/conf.d/50-dsv.conf")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /etc/alsa /etc/alsa/conf.d /usr/lib/systemd /usr/lib/systemd/system
      /usr/lib64/alsa-lib /usr/share/doc)
  include(CPack)
endif()
