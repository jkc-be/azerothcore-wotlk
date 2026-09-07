if("${MODULE_MOD-ALLES}" STREQUAL "static")
  if(BUILD_TESTING)
    enable_testing()
    add_executable(alles_statement_layout_mismatch
      "${CMAKE_CURRENT_LIST_DIR}/tests/integration/StatementLayoutMismatch.cpp")
    target_link_libraries(alles_statement_layout_mismatch PRIVATE database)
    target_include_directories(alles_statement_layout_mismatch PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    add_test(NAME alles_statement_layout_mismatch
      COMMAND "${CMAKE_COMMAND}"
        "-DCONSUMER=$<TARGET_FILE:alles_statement_layout_mismatch>"
        -P "${CMAKE_CURRENT_LIST_DIR}/tests/integration/ExpectLayoutMismatch.cmake")

    file(GLOB ALLES_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_LIST_DIR}/tests/domain/*.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/storage/*.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/runtime/*.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/interpreter/*.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/tests/perception/*.cpp")
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES ${ALLES_TEST_SOURCES})
    set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
      "${CMAKE_CURRENT_LIST_DIR}/src/domain"
      "${CMAKE_CURRENT_LIST_DIR}/src")
  endif()
endif()
