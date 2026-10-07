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
  target_link_libraries(dsv_client PUBLIC avrt winmm)
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

add_executable(dsvd src/daemon/main.cpp)
target_link_libraries(dsvd PRIVATE dsv_net)
install(TARGETS dsvd RUNTIME DESTINATION bin)
install(FILES config/dsv.conf.example DESTINATION share/dsv)

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
