cmake_minimum_required(VERSION 3.20)
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(fixture "${CMAKE_CURRENT_LIST_DIR}/apple-abi-probe")
if(NOT DEFINED TEST_GENERATOR)
    set(TEST_GENERATOR Ninja)
    find_program(TEST_MAKE_PROGRAM NAMES ninja ninja-build REQUIRED)
endif()
set(generator_args -G "${TEST_GENERATOR}")
if(DEFINED TEST_MAKE_PROGRAM AND NOT TEST_MAKE_PROGRAM STREQUAL "")
    list(APPEND generator_args "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(TEST_OUTPUT "${TEST_OUTPUT}/run-${run_id}")
if(EXISTS "${TEST_OUTPUT}")
    message(FATAL_ERROR "Bundle fixture output already exists")
endif()
file(MAKE_DIRECTORY "${TEST_OUTPUT}")
include("${CMAKE_CURRENT_LIST_DIR}/ConsumerBuildContext.cmake")
unset(ENV{CMAKE_TOOLCHAIN_FILE})
unset(CMAKE_TOOLCHAIN_FILE)
unset(CMAKE_TOOLCHAIN_FILE CACHE)
set(CMAKE_USER_MAKE_RULES_OVERRIDE_C "")
set(CMAKE_USER_MAKE_RULES_OVERRIDE_CXX "")
moq_write_consumer_abi_context("${TEST_OUTPUT}/none.cmake")
set(CMAKE_TOOLCHAIN_FILE "${fixture}/Toolchain.cmake")
moq_write_consumer_abi_context("${TEST_OUTPUT}/selected.cmake")

# Real -C subprocesses, but LANGUAGES NONE: these check ownership, not ABI proof.
function(check_bundle name diagnostic)
    if(EXISTS "${TEST_OUTPUT}/${name}/CMakeCache.txt")
        message(FATAL_ERROR "Bundle fixture needs a fresh directory: ${name}")
    endif()
    execute_process(COMMAND ${ARGN} ${generator_args}
        -S "${fixture}/bundle-child"
        -B "${TEST_OUTPUT}/${name}" RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    file(WRITE "${TEST_OUTPUT}/${name}.log" "${stdout}${stderr}")
    if(diagnostic STREQUAL "")
        if(NOT result EQUAL 0 OR "${stdout}${stderr}" MATCHES "[Ww]arning|CMake Error")
            message(FATAL_ERROR "Bundle case ${name} failed: ${stdout}${stderr}")
        endif()
    else()
        string(REGEX MATCHALL "CMake Error" errors "${stdout}${stderr}")
        list(LENGTH errors error_count)
        if(NOT result STREQUAL "1" OR NOT error_count EQUAL 1 OR
           "${stdout}${stderr}" MATCHES "[Ww]arning" OR
           NOT "${stdout}${stderr}" MATCHES "${diagnostic}")
            message(FATAL_ERROR "Bundle case ${name} did not reject cleanly: ${stdout}${stderr}")
        endif()
    endif()
endfunction()

set(mismatch "Consumer ABI bundle toolchain mismatch")
check_bundle(none "" "${CMAKE_COMMAND}" -C "${TEST_OUTPUT}/none.cmake")
check_bundle(later-toolchain "${mismatch}" "${CMAKE_COMMAND}"
    -C "${TEST_OUTPUT}/none.cmake" "-DCMAKE_TOOLCHAIN_FILE=${fixture}/Toolchain.cmake")
check_bundle(earlier-toolchain "${mismatch}" "${CMAKE_COMMAND}"
    "-DCMAKE_TOOLCHAIN_FILE=${fixture}/Toolchain.cmake" -C "${TEST_OUTPUT}/none.cmake")
check_bundle(environment-toolchain "${mismatch}" "${CMAKE_COMMAND}" -E env
    "CMAKE_TOOLCHAIN_FILE=${fixture}/Toolchain.cmake" "${CMAKE_COMMAND}"
    -C "${TEST_OUTPUT}/none.cmake")
check_bundle(equal "" "${CMAKE_COMMAND}" -C "${TEST_OUTPUT}/selected.cmake"
    "-DCMAKE_TOOLCHAIN_FILE=${fixture}/Toolchain.cmake")
check_bundle(equal-environment "" "${CMAKE_COMMAND}" -E env
    "CMAKE_TOOLCHAIN_FILE=${fixture}/Toolchain.cmake" "${CMAKE_COMMAND}"
    -C "${TEST_OUTPUT}/selected.cmake")
check_bundle(empty "" "${CMAKE_COMMAND}" -C "${TEST_OUTPUT}/none.cmake"
    -DCMAKE_TOOLCHAIN_FILE= -DCMAKE_USER_MAKE_RULES_OVERRIDE_C=
    -DCMAKE_USER_MAKE_RULES_OVERRIDE_CXX=)
check_bundle(empty-mismatch "${mismatch}" "${CMAKE_COMMAND}"
    -C "${TEST_OUTPUT}/selected.cmake" -DCMAKE_TOOLCHAIN_FILE=)
check_bundle(hook-collision "Consumer ABI bundle cannot replace caller" "${CMAKE_COMMAND}"
    -DCMAKE_PROJECT_INCLUDE_BEFORE= -C "${TEST_OUTPUT}/none.cmake")
