add_executable(virgil_unit_tests unit_tests.cpp)
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
