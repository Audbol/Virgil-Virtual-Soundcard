add_executable(dsv_unit_tests unit_tests.cpp)
target_link_libraries(dsv_unit_tests PRIVATE dsv_net)
add_test(NAME unit COMMAND dsv_unit_tests)

add_executable(dsv_loopback_test loopback_test.cpp)
target_link_libraries(dsv_loopback_test PRIVATE dsv_net)
add_test(NAME loopback COMMAND dsv_loopback_test)

add_executable(dsv_latency_probe latency_probe.cpp)
target_link_libraries(dsv_latency_probe PRIVATE dsv_net)
