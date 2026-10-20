cmake_minimum_required(VERSION 3.21)
project(hd2vr LANGUAGES C CXX)

# VR mod for the offline Helldivers 2 build (GameGuard era). See ../docs/.
# This is a trimmed, public build file: it builds only the VR / rendering / memory-tooling targets.
# The GameGuard-specific experiments (path-spoof hook, APC early-bird injector) and the third-party
# anti-cheat neutraliser are intentionally NOT part of this archive — see ../docs/03-gameguard-nprotect.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- MinHook (function hooking) ---
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3)
FetchContent_MakeAvailable(minhook)   # v1.3.3 has no root CMakeLists, so build it ourselves:
add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# --- Dear ImGui (the in-game overlay + its dx12/win32 backends) ---
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# --- OpenXR SDK (headset output). Static loader, so the mod DLL needs no extra DLL. ---
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG        release-1.1.63)
FetchContent_MakeAvailable(openxr)

# --- the mod DLL (injected into the offline game): D3D12 hooks, ImGui overlay, VR hot-reload host,
#     and the render-camera hook (embedded) ---
add_library(hd2vr_mod SHARED
  mod/dllmain.cpp
  mod/d3d12_hook.cpp
  mod/file_redirect.cpp
  mod/vr_host.cpp
  mod/cam_hook.cpp)
target_compile_definitions(hd2vr_mod PRIVATE HD2VR_CAM_EMBEDDED)
target_link_libraries(hd2vr_mod PRIVATE minhook imgui d3d12 dxgi)
set_target_properties(hd2vr_mod PROPERTIES OUTPUT_NAME "hd2vr_mod")

# --- the VR module (OpenXR stereo + head tracking), hot-reloaded by the host when rebuilt ---
add_library(hd2vr_vr SHARED mod/vr_session.cpp)
target_link_libraries(hd2vr_vr PRIVATE d3d12 dxgi d3dcompiler openxr_loader psapi)
set_target_properties(hd2vr_vr PROPERTIES OUTPUT_NAME "hd2vr_vr")

# --- standalone render-camera hook (for tests; the same code is embedded in hd2vr_mod) ---
add_library(hd2vr_cam SHARED mod/cam_hook.cpp)
target_link_libraries(hd2vr_cam PRIVATE minhook)
set_target_properties(hd2vr_cam PROPERTIES OUTPUT_NAME "hd2vr_cam")

# --- injectors / scanner ---
add_executable(hd2vr_inject injector/inject.cpp)        # LoadLibrary injector (post kill-flag)
add_executable(hd2vr_watch  injector/watch_inject.cpp)  # early injector (suspend-at-creation)
add_executable(hd2vr_scan   injector/scan_mem.cpp)      # read-only memory scanner (diagnostics)

# --- in-process tools ---
add_library(hd2vr_rename SHARED mod/rename_label.cpp)             # in-memory label rename
add_library(hd2vr_lua    SHARED mod/lua_probe.cpp)               # runs Lua inside the game
target_link_libraries(hd2vr_lua PRIVATE minhook)
add_library(hd2vr_cheat  SHARED mod/cheat_scan.cpp)              # in-game memory search/patch
add_library(hd2vr_hwbp   SHARED mod/hwbp.cpp)                    # hardware-watchpoint RE tool
add_library(hd2vr_inputprobe SHARED mod/input_probe.cpp)        # which input API the game reads
target_link_libraries(hd2vr_inputprobe PRIVATE minhook)
add_library(hd2vr_inputtest  SHARED mod/input_test.cpp)         # in-process mouse-move test
