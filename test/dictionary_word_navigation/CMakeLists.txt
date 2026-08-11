add_executable(dictionary_word_navigation_tests
  DictionaryWordNavigationTest.cpp
)

target_link_libraries(dictionary_word_navigation_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(dictionary_word_navigation_tests PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

gtest_discover_tests(dictionary_word_navigation_tests)
