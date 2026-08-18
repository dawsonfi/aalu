add_executable(InflateStreamTest
  InflateStreamTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
)

target_include_directories(InflateStreamTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(InflateStreamTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InflateStreamTest)
