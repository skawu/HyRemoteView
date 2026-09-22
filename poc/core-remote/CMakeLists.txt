cmake_minimum_required(VERSION 3.21)

# POC_ONLY. This project exists to answer G3-POC-1A (skawu/HyRemoteView#9) and is not the future
# product build. The `poc-core-remote` target deliberately stays dependency-free: the client core is
# built from the C++ standard library plus its own socket layer only.
project(PocCoreRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(POC_CORE_REMOTE_BUILD_TESTS "Build the POC_ONLY harness tests." ON)
set(POC_HYREMOTE_SOURCE_DIR "" CACHE PATH
    "Path to an unmodified HyRemote checkout; enables the runtime-backed fixture.")

add_library(poc-core-remote STATIC
    src/poc_boundary.cpp
)
target_include_directories(poc-core-remote PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(poc-core-remote PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(poc-core-remote PRIVATE /W4)
else()
    target_compile_options(poc-core-remote PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(POC_CORE_REMOTE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
