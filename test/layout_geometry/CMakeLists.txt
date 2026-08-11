add_executable(layout_geometry_tests
  LayoutGeometryTest.cpp
)

target_link_libraries(layout_geometry_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(layout_geometry_tests PRIVATE
  ${REPO_ROOT}/src
)

gtest_discover_tests(layout_geometry_tests)
