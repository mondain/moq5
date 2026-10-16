add_executable(test_simple_relay_lifecycle test_lifecycle.c)
target_link_libraries(test_simple_relay_lifecycle PRIVATE moq::relay)
target_include_directories(test_simple_relay_lifecycle PRIVATE
    ${PROJECT_SOURCE_DIR}/adapters/msquic/include)
target_include_directories(test_simple_relay_lifecycle SYSTEM PRIVATE
    $<TARGET_PROPERTY:msquic::msquic,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_features(test_simple_relay_lifecycle PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_simple_relay_lifecycle PRIVATE -Wall -Wextra -Wpedantic
        $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
add_test(NAME simple_relay_lifecycle COMMAND test_simple_relay_lifecycle)
set_tests_properties(simple_relay_lifecycle PROPERTIES LABELS "relay;sansio" TIMEOUT 20)
add_test(NAME simple_relay_help COMMAND moq_simple_relay --help)
set_tests_properties(simple_relay_help PROPERTIES LABELS "relay;sansio"
    PASS_REGULAR_EXPRESSION "drafts 22, 18 and 16")

add_executable(simple_relay_peers peers.c)
target_link_libraries(simple_relay_peers PRIVATE moq::adapter-msquic-managed)
target_compile_features(simple_relay_peers PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(simple_relay_peers PRIVATE -Wall -Wextra -Wpedantic
        $<$<BOOL:${MOQ_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME simple_relay_loopback COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/loopback.py $<TARGET_FILE:moq_simple_relay>
    $<TARGET_FILE:simple_relay_peers>
    ${PROJECT_SOURCE_DIR}/adapters/msquic/tests/test_only_loopback_cert.pem
    ${PROJECT_SOURCE_DIR}/adapters/msquic/tests/test_only_loopback_key.pem)
set_tests_properties(simple_relay_loopback PROPERTIES LABELS "relay;loopback"
    TIMEOUT 210 RUN_SERIAL TRUE)
# The same campaign on PORT 0 while IPv4-only sockets hold much of the
# ephemeral range (tests/support/port_shadow.c): the peers dial 127.0.0.1.
if(TARGET moq_port_shadow)
    add_test(NAME simple_relay_loopback_port_shadow
        COMMAND $<TARGET_FILE:moq_port_shadow> -- ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/loopback.py $<TARGET_FILE:moq_simple_relay>
            $<TARGET_FILE:simple_relay_peers>
            ${PROJECT_SOURCE_DIR}/adapters/msquic/tests/test_only_loopback_cert.pem
            ${PROJECT_SOURCE_DIR}/adapters/msquic/tests/test_only_loopback_key.pem)
    # Every port-shadow test holds thousands of IPv4 ports for its run; two at
    # once can exhaust the ephemeral range, so they share one resource lock.
    set_tests_properties(simple_relay_loopback_port_shadow PROPERTIES
        LABELS "relay;loopback" TIMEOUT 210 RUN_SERIAL TRUE
        RESOURCE_LOCK ipv4_port_shadow)
endif()
