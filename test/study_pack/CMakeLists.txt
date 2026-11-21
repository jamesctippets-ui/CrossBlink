FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(StudyPackTest
  StudyPackTest.cpp
  StudyPageLayoutTest.cpp
  ${REPO_ROOT}/lib/StudyPack/StudyPackParse.cpp
  ${REPO_ROOT}/lib/StudyPack/StudySrs.cpp
  ${REPO_ROOT}/lib/StudyPack/StudyStats.cpp
  ${REPO_ROOT}/lib/StudyPack/StudyPageLayout.cpp
)
target_include_directories(StudyPackTest PRIVATE ${REPO_ROOT}/lib/StudyPack ${arduinojson_SOURCE_DIR}/src)
target_link_libraries(StudyPackTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(StudyPackTest)
