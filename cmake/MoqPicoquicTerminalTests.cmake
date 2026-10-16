include_guard(GLOBAL)
include(CheckCSourceCompiles)
include(CMakePushCheckState)

# Use the selected provider header, not adapter ordering or a cached result
# from a previously selected provider.
function(moq_add_picoquic_terminal_tests name adapter)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${MOQ_PICOQUIC_SOURCE_DIR}/picoquic")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    unset(_moq_terminal_contract CACHE)
    unset(_moq_terminal_contract)
    check_c_source_compiles([=[
#include <picoquic.h>
#if !defined(PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION) || PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION < 1
#error terminal_contract_not_available
#endif
int main(void) { return 0; }
]=] _moq_terminal_contract)
    cmake_pop_check_state()

    set(_tests "${PROJECT_SOURCE_DIR}/adapters/picoquic/tests")
    if(_moq_terminal_contract)
        set(_source "${_tests}/test_pq_terminal_adapter.c")
        set(_name "${name}_terminal")
    else()
        set(_source "${_tests}/test_pq_terminal_legacy.c")
        set(_name "${name}_terminal_legacy")
    endif()
    add_executable(test_${_name} "${_source}")
    target_link_libraries(test_${_name} PRIVATE ${adapter} picoquic-test ${ARGN})
    target_include_directories(test_${_name} PRIVATE
        "${PROJECT_SOURCE_DIR}/adapters/picoquic")
    if(name STREQUAL "pico_wt")
        target_sources(test_${_name} PRIVATE
            "${PROJECT_SOURCE_DIR}/adapters/pico_wt/tests/pico_wt_harness.c")
        target_include_directories(test_${_name} PRIVATE
            "${PROJECT_SOURCE_DIR}/adapters/pico_wt"
            "${PROJECT_SOURCE_DIR}/adapters/pico_wt/tests")
        target_compile_definitions(test_${_name} PRIVATE MOQ_TERMINAL_WT=1)
    endif()
    set(_targets test_${_name})
    add_test(NAME ${_name} COMMAND test_${_name})
    set_tests_properties(${_name} PROPERTIES TIMEOUT 120)

    if(_moq_terminal_contract AND NOT TARGET test_pq_terminal_provider)
        add_executable(test_pq_terminal_provider "${_tests}/test_pq_terminal.c")
        target_link_libraries(test_pq_terminal_provider PRIVATE picoquic-test)
        list(APPEND _targets test_pq_terminal_provider)
        add_test(NAME pq_terminal_provider COMMAND test_pq_terminal_provider)
        set_tests_properties(pq_terminal_provider PROPERTIES TIMEOUT 120)
    endif()
    foreach(_target IN LISTS _targets)
        target_compile_definitions(${_target} PRIVATE
            PICOQUIC_SOURCE_DIR="${MOQ_PICOQUIC_SOURCE_DIR}")
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(${_target} PRIVATE
                -Wall -Wextra -Wpedantic
                $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
    endforeach()
endfunction()
