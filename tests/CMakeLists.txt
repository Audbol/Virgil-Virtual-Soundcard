add_executable(virgil_unit_tests unit_tests.cpp ${CMAKE_SOURCE_DIR}/src/daemon/diagnostics.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/api.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/settings_model.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/desktop_audio_settings.cpp
               ${CMAKE_SOURCE_DIR}/tools/control_common/view.cpp)
target_include_directories(virgil_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/daemon
                           ${CMAKE_SOURCE_DIR}/tools/control_common)
target_link_libraries(virgil_unit_tests PRIVATE virgil_net)
add_test(NAME unit COMMAND virgil_unit_tests)

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
if(WIN32)
  add_executable(desktop_audio_probe desktop_audio_probe.cpp ${CMAKE_SOURCE_DIR}/tools/control_win/wasapi.cpp)
  target_include_directories(desktop_audio_probe PRIVATE ${CMAKE_SOURCE_DIR}/tools/control_win)
  target_link_libraries(desktop_audio_probe PRIVATE virgil_client ole32)
endif()
