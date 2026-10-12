cmake_minimum_required(VERSION 3.24)
project(gdvr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no runtime DLLs to ship

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Grim Dawn is 64-bit: configure with -A x64")
endif()

include(FetchContent)
# SOURCE_SUBDIR points at a folder without a CMakeLists so only the sources are fetched;
# we compile them ourselves below.
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3
  SOURCE_SUBDIR  _none)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  SOURCE_SUBDIR  _none)
FetchContent_MakeAvailable(minhook imgui)

# OpenXR loader, built as a static library with the same static CRT as the mod
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG        release-1.1.60)
FetchContent_MakeAvailable(openxr)
# The SDK forces the DLL CRT onto its static loader; match the mod's static CRT instead.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# The mod itself, injected into Grim Dawn.exe
add_library(gdvr SHARED
  src/gdvr/dllmain.cpp
  src/gdvr/engine_hooks.cpp
  src/gdvr/overlay.cpp
  src/gdvr/log.cpp
  src/gdvr/mask.cpp
  src/gdvr/perf.cpp
  src/gdvr/xr.cpp)
target_link_libraries(gdvr PRIVATE minhook imgui openxr_loader d3d11 dxgi d3dcompiler)
target_compile_definitions(gdvr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

set(GDVR_GAME_DIR "G:/Games/Steam/steamapps/common/Grim Dawn" CACHE PATH "Grim Dawn install folder")

add_library(gdvr_common STATIC src/common/inject.cpp)
target_compile_definitions(gdvr_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Injector / hot-reloader
add_executable(gdvr_inject src/injector/main.cpp)
target_link_libraries(gdvr_inject PRIVATE gdvr_common)

# One-click launcher for the Steam shortcut: backs up saves, starts the game, injects
add_executable(gdvr_launch src/launcher/main.cpp)
target_link_libraries(gdvr_launch PRIVATE gdvr_common shell32 ole32)
file(WRITE ${CMAKE_BINARY_DIR}/generated/gdvr_config.h.in "#pragma once\n#define GDVR_GAME_DIR L\"@GDVR_GAME_DIR@\"\n")
configure_file(${CMAKE_BINARY_DIR}/generated/gdvr_config.h.in ${CMAKE_BINARY_DIR}/generated/gdvr_config.h @ONLY)
target_include_directories(gdvr_launch PRIVATE ${CMAKE_BINARY_DIR}/generated)

# Put all outputs next to each other so the injector/launcher find the DLL
set_target_properties(gdvr gdvr_inject gdvr_launch PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin)
