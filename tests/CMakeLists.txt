add_executable(virgil_unit_tests unit_tests.cpp ${CMAKE_SOURCE_DIR}/src/daemon/diagnostics.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/api.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/settings_model.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/desktop_audio_settings.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/view.cpp)
target_include_directories(virgil_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/daemon
                           ${CMAKE_SOURCE_DIR}/tools/control_common)
target_link_libraries(virgil_unit_tests PRIVATE virgil_net)
add_test(NAME unit COMMAND virgil_unit_tests)

# Desktop audio engine (Linux / macOS engine; built everywhere) against a fake
# sound system and an in-process Virgil engine.
add_executable(virgil_desktop_audio_test desktop_audio_test.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/desktop_audio_engine.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/desktop_audio_settings.cpp)
target_include_directories(virgil_desktop_audio_test PRIVATE ${CMAKE_SOURCE_DIR}/tools/control_common)
target_link_libraries(virgil_desktop_audio_test PRIVATE virgil_net)
add_test(NAME desktop_audio COMMAND virgil_desktop_audio_test)

# Linux: desktop audio with a real PulseAudio / PipeWire server
# (tests/desktop_audio_e2e.sh; needs a sound server, so not run by ctest).
if(TARGET virgil_desktop_audio AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(virgil_desktop_audio_e2e desktop_audio_e2e.cpp)
  target_link_libraries(virgil_desktop_audio_e2e PRIVATE virgil_desktop_audio virgil_net)
endif()

# macOS: the Core Audio backend lists devices and apps, reports what the OS
# version allows and opens / closes a render stream (skipped without a
# device). Creates no process tap (that would need the recording permission).
if(TARGET virgil_desktop_audio AND APPLE)
  add_executable(virgil_desktop_audio_coreaudio_test desktop_audio_coreaudio_test.cpp)
  target_link_libraries(virgil_desktop_audio_coreaudio_test PRIVATE virgil_desktop_audio)
  add_test(NAME desktop_audio_coreaudio COMMAND virgil_desktop_audio_coreaudio_test)
endif()

add_executable(virgil_loopback_test loopback_test.cpp)
target_link_libraries(virgil_loopback_test PRIVATE virgil_net)
add_test(NAME loopback COMMAND virgil_loopback_test)

if(WIN32 AND TARGET VirgilAsio)
  add_executable(virgil_asio_host_test asio_host_test.cpp)
  target_include_directories(virgil_asio_host_test PRIVATE "${ASIO_SDK_DIR}/common")
  target_link_libraries(virgil_asio_host_test PRIVATE ole32 uuid)
endif()

# End-to-end helper for tests/e2e_netns.sh (two daemons, one test network).
add_executable(virgil_tone tone_tool.cpp)
target_link_libraries(virgil_tone PRIVATE virgil_client)

# Windows: checks desktop audio capture (WASAPI loopback, process loopback) on a
# PC without Virgil; writes a WAV file. Not installed.
# (built from the top-level CMakeLists.txt, so release builds include it)
