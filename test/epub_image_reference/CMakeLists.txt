find_package(EXPAT REQUIRED)

add_executable(epub_image_reference_tests
  EpubImageReferenceTest.cpp
)

target_link_libraries(epub_image_reference_tests PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

target_include_directories(epub_image_reference_tests PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub/parsers
)

gtest_discover_tests(epub_image_reference_tests)
