add_executable(ProgressFormatTest
  ProgressFormatTest.cpp
)

target_link_libraries(ProgressFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressFormatTest)
