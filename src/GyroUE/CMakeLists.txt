cmake_minimum_required(VERSION 3.24)
project(GyroUE VERSION 0.9.0 LANGUAGES C CXX)  # the one place the GyroUE version lives (DLL log line + release zip name)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one less DLL to ship

# GyroLib SDK (pinned in tools/gyrolib-sdk.sh), passed with -DCMAKE_PREFIX_PATH by tools/build-gyroue.sh
find_package(GyroLib 1.2 CONFIG REQUIRED COMPONENTS SDL)

# MinHook (BSD-2), pinned in third_party/: inline hooks for protected exes whose import table is a stub.
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/minhook-1.3.4")
add_library(minhook STATIC "${MINHOOK_DIR}/src/buffer.c" "${MINHOOK_DIR}/src/hook.c" "${MINHOOK_DIR}/src/trampoline.c"
            "${MINHOOK_DIR}/src/hde/hde64.c")
target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")

# Version resource (file properties: description, product name, version) from project() above.
configure_file(version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_library(GyroUE SHARED dllmain.cpp proxy.cpp host.cpp hooks.cpp overlay.cpp perf.cpp ue.cpp log.cpp notify.cpp deps.cpp xinput.cpp nativepad.cpp longpress.cpp proxy.def
            "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
set_target_properties(GyroUE PROPERTIES OUTPUT_NAME winmm)
target_compile_definitions(GyroUE PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE "GYROUE_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(GyroUE PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(GyroUE PRIVATE GyroLib::gyrolib_sdl minhook delayimp dxguid)
# gyrolib.dll lives in the GyroUE subfolder and is loaded by full path before first use.
target_link_options(GyroUE PRIVATE /DELAYLOAD:gyrolib.dll)
# Symbols for crash dumps (build/GyroUE/Release/winmm.pdb; never shipped). /OPT keeps the release layout.
target_compile_options(GyroUE PRIVATE /Zi)
target_link_options(GyroUE PRIVATE /DEBUG /OPT:REF /OPT:ICF)
