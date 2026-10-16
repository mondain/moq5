add_executable(moq-interop-client main.c object_relay.c endpoint_error.c
               sub_answers.c)
# The client drives the sans-I/O core via the service-tier endpoint, which owns
# transport selection (raw QUIC + WebTransport) and version negotiation. Linking
# moq::service transitively provides moq::core (session) plus the picoquic and
# pico_wt adapters for both transports.
target_link_libraries(moq-interop-client PRIVATE moq::service)

# Owned tool: warning baseline (PRIVATE; -Werror gated by the option).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(moq-interop-client PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# The object-relay verifier is pure (no I/O, no library), so its unit test runs
# hermetically and shows that each deviation the live case must catch is
# rejected. The live case needs a separately launched relay; see
# scripts/run_object_relay.sh.
if(MOQ_BUILD_TESTS)
    add_executable(test_object_relay_verify tests/test_object_relay_verify.c
                   object_relay.c)
    target_include_directories(test_object_relay_verify PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(test_object_relay_verify PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_object_relay_verify PRIVATE
            -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME interop_object_relay_verify COMMAND test_object_relay_verify)
    set_tests_properties(interop_object_relay_verify PROPERTIES
        LABELS "interop;unit" TIMEOUT 30)

    # The endpoint-failure text (result name, code and the unsupported-draft
    # hint); links only the core for moq_strerror.
    add_executable(test_interop_endpoint_error tests/test_endpoint_error.c
                   endpoint_error.c)
    target_include_directories(test_interop_endpoint_error PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_interop_endpoint_error PRIVATE moq::core)
    target_compile_features(test_interop_endpoint_error PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(test_interop_endpoint_error PRIVATE
            -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_test(NAME interop_endpoint_error COMMAND test_interop_endpoint_error)
    set_tests_properties(interop_endpoint_error PROPERTIES
        LABELS "interop;unit" TIMEOUT 30)

    # Forwarded SUBSCRIBE answers (the test track accepted, every other
    # request refused exactly once) over a SimPair in every draft.
    if(TARGET moq::sim)
        add_executable(test_interop_sub_answers tests/test_sub_answers.c
                       sub_answers.c)
        target_include_directories(test_interop_sub_answers PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_interop_sub_answers PRIVATE moq::sim moq::core)
        target_compile_features(test_interop_sub_answers PRIVATE c_std_11)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(test_interop_sub_answers PRIVATE
                -Wall -Wextra -Wpedantic $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
        endif()
        add_test(NAME interop_sub_answers COMMAND test_interop_sub_answers)
        set_tests_properties(interop_sub_answers PROPERTIES
            LABELS "interop;unit" TIMEOUT 30)
    endif()
endif()
