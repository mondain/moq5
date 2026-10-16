cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED TEST_EXE OR NOT EXISTS "${TEST_EXE}")
    message(FATAL_ERROR "fill setup checker requires the actual fixture executable")
endif()

set(failures 0)
foreach(kind IN ITEMS config thread)
    if(kind STREQUAL "config")
        set(expected "FAIL: config refused: not valid JSON\n")
    else()
        set(expected "FAIL: serve thread\n")
    endif()
    execute_process(
        COMMAND "${TEST_EXE}" unused-cert unused-key "setup_${kind}_refusal"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        TIMEOUT 10)
    if(NOT "${result}" STREQUAL "1" OR NOT "${output}" STREQUAL "" OR
       NOT "${error}" STREQUAL "${expected}")
        message(STATUS "setup_${kind}_refusal: expected exit1 with its exact diagnostic; got ${result}\nstdout=${output}\nstderr=${error}")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()
if(failures GREATER 0)
    message(FATAL_ERROR "fill setup failure propagation: ${failures} cases failed")
endif()
message(STATUS "fill setup failure propagation: 2/2 refused exactly")
