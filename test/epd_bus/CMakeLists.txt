add_executable(epd_bus_tests
  EpdBusTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.cpp
)
target_include_directories(epd_bus_tests BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
target_link_libraries(epd_bus_tests PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(epd_bus_tests)
