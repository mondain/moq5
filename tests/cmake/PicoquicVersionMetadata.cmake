cmake_minimum_required(VERSION 3.13)

if(NOT MOQ_SOURCE OR NOT MOQ_WORK)
    message(FATAL_ERROR "MOQ_SOURCE and MOQ_WORK are required")
endif()
find_program(_git git)
if(NOT _git)
    message(FATAL_ERROR "Git is required to exercise the provider patch")
endif()
file(MAKE_DIRECTORY "${MOQ_WORK}/source/picoquic")

# Minimal upstream metadata section, with the real series patch applied below.
set(_old [=[include(CMakePackageConfigHelpers)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(PROJECT_VERSION "0.0.0-${GIT_HASH}")
message(STATUS "Project version: ${PROJECT_VERSION}")

]=])
set(_prefix [=[cmake_minimum_required(VERSION 3.13)
project(picoquic VERSION 1.1.53.0 LANGUAGES NONE)
]=])
set(_suffix [=[file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/observed.txt" "${PROJECT_VERSION}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/package-version.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
]=])
file(WRITE "${MOQ_WORK}/source/picoquic/CMakeLists.txt" "${_prefix}${_old}${_suffix}")

# Old behavior must be distinguishable even beneath an enclosing Git checkout.
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${MOQ_WORK}/source/picoquic" -B "${MOQ_WORK}/old"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "old metadata control failed to configure: ${_out}${_err}")
endif()
file(READ "${MOQ_WORK}/old/observed.txt" _observed)
if(NOT _observed MATCHES "^0\\.0\\.0-")
    message(FATAL_ERROR "old metadata control did not expose version replacement")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "GIT_CEILING_DIRECTORIES=${MOQ_WORK}/source"
    "${_git}" apply
    "${MOQ_SOURCE}/patches/picoquic/0008-use-declared-project-version.patch"
    WORKING_DIRECTORY "${MOQ_WORK}/source/picoquic"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10)
if(NOT _rc STREQUAL "0" OR NOT _out STREQUAL "" OR NOT _err STREQUAL "")
    message(FATAL_ERROR "version patch did not apply cleanly: ${_out}${_err}")
endif()

file(WRITE "${MOQ_WORK}/source/CMakeLists.txt" [=[cmake_minimum_required(VERSION 3.13)
project(parent VERSION 9.8.7 LANGUAGES NONE)
add_subdirectory(picoquic)
]=])
foreach(_mode archive subproject)
    if(_mode STREQUAL "archive")
        set(_source "${MOQ_WORK}/source/picoquic")
        set(_result "${MOQ_WORK}/${_mode}")
    else()
        set(_source "${MOQ_WORK}/source")
        set(_result "${MOQ_WORK}/${_mode}/picoquic")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_source}"
        -B "${MOQ_WORK}/${_mode}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10)
    if(NOT _rc STREQUAL "0" OR NOT _err STREQUAL "" OR
       _out MATCHES "[Ww]arning:|[Ee]rror:|[Ff]atal:")
        message(FATAL_ERROR "${_mode} configure was not clean: ${_out}${_err}")
    endif()
    file(READ "${_result}/observed.txt" _observed)
    file(READ "${_result}/package-version.cmake" _package)
    if(NOT _observed STREQUAL "1.1.53.0" OR
       NOT _package MATCHES "set\\(PACKAGE_VERSION \"1\\.1\\.53\\.0\"\\)")
        message(FATAL_ERROR "${_mode} lost the declared package version")
    endif()
endforeach()
message(STATUS "Declared picoquic version retained for archive and subproject builds")
