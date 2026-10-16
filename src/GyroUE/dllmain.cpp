// GyroUE entry point. Loaded as winmm.dll (or version.dll, see proxy.cpp) by the game exe (static import, so DllMain runs on the
// main thread, which in Unreal is the game thread). It patches the game modules' imports of PeekMessageW:
// Unreal pumps Windows messages on the game thread once per frame, and the call that finds the
// queue empty marks the end of that pump, which is where the gyro frame runs.
#include "hooks.hpp"
#include "host.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "overlay.hpp"

#include <windows.h>

#include <cwctype>
#include <string>

namespace {

using PeekMessageW_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

PeekMessageW_t g_real_peek = nullptr;
DWORD g_game_thread = 0;
bool g_in_frame = false;
LARGE_INTEGER g_freq{}, g_last_frame{};

BOOL WINAPI hooked_peek(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    const BOOL got = g_real_peek(msg, hwnd, min, max, remove);
    if (got && msg && msg->message == WM_INPUT && GetCurrentThreadId() == g_game_thread)
        gyroue::host_raw_input(reinterpret_cast<void*>(msg->lParam));
    if (!got && !g_in_frame && GetCurrentThreadId() == g_game_thread) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        // At most one gyro frame per 0.5 ms, in case the game pumps several times per frame.
        if (now.QuadPart - g_last_frame.QuadPart >= g_freq.QuadPart / 2000) {
            g_last_frame = now;
            g_in_frame = true;
            const int64_t start = gyroue::perf::now();
            const bool active = gyroue::host_frame();
            gyroue::perf::record(active ? gyroue::perf::Active : gyroue::perf::Idle, start);
            g_in_frame = false;
        }
    }
    return got;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

void attach(HMODULE self) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(self, buffer, MAX_PATH);
    std::wstring dir = buffer;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring exe = buffer;
    std::wstring stem = exe.substr(exe.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.find_last_of(L'.'));

    // Only one process per install runs GyroUE: Windows loads the proxy into every exe started from this
    // folder (the game, but also a config tool or benchmark shipped next to it), and two instances would
    // fight over the pad and GyroUE.log. The first process to start owns a session mutex named after the
    // install folder (hashed: mutex names can't hold '\' and are limited to MAX_PATH); later ones stay a
    // plain system DLL proxy. The handle stays open until the process exits, which releases the mutex.
    const std::wstring mod_dir = dir + L"\\GyroUE";
    const std::wstring mutex_name = L"Local\\GyroUE-" + std::to_wstring(std::hash<std::wstring>{}(lower(dir)));
    HANDLE instance = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (!instance) return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance);
        return;
    }

    gyroue::log_open(mod_dir + L"\\GyroUE.log");
    gyroue::log("GyroUE " GYROUE_VERSION " in %s", gyroue::narrow(exe).c_str());

    QueryPerformanceFrequency(&g_freq);
    g_game_thread = GetCurrentThreadId();
    gyroue::hooks::init(stem.c_str());
    gyroue::host_configure(mod_dir, stem);
    gyroue::overlay::install(mod_dir + L"\\gyrolib.ini");
    // Modular builds import PeekMessageW from ApplicationCore/Core instead of the exe; the hook
    // covers every game module, including ones loaded later.
    if (!gyroue::hooks::add("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&hooked_peek),
                            reinterpret_cast<void**>(&g_real_peek)))
        gyroue::log("hook: PeekMessageW not imported by any loaded game module yet");
    gyroue::log("hook: PeekMessageW hook registered, game thread %lu", g_game_thread);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        attach(instance);
    }
    return TRUE;
}
