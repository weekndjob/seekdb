# Focused full-text regression module, using the production shared library.
file(GLOB_RECURSE FTS_REGRESSION_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/storage/fts/test_*.cpp")
add_executable(storage_tests ${FTS_REGRESSION_SOURCES})
target_link_libraries(storage_tests PRIVATE oceanbase gtest_main gtest pthread)
set_target_properties(storage_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/storage")
add_test(NAME storage_tests COMMAND storage_tests)
add_executable(sql_tests sql/optimizer/test_fts_score_utils.cpp)
target_link_libraries(sql_tests PRIVATE oceanbase gtest_main gtest pthread)
set_target_properties(sql_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sql")
add_test(NAME sql_tests COMMAND sql_tests)
add_custom_target(pretest DEPENDS storage_tests sql_tests)
