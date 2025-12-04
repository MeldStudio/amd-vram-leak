cmake_minimum_required(VERSION 3.20)
project(amd-vram-leak LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use new timestamp behavior for URL downloads
cmake_policy(SET CMP0135 NEW)

include(FetchContent)

# Fetch AMD ADLX SDK for GPU metrics
FetchContent_Declare(
    adlx
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/ADLX.git
    GIT_TAG V1.4
)

# Fetch fmt for text formatting and styling
FetchContent_Declare(
    fmt
    URL https://github.com/fmtlib/fmt/archive/e137db699a1b2db2d198fddd824b40945ab37056.tar.gz
    URL_HASH SHA256=0982b5879aaa3b863b07e6dfd01b7ef74e8c26edb54e8f74e7019c0e98037783
)

FetchContent_MakeAvailable(adlx fmt)

add_executable(repro
    main.cpp
    ${adlx_SOURCE_DIR}/SDK/ADLXHelper/Windows/Cpp/ADLXHelper.cpp
    ${adlx_SOURCE_DIR}/SDK/platform/Windows/WinAPIs.cpp
)

target_include_directories(repro PRIVATE
    ${adlx_SOURCE_DIR}
)

target_link_libraries(repro PRIVATE
    d3d11
    dxgi
    fmt::fmt
)
