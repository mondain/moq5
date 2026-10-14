file(MAKE_DIRECTORY "${CHECK_DIR}")
set(failures 0)
foreach(case good hidden extra)
    set(flags)
    if(case STREQUAL hidden)
        list(APPEND flags -DHIDE_WRITE)
    elseif(case STREQUAL extra)
        list(APPEND flags -DEXTRA_EXPORT)
    endif()
    set(library "${CHECK_DIR}/${case}.so")
    execute_process(COMMAND "${CC}" -shared -fPIC -fvisibility=hidden
        -Wall -Wextra -Wpedantic -Werror ${flags}
        "${SOURCE_DIR}/symbol_fixture.c" -o "${library}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT "${err}" STREQUAL "")
        message(FATAL_ERROR "fixture compile failed: ${out}${err}")
    endif()
    if(case STREQUAL hidden)
        # Retain a GLOBAL ordinary-symtab symbol which is absent from dynsym.
        # A dynamic export oracle must not adopt it from the ordinary table.
        execute_process(COMMAND objcopy --globalize-symbol=moq_cmaf_packager_write
            "${library}" RESULT_VARIABLE rc ERROR_VARIABLE err)
        if(NOT rc EQUAL 0 OR NOT "${err}" STREQUAL "")
            message(FATAL_ERROR "objcopy fixture failed: ${err}")
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DNM=${NM}"
        "-DLIBRARY=${library}" -DSHARED=ON
        -P "${SOURCE_DIR}/check_symbols.cmake"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(case STREQUAL good)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "valid export set refused: ${out}${err}")
        endif()
    elseif(rc EQUAL 0)
        message(STATUS "FAIL: ${case} export mutant escaped the oracle")
        math(EXPR failures "${failures} + 1")
    elseif(NOT "${out}${err}" MATCHES "packager export set mismatch")
        message(FATAL_ERROR "${case} failed for the wrong reason: ${out}${err}")
    endif()
    message(STATUS "export oracle ${case}: expected result")
endforeach()
if(failures)
    message(FATAL_ERROR "export oracle failures=${failures}")
endif()
