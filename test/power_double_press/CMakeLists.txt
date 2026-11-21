add_executable(PowerDoublePressTest
  PowerDoublePressTest.cpp
)

target_include_directories(PowerDoublePressTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PowerDoublePressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerDoublePressTest)
