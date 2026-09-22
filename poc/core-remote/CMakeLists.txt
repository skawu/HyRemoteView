cmake_minimum_required(VERSION 3.21)

# POC_ONLY. This project exists to answer G3-POC-1A (skawu/HyRemoteView#9) and is not the future
# product build.
#
# The client core (`rfb/`, `net/`, `harness/`) is deliberately dependency-free: it is built from the
# C++ standard library plus its own socket layer only, which is what makes it a HyRemoteView-owned,
# bounded, GPL-free client and keeps one source tree buildable on Windows and Linux. Qt appears only
# in the optional HyRemote runtime fixture, because the HyRemote runtime itself needs it.
project(PocCoreRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(POC_CORE_REMOTE_BUILD_TESTS "Build the POC_ONLY harness tests." ON)
set(POC_HYREMOTE_SOURCE_DIR "" CACHE PATH
    "Path to an unmodified HyRemote checkout; enables the runtime-backed fixture.")

add_library(poc-core-remote STATIC
    src/poc_boundary.cpp
    src/net/socket.cpp
    src/rfb/wire.cpp
    src/rfb/pixel.cpp
    src/rfb/des.cpp
    src/rfb/vnc_auth.cpp
    src/rfb/client.cpp
    src/harness/deterministic_content.cpp
    src/harness/scripted_server.cpp
)
target_include_directories(poc-core-remote PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(poc-core-remote PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(poc-core-remote PRIVATE /W4)
else()
    target_compile_options(poc-core-remote PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(poc-core-remote PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(poc-core-remote PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------------------------
# Tools: the probe (client side) and, when a HyRemote checkout is provided, the runtime fixture
# that drives HyRemote's own RFB transport with the same deterministic content.
# ---------------------------------------------------------------------------------------------

add_executable(rfb-probe tools/rfb_probe.cpp)
target_link_libraries(rfb-probe PRIVATE poc-core-remote)

if(POC_HYREMOTE_SOURCE_DIR)
    if(NOT EXISTS "${POC_HYREMOTE_SOURCE_DIR}/src/runtime/src/transport/rfb_transport.cpp")
        message(FATAL_ERROR
            "POC_HYREMOTE_SOURCE_DIR=${POC_HYREMOTE_SOURCE_DIR} does not look like a HyRemote checkout")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
    add_subdirectory(hyremote_fixture)
endif()

if(POC_CORE_REMOTE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
