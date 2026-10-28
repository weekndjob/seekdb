# Focused SQL regression target. Use Google Test's shared main and the same
# production library as the repository's Linux pretest contract.
file(GLOB_RECURSE SQL_REGRESSION_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/sql/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sql/*_test.cpp")
add_executable(sql_tests ${SQL_REGRESSION_SOURCES})
target_link_libraries(sql_tests PRIVATE oceanbase gtest_main gtest pthread)
set_target_properties(sql_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sql")
add_test(NAME sql_tests COMMAND sql_tests)
add_custom_target(pretest DEPENDS sql_tests)
