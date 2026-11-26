cmake_minimum_required(VERSION 3.16)
project(dm_linux_x64_spike LANGUAGES CXX)

# Phase 0 spike for the host on Linux on x86_64 (Steam Deck): a guest arm64 function running in dynarmic on an x86_64 host, with an `svc #0` the x86_64 host answers.
# dynarmic is fetched at configure time at a pinned commit; Boost (headers only: boost/icl, boost/variant) must come from the system (Debian: libboost-dev) or -DBoost_INCLUDE_DIR=<headers>.
# See README.md in this directory and docs/plan_linux_x64.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# dynarmic's vendored robin-map declares cmake_minimum_required(VERSION < 3.5); CMake 4 removed the compatibility shim unless this is set.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "only the 64-bit frontend: the games are arm64" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(dynarmic
    URL https://codeload.github.com/lioncash/dynarmic/tar.gz/a41c380246d3d9f9874f0f792d234dc0cc17c180
    URL_HASH SHA256=cc0dfec19b7cfb80a649bd994bd0569c0482de1f49448773d2899f30b6a180c3)
FetchContent_MakeAvailable(dynarmic)

add_executable(dm_linux_x64_spike main.cpp)
target_link_libraries(dm_linux_x64_spike PRIVATE dynarmic)
