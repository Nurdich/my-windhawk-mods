cmake_minimum_required(VERSION 3.20)
project(BetterFileSizes VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# MinHook library (download and build)
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageworking/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# Hook DLL
add_library(BetterFileSizesHook SHARED
    hook_dll/main.cpp
    hook_dll/config.cpp
    hook_dll/symbol_resolver.cpp
)

target_include_directories(BetterFileSizesHook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${minhook_SOURCE_DIR}/include
)

target_link_libraries(BetterFileSizesHook PRIVATE
    minhook
    ole32
    oleaut32
    propsys
    shell32
    shlwapi
    dbghelp
)

# Injector executable
add_executable(BetterFileSizesInjector
    injector/main.cpp
)

target_link_libraries(BetterFileSizesInjector PRIVATE
    shlwapi
)

# Set output directories
set_target_properties(BetterFileSizesHook BetterFileSizesInjector
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
