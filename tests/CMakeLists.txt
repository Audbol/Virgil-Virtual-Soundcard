add_executable(dsv_unit_tests unit_tests.cpp)
target_link_libraries(dsv_unit_tests PRIVATE dsv_net)
add_test(NAME unit COMMAND dsv_unit_tests)

add_executable(dsv_loopback_test loopback_test.cpp)
target_link_libraries(dsv_loopback_test PRIVATE dsv_net)
add_test(NAME loopback COMMAND dsv_loopback_test)

add_executable(dsv_latency_probe latency_probe.cpp)
target_link_libraries(dsv_latency_probe PRIVATE dsv_net)

if(WIN32 AND TARGET DSVAsio)
  add_executable(dsv_asio_host_test asio_host_test.cpp)
  target_include_directories(dsv_asio_host_test PRIVATE "${ASIO_SDK_DIR}/common")
  target_link_libraries(dsv_asio_host_test PRIVATE ole32 uuid)
endif()
