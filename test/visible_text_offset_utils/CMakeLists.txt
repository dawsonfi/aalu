add_executable(VisibleTextOffsetUtilsTest
  VisibleTextOffsetUtilsTest.cpp
)

target_link_libraries(VisibleTextOffsetUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleTextOffsetUtilsTest)
