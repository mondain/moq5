add_library(moq-mpeg2ts
    src/mpeg2ts.c
)

add_library(moq::mpeg2ts ALIAS moq-mpeg2ts)

target_include_directories(moq-mpeg2ts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(moq-mpeg2ts PUBLIC moq::core)
target_compile_features(moq-mpeg2ts PUBLIC c_std_11)

set_target_properties(moq-mpeg2ts PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME moq-mpeg2ts
    EXPORT_NAME mpeg2ts
)

install(TARGETS moq-mpeg2ts
    EXPORT libmoqTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/moq
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")

target_compile_definitions(moq-mpeg2ts PRIVATE MOQ_BUILDING)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(moq-mpeg2ts PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

if(MOQ_BUILD_TESTS)
    add_executable(test_mpeg2ts tests/test_mpeg2ts.c)
    target_link_libraries(test_mpeg2ts PRIVATE moq::mpeg2ts)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_mpeg2ts PRIVATE -Wall -Wextra -Wpedantic
            $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME mpeg2ts COMMAND test_mpeg2ts)
    add_test(NAME mpeg2ts_symbols COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM} -DREADELF=${CMAKE_READELF}
        -DELF=$<STREQUAL:${CMAKE_EXECUTABLE_FORMAT},ELF>
        -DLIBRARY=$<TARGET_FILE:moq-mpeg2ts>
        -DSHARED=$<BOOL:${BUILD_SHARED_LIBS}>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.cmake)
    set_tests_properties(mpeg2ts_symbols PROPERTIES TIMEOUT 90 LABELS "media;mpeg2ts")
    if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
        add_test(NAME mpeg2ts_export_oracle COMMAND ${CMAKE_COMMAND}
            -DCC=${CMAKE_C_COMPILER} -DNM=${CMAKE_NM}
            -DREADELF=${CMAKE_READELF} -DAR=${CMAKE_AR}
            -DOBJCOPY=${CMAKE_OBJCOPY}
            -DCHECK_DIR=${CMAKE_CURRENT_BINARY_DIR}/symbol-fixtures
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols_oracle.cmake)
        set_tests_properties(mpeg2ts_export_oracle PROPERTIES TIMEOUT 90 LABELS "media;mpeg2ts")
    endif()

    add_executable(test_mpeg2ts_state tests/test_mpeg2ts_state.c)
    target_link_libraries(test_mpeg2ts_state PRIVATE moq::mpeg2ts)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_mpeg2ts_state PRIVATE -Wall -Wextra -Wpedantic
            $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME mpeg2ts_state COMMAND test_mpeg2ts_state)

    # The functions kept out of the public API, built from the library's
    # source because a shared build does not export them.
    add_executable(test_mpeg2ts_internal tests/test_mpeg2ts_internal.c
        src/mpeg2ts.c)
    target_compile_definitions(test_mpeg2ts_internal PRIVATE MOQ_BUILDING)
    target_include_directories(test_mpeg2ts_internal PRIVATE include)
    target_link_libraries(test_mpeg2ts_internal PRIVATE moq::core)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_mpeg2ts_internal PRIVATE -Wall -Wextra
            -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME mpeg2ts_internal COMMAND test_mpeg2ts_internal)

    # Concurrent first use in a fresh process (meaningful under TSan).
    find_package(Threads)
    if(Threads_FOUND AND NOT WIN32)
        add_executable(test_mpeg2ts_threads tests/test_mpeg2ts_threads.c)
        target_link_libraries(test_mpeg2ts_threads PRIVATE moq::mpeg2ts
            Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(test_mpeg2ts_threads PRIVATE -Wall -Wextra
                -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        add_test(NAME mpeg2ts_threads COMMAND test_mpeg2ts_threads)
    endif()
endif()
