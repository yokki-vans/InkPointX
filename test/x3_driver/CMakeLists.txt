add_executable(x3_driver_tests
  Uc8279DriverTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8279Driver.cpp
)
target_include_directories(x3_driver_tests BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
target_link_libraries(x3_driver_tests PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(x3_driver_tests)

add_executable(uc8179_driver_tests
  Uc8179DriverTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8179Driver.cpp
)
target_include_directories(uc8179_driver_tests BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
target_compile_definitions(uc8179_driver_tests PRIVATE TEST_DEVICE_X4=1)
target_link_libraries(uc8179_driver_tests PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(uc8179_driver_tests)

add_executable(uc8253_driver_tests
  Uc8253DriverTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp
)
target_include_directories(uc8253_driver_tests BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
target_link_libraries(uc8253_driver_tests PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(uc8253_driver_tests)
