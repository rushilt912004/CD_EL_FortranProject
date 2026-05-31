find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
    add_test(
        NAME CommonAnalyzerTestSuite
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
            $<TARGET_FILE:common-analyzer>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
