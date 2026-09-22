# POC_ONLY tests. Plain executables registered with CTest, so no test framework dependency is
# introduced by the verification harness itself.
function(poc_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE poc-core-remote)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

# No server needed.
poc_add_test(poc-core-remote-test-boundary test_boundary.cpp)
poc_add_test(poc-core-remote-test-primitives test_rfb_primitives.cpp)

# Against the POC_ONLY scripted double.
poc_add_test(poc-core-remote-test-handshake test_handshake.cpp)
poc_add_test(poc-core-remote-test-framebuffer test_framebuffer.cpp)
poc_add_test(poc-core-remote-test-input-semantics test_input_semantics.cpp)
poc_add_test(poc-core-remote-test-negative-bounds test_negative_bounds.cpp)
poc_add_test(poc-core-remote-test-connection-lifecycle test_connection_lifecycle.cpp)
