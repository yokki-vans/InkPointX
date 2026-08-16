add_executable(x3_refresh_policy_tests
  X3RefreshPolicyTest.cpp
)

target_link_libraries(x3_refresh_policy_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(x3_refresh_policy_tests)
