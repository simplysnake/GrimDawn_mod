cmake_minimum_required(VERSION 3.24)
project(gdvr VERSION 0.3.2 LANGUAGES C CXX)

# Development builds (default) log into the repo and keep the diagnostics; release builds (package.ps1)
# use only the install folder and %LOCALAPPDATA%\gdvr.
option(GDVR_DEV "Development build" ON)

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
  src/gdvr/d3dtrace.cpp
  src/gdvr/devotion.cpp
  src/gdvr/dllmain.cpp
  src/gdvr/engine_hooks.cpp
  src/gdvr/overlay.cpp
  src/gdvr/paths.cpp
  src/gdvr/input.cpp
  src/gdvr/labels.cpp
  src/gdvr/log.cpp
  src/gdvr/mask.cpp
  src/gdvr/mirror.cpp
  src/gdvr/numbers.cpp
  src/gdvr/perf.cpp
  src/gdvr/sky.cpp
  src/gdvr/pointer.cpp
  src/gdvr/xr.cpp)
target_link_libraries(gdvr PRIVATE minhook imgui openxr_loader d3d11 dxgi d3dcompiler dinput8 dxguid windowscodecs ole32 dbghelp)
target_compile_definitions(gdvr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE GDVR_VERSION="${PROJECT_VERSION}")
if(GDVR_DEV)
  # Development builds also log into the repo (logs/), readable from tools that can't see %LOCALAPPDATA%.
  target_compile_definitions(gdvr PRIVATE GDVR_DEV_LOG_DIR=L"${CMAKE_SOURCE_DIR}/logs")
endif()

set(GDVR_GAME_DIR "" CACHE PATH "Grim Dawn install folder (development fallback; the launcher looks in Steam first)")

add_library(gdvr_common STATIC src/common/inject.cpp)
target_compile_definitions(gdvr_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Injector / hot-reloader
add_executable(gdvr_inject src/injector/main.cpp)
target_link_libraries(gdvr_inject PRIVATE gdvr_common)

# Launcher for the Steam shortcut: installs the dinput8.dll stand-in, backs up saves, swaps in the VR
# options, starts SteamVR and the game (no console window, no injection)
add_executable(gdvr_launch WIN32 src/launcher/main.cpp src/launcher/steam_shortcut.cpp src/launcher/update.cpp)
target_link_libraries(gdvr_launch PRIVATE shell32 ole32 winhttp bcrypt)
target_compile_definitions(gdvr_launch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE GDVR_VERSION_W=L"${PROJECT_VERSION}")
if(GDVR_DEV)
  file(WRITE ${CMAKE_BINARY_DIR}/generated/gdvr_config.h.in "#pragma once\n#define GDVR_GAME_DIR L\"@GDVR_GAME_DIR@\"\n")
  configure_file(${CMAKE_BINARY_DIR}/generated/gdvr_config.h.in ${CMAKE_BINARY_DIR}/generated/gdvr_config.h @ONLY)
  target_include_directories(gdvr_launch PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_compile_definitions(gdvr_launch PRIVATE GDVR_DEV)
endif()

# dinput8.dll stand-in for the game's x64 folder: loads the mod only when the launcher starts the game
add_library(gdvr_proxy SHARED src/proxy/dinput8.cpp src/proxy/dinput8.def)
set_target_properties(gdvr_proxy PROPERTIES OUTPUT_NAME dinput8)
target_compile_definitions(gdvr_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE GDVR_VERSION="${PROJECT_VERSION}")

# Diagnostic: logs every key press on the PC and where it came from (listen-only Raw Input)
add_executable(gdvr_keywatch src/keywatch/main.cpp)
target_compile_definitions(gdvr_keywatch PRIVATE UNICODE _UNICODE)

# Put all outputs next to each other so the injector/launcher find the DLL
# (development builds: bin/ in the repo, where the Steam shortcut points; release builds: their build folder)
if(GDVR_DEV)
  set(GDVR_OUT ${CMAKE_SOURCE_DIR}/bin)
else()
  set(GDVR_OUT ${CMAKE_BINARY_DIR}/out)
endif()
set_target_properties(gdvr gdvr_inject gdvr_launch gdvr_keywatch gdvr_proxy PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${GDVR_OUT}
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${GDVR_OUT}
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${GDVR_OUT}
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${GDVR_OUT})
