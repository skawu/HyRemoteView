# POC_ONLY HyRemote runtime fixture.
#
# Compiles HyRemote's *unmodified* RFB transport together with the core frame/storage code it needs,
# and drives it with the same deterministic content the client asserts. HyRemote is consumed as an
# external source tree: nothing under POC_HYREMOTE_SOURCE_DIR is modified, and the fixture is built
# by the POC's own CMake project rather than by HyRemote's build system.
#
# The generated export header is the one HyRemote's own build produces with an empty macro: the
# fixture links the transport statically, so no DLL export annotation applies. It is a generated-file
# shim, not a patched HyRemote header.

option(POC_HYREMOTE_WITH_SECURITY
       "Compile HyRemote's VNC authentication, mirroring HYREMOTE_WITH_TRANSPORT_SECURITY." OFF)

set(POC_FIXTURE_SOURCES
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/capabilities.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/detail/mailbox.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/frame.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/input.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/session.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/storage.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src/types.cpp"
    "${POC_HYREMOTE_SOURCE_DIR}/src/runtime/src/transport/rfb_transport.cpp"
    hyremote_runtime_fixture_main.cpp
)

if(POC_HYREMOTE_WITH_SECURITY)
    list(APPEND POC_FIXTURE_SOURCES
        "${POC_HYREMOTE_SOURCE_DIR}/src/runtime/src/transport/vnc_auth.cpp")
endif()

add_executable(hyremote-runtime-fixture ${POC_FIXTURE_SOURCES})

target_include_directories(hyremote-runtime-fixture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/include"
    # HyRemote's own core target adds this private include directory; the runtime transport sources
    # rely on it, so the fixture has to mirror it to compile them unmodified.
    "${POC_HYREMOTE_SOURCE_DIR}/src/core/src"
    "${POC_HYREMOTE_SOURCE_DIR}/src/runtime/src"
)

target_compile_features(hyremote-runtime-fixture PRIVATE cxx_std_17)
target_compile_definitions(hyremote-runtime-fixture PRIVATE HYREMOTE_HAS_RFB_TRANSPORT=1)

target_link_libraries(hyremote-runtime-fixture PRIVATE
    poc-core-remote
    Qt6::Core
    Qt6::Network
)

if(POC_HYREMOTE_WITH_SECURITY)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(hyremote-runtime-fixture PRIVATE HYREMOTE_HAS_TRANSPORT_SECURITY=1)
    target_link_libraries(hyremote-runtime-fixture PRIVATE OpenSSL::Crypto)
endif()

set_target_properties(hyremote-runtime-fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
