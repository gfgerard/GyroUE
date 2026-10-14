#include "xinput.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "nativepad.hpp"
#include "notify.hpp"

#include <windows.h>
#include <psapi.h>
#include <Xinput.h>

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.h>
#include <gyrolib/runtime.h>
#include <gyrolib/sdl.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gyroue::xinput {
namespace {

using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetState_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCaps_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using Enable_t = void(WINAPI*)(BOOL);
using RegisterRawInputDevices_t = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);

Settings g_settings;

// Originals, filled by hooks::add (regular imports) or resolved on first use (delay-load imports of a DLL that
// wasn't loaded yet when the hook was registered in DllMain).
GetState_t g_get_state = nullptr;
GetState_t g_get_state_ex = nullptr;  // XInputGetStateEx: ordinal 100 only (adds the Guide button, 0x0400)
SetState_t g_set_state = nullptr;
GetCaps_t g_get_caps = nullptr;
Enable_t g_enable = nullptr;
RegisterRawInputDevices_t g_register_raw = nullptr;
GetRawInputData_t g_get_raw_data = nullptr;
DirectInput8Create_t g_di8_create = nullptr;

// The original of an import: from the hook, else from the DLL the game imports it from (loaded on demand; never
// called from DllMain: the game only calls these once it runs).
template <typename F>
F original(F& slot, const char* function, const char* fallback_dll, WORD ordinal = 0) {
    if (F f = reinterpret_cast<F>(InterlockedCompareExchangePointer(reinterpret_cast<void**>(&slot), nullptr, nullptr)))
        return f;
    std::string dll = hooks::source_dll(function);
    if (dll.empty()) dll = fallback_dll;
    HMODULE m = LoadLibraryA(dll.c_str());
    F f = m ? reinterpret_cast<F>(GetProcAddress(m, ordinal ? MAKEINTRESOURCEA(ordinal) : function)) : nullptr;
    if (f) InterlockedExchangePointer(reinterpret_cast<void**>(&slot), reinterpret_cast<void*>(f));
    return f;
}

// ------------------------------------------------------------------ state shown to the game

std::mutex g_mutex;          // guards g_exposed, g_slot, g_state
bool g_exposed = false;      // the pad is shown to the game as XInput controller g_slot
DWORD g_slot = 0;
XINPUT_STATE g_state{};      // last state built on the game thread (neutral while the GyroLib panel is open); includes
                             // the Guide bit, which only XInputGetStateEx reports
std::atomic<bool> g_enabled{true};            // XInputEnable(FALSE) from the game: neutral state, no rumble
std::atomic<bool> g_neutral{false};           // GyroLib's panel has the controller: every slot reads idle
std::atomic<uint32_t> g_rumble{0};            // last XInputSetState for our slot: left (low) << 16 | right (high)
std::atomic<bool> g_game_polls{false};        // the game called XInputGetState at least once

// ------------------------------------------------------------------ native pad support (Auto mode)

// Two kinds of signs. A Sony pad library (libScePad, linked in or not; hid.dll among the imports means the same in
// these games) only reads PlayStation controllers: an Xbox-less DirectInput pad or a Switch Pro controller still needs
// the translation there. An own SDL, Raw Input for game controllers or DirectInput read any controller.
enum Sign { Sony = 0, Any = 1 };
std::mutex g_native_mutex;
std::string g_native_reason[2];               // first sign of each kind ("" = none)
std::atomic<bool> g_native[2] = {false, false};

void native_sign(const std::string& reason, Sign kind = Any) {
    std::lock_guard lock(g_native_mutex);
    if (!g_native_reason[kind].empty()) return;
    g_native_reason[kind] = reason;
    g_native[kind] = true;
    log("xinput: the game reads %s itself: %s", kind == Sony ? "PlayStation controllers" : "controllers", reason.c_str());
}

std::string native_reason(Sign kind) {
    std::lock_guard lock(g_native_mutex);
    return g_native_reason[kind];
}

// ------------------------------------------------------------------ hooks (any game thread)

constexpr WORD kGuide = 0x0400;

// Our slot's state, or false for the real XInput.
bool our_state(DWORD user, XINPUT_STATE* state, bool guide) {
    std::lock_guard lock(g_mutex);
    if (!g_exposed || user != g_slot || !state) return false;
    *state = g_state;
    if (!guide) state->Gamepad.wButtons &= ~kGuide;
    if (!g_enabled) std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return true;
}

// A real XInput controller (Xbox pad, Steam Input, a remapper) also reads idle while GyroLib's panel has the
// controller: GyroLib may be reading that same pad through SDL. Still connected, so the game doesn't pause.
DWORD idle_while_panel(DWORD result, XINPUT_STATE* state) {
    if (result == ERROR_SUCCESS && state && g_neutral.load(std::memory_order_relaxed))
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return result;
}

DWORD WINAPI hk_get_state(DWORD user, XINPUT_STATE* state) {
    if (!g_game_polls.exchange(true)) log("xinput: the game polls XInputGetState");
    if (our_state(user, state, false)) return ERROR_SUCCESS;
    GetState_t real = original(g_get_state, "XInputGetState", "xinput1_4.dll");
    return idle_while_panel(real ? real(user, state) : ERROR_DEVICE_NOT_CONNECTED, state);
}

DWORD WINAPI hk_get_state_ex(DWORD user, XINPUT_STATE* state) {
    if (!g_game_polls.exchange(true)) log("xinput: the game polls XInputGetStateEx");
    if (our_state(user, state, true)) return ERROR_SUCCESS;
    GetState_t real = original(g_get_state_ex, "XInputGetStateEx", "xinput1_4.dll", 100);
    return idle_while_panel(real ? real(user, state) : ERROR_DEVICE_NOT_CONNECTED, state);
}

DWORD WINAPI hk_set_state(DWORD user, XINPUT_VIBRATION* vibration) {
    {
        std::lock_guard lock(g_mutex);
        if (g_exposed && user == g_slot) {
            if (!vibration) return ERROR_BAD_ARGUMENTS;
            g_rumble = uint32_t(vibration->wLeftMotorSpeed) << 16 | vibration->wRightMotorSpeed;
            return ERROR_SUCCESS;
        }
    }
    SetState_t real = original(g_set_state, "XInputSetState", "xinput1_4.dll");
    return real ? real(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI hk_get_caps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    {
        std::lock_guard lock(g_mutex);
        if (g_exposed && user == g_slot) {
            if (!caps) return ERROR_BAD_ARGUMENTS;
            *caps = {};
            caps->Type = XINPUT_DEVTYPE_GAMEPAD;
            caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Gamepad.wButtons = 0xF3FF;  // every button XInputGetState reports
            caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
            caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = SHORT(-64);  // 0xFFC0
            caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFFFF;
            return ERROR_SUCCESS;
        }
    }
    GetCaps_t real = original(g_get_caps, "XInputGetCapabilities", "xinput1_4.dll");
    return real ? real(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI hk_enable(BOOL enable) {
    g_enabled = enable != FALSE;
    if (Enable_t real = original(g_enable, "XInputEnable", "xinput1_4.dll")) real(enable);
}

// Raw Input for joysticks (usage 4), gamepads (5) or multi-axis controllers (8): the game reads HID pads itself
// (UE's RawInput plugin does this). Mouse/keyboard registrations are ordinary. XInput=2 drops these registrations so
// the game only gets the Xbox controller.
BOOL WINAPI hk_register_raw(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    RegisterRawInputDevices_t real = original(g_register_raw, "RegisterRawInputDevices", "user32.dll");
    if (!real) return FALSE;
    if (!devices || size < sizeof(RAWINPUTDEVICE)) return real(devices, count, size);
    auto pad = [](const RAWINPUTDEVICE& d) {
        return d.usUsagePage == 1 && (d.usUsage == 4 || d.usUsage == 5 || d.usUsage == 8) && !(d.dwFlags & RIDEV_REMOVE);
    };
    std::vector<RAWINPUTDEVICE> kept;
    for (UINT i = 0; i < count; ++i) {
        log("input: the game registers Raw Input usage page 0x%X usage 0x%X flags 0x%lX", unsigned(devices[i].usUsagePage),
            unsigned(devices[i].usUsage), devices[i].dwFlags);
        if (!pad(devices[i])) {
            kept.push_back(devices[i]);
        } else if (g_settings.mode == Auto) {
            native_sign("it registers Raw Input for game controllers (usage page 1, usage " + std::to_string(devices[i].usUsage) + ")");
        } else {
            log("xinput: XInput=2: the game's Raw Input registration for game controllers (usage %u) dropped",
                unsigned(devices[i].usUsage));
        }
    }
    if (g_settings.mode != Always || kept.size() == count) return real(devices, count, size);
    return kept.empty() ? TRUE : real(kept.data(), UINT(kept.size()), sizeof(RAWINPUTDEVICE));
}

// Diagnostics: each HID device (not mouse/keyboard) the game receives Raw Input reports from, once.
UINT WINAPI hk_get_raw_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    GetRawInputData_t real = original(g_get_raw_data, "GetRawInputData", "user32.dll");
    const UINT result = real ? real(input, command, data, size, header_size) : UINT(-1);
    if (data && command == RID_INPUT && result != UINT(-1) && result >= sizeof(RAWINPUTHEADER) &&
        static_cast<const RAWINPUT*>(data)->header.dwType == RIM_TYPEHID) {
        const DWORD error = GetLastError();
        static std::mutex mutex;
        static HANDLE seen[8];
        static int count = 0;
        const HANDLE device = static_cast<const RAWINPUT*>(data)->header.hDevice;
        bool fresh = true;
        {
            std::lock_guard lock(mutex);
            for (int i = 0; i < count; ++i) fresh = fresh && seen[i] != device;
            if (fresh && count < 8) seen[count++] = device;
        }
        if (fresh) {
            wchar_t name[512] = {};
            UINT length = 512;
            GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &length);
            log("input: the game reads Raw Input reports from HID device %04X:%04X", nativepad::hid_path_id(name, L"vid"),
                nativepad::hid_path_id(name, L"pid"));
        }
        SetLastError(error);
    }
    return result;
}

HRESULT WINAPI hk_di8_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, void* outer) {
    native_sign("it creates DirectInput (DirectInput8Create)");
    DirectInput8Create_t real = original(g_di8_create, "DirectInput8Create", "dinput8.dll");
    return real ? real(instance, version, iid, out, outer) : E_FAIL;
}

// ------------------------------------------------------------------ watcher thread

// Polls the real XInput slots and the loaded modules once per second, off the game thread (XInputGetState on an
// empty slot can be slow, and a module scan is a few hundred calls).
std::atomic<uint32_t> g_real_mask{0};   // real XInput controllers connected (bit per slot)
std::atomic<bool> g_watch_ready{false}; // at least one full pass done
std::atomic<HMODULE> g_our_sdl{nullptr};

// Native pad libraries among the game modules' imports: a delay-loaded libScePad.dll (The Outer Worlds: Spacer's
// Choice) only loads on the game's first scePad call, possibly after the grace period, so the import itself counts.
// Same for hid.dll: Hogwarts Legacy opened the DualSense's HID device 15 s after the pad was shown as XInput.
void check_imports() {
    if (g_native[Sony] && g_native[Any]) return;
    for (const hooks::ImportedDll& dll : hooks::imported_dlls()) {
        std::string name = dll.name;
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        const char* how = dll.delay ? ", delay-load" : "";
        if (name.rfind("libscepad", 0) == 0) {
            native_sign("it imports Sony's pad library (" + dll.name + how + ", native DualSense support)", Sony);
        }
        if (name == "sdl2.dll" || name == "sdl3.dll") {
            native_sign("it imports its own SDL (" + dll.name + how + ")");
        }
        // Stock Unreal doesn't import hid.dll; the games that do link Sony's pad library (Hogwarts Legacy, Cronos,
        // Mortal Shell, Jedi: Fallen Order: the same HidD_* set as libScePad.dll). UE's RawInput plugin imports it too,
        // but it also registers Raw Input for game controllers, a sign for any controller.
        if (name == "hid.dll")
            native_sign("it imports the HID library (" + dll.name + how + ", Sony's pad library)", Sony);
    }
}

void scan_modules() {
    check_imports();  // modules loaded since the last pass
    if (g_native[Sony] && g_native[Any]) return;
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 2048);
    for (size_t i = 0; i < n; ++i) {
        wchar_t name[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        for (wchar_t* c = name; *c; ++c) *c = wchar_t(std::towlower(*c));
        const std::wstring n2 = name;
        if (n2.rfind(L"libscepad", 0) == 0) {
            native_sign("it loads Sony's pad library (" + narrow(n2) + ", native DualSense support)", Sony);
        }
        if ((n2 == L"sdl2.dll" || n2 == L"sdl3.dll") && mods[i] != g_our_sdl.load()) {
            native_sign("it loads its own SDL (" + narrow(n2) + ")");
        }
    }
}

// ------------------------------------------------------------------ XInputTrace (diagnostics)

// Every XInput DLL in the process gets inline hooks on XInputGetState (and XInputGetStateEx where it exists), so calls
// that don't go through a game module's import table (another module, GetProcAddress) show up too. Each calling module
// is logged once, with the first call's return address. GyroUE's own calls (the watcher) and GyroLib's are marked.
const char* const kTraceDlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinputuap.dll"};
constexpr int kTraceSlots = 8;  // dll * 2 (GetState, GetStateEx)
GetState_t g_trace_original[kTraceSlots] = {};
bool g_trace_hooked[kTraceSlots] = {};

void note_caller(int slot, void* return_address) {
    static std::mutex mutex;
    static std::pair<int, HMODULE> seen[48];
    static int count = 0;
    HMODULE caller = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(return_address), &caller);
    {
        std::lock_guard lock(mutex);
        for (int i = 0; i < count; ++i)
            if (seen[i].first == slot && seen[i].second == caller) return;
        if (count < 48) seen[count++] = {slot, caller};
    }
    wchar_t path[MAX_PATH] = L"(no module: generated code)";
    if (caller) GetModuleFileNameW(caller, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&note_caller), &self);
    log("xinput trace: %s!%s called from %s+0x%llx%s", kTraceDlls[slot / 2], slot % 2 ? "XInputGetStateEx" : "XInputGetState",
        narrow(name).c_str(),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(return_address) - reinterpret_cast<uintptr_t>(caller)),
        caller == self ? " (GyroUE itself)" : "");
}

template <int Slot>
DWORD WINAPI trace_get_state(DWORD user, XINPUT_STATE* state) {
    note_caller(Slot, _ReturnAddress());
    return g_trace_original[Slot](user, state);
}

GetState_t trace_detour(int slot) {
    static const GetState_t detours[kTraceSlots] = {&trace_get_state<0>, &trace_get_state<1>, &trace_get_state<2>, &trace_get_state<3>,
                                                    &trace_get_state<4>, &trace_get_state<5>, &trace_get_state<6>, &trace_get_state<7>};
    return detours[slot];
}

// Watcher thread: hook XInput DLLs as they appear; also note the other controller APIs a game can use.
void trace_pass() {
    for (int d = 0; d < 4; ++d) {
        HMODULE m = GetModuleHandleA(kTraceDlls[d]);
        if (!m) continue;
        for (int k = 0; k < 2; ++k) {
            const int slot = d * 2 + k;
            if (g_trace_hooked[slot]) continue;
            g_trace_hooked[slot] = true;
            void* target = reinterpret_cast<void*>(GetProcAddress(m, k ? MAKEINTRESOURCEA(100) : "XInputGetState"));
            if (!target) continue;
            const bool ok = hooks::hook_function(target, reinterpret_cast<void*>(trace_detour(slot)),
                                                 reinterpret_cast<void**>(&g_trace_original[slot]));
            log("xinput trace: %s!%s %s", kTraceDlls[d], k ? "XInputGetStateEx" : "XInputGetState", ok ? "hooked" : "could not be hooked");
        }
    }
    static bool seen[3] = {};
    const char* apis[] = {"Windows.Gaming.Input.dll", "GameInput.dll", "dinput8.dll"};
    for (int i = 0; i < 3; ++i)
        if (!seen[i] && GetModuleHandleA(apis[i])) {
            seen[i] = true;
            log("xinput trace: %s is loaded in the game", apis[i]);
        }
}

void watch() {
    GetState_t real = original(g_get_state, "XInputGetState", "xinput1_4.dll");
    if (!real) log("xinput: the real XInputGetState could not be resolved; real controllers count as absent");
    for (;;) {
        uint32_t mask = 0;
        for (DWORD i = 0; real && i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE s{};
            if (real(i, &s) == ERROR_SUCCESS) mask |= 1u << i;
        }
        g_real_mask = mask;
        if (g_settings.mode == Auto) scan_modules();
        if (g_settings.trace) trace_pass();
        hooks::verify();  // an overlay may rewrite the game's XInput imports after GyroUE patched them (Mortal Shell: EOS)
        g_watch_ready = true;
        Sleep(1000);
    }
}

// ------------------------------------------------------------------ game thread

struct Pad {
    SDL_JoystickID id = 0;
    uint64_t physical = 0;
    ULONGLONG next_lookup = 0;
};
Pad g_pad;
ULONGLONG g_seen_since = 0;          // the current pad was first found (grace period before showing it)
std::string g_last_decision;
uint32_t g_rumble_sent = 0;
ULONGLONG g_rumble_refresh = 0;
bool g_watch_started = false;
constexpr ULONGLONG kGraceMs = 2000;

// The SDL gamepad of GyroLib's selected controller (GyroLib opened it; SDL_GetGamepadFromID adds no reference).
SDL_Gamepad* find_pad(gl_context* ctx, gl_sdl* reader, const gl_output& out) {
    const uint64_t physical = out.physical_id ? out.physical_id : gl_get_selected_device(ctx);
    const ULONGLONG now = GetTickCount64();
    if (g_pad.id && g_pad.physical == physical && now < g_pad.next_lookup)
        if (SDL_Gamepad* pad = SDL_GetGamepadFromID(g_pad.id)) return pad;
    g_pad = {};
    g_pad.next_lookup = now + 500;
    if (!physical) return nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return nullptr;
    for (int i = 0; i < count && !g_pad.id; ++i) {
        const uint64_t endpoint = gl_sdl_endpoint_for_instance(reader, ids[i]);
        if (!endpoint) continue;
        for (uint32_t e = 0, n = gl_endpoint_count(ctx); e < n; ++e) {
            gl_endpoint info{};
            if (gl_get_endpoint(ctx, e, &info) == GL_OK && info.id == endpoint && info.physical_id == physical) {
                g_pad.id = ids[i];
                g_pad.physical = physical;
                break;
            }
        }
    }
    SDL_free(ids);
    return g_pad.id ? SDL_GetGamepadFromID(g_pad.id) : nullptr;
}

SHORT axis(SDL_Gamepad* pad, SDL_GamepadAxis a, bool invert) {
    int v = SDL_GetGamepadAxis(pad, a);
    if (invert) v = -v;  // SDL: down is positive; XInput: up is positive
    return SHORT(std::clamp(v, -32768, 32767));
}

BYTE trigger(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const int v = std::max(0, int(SDL_GetGamepadAxis(pad, a)));
    return BYTE(std::min(255, (v * 255 + 16383) / 32767));
}

XINPUT_GAMEPAD read_pad(SDL_Gamepad* pad, bool suppress_right_stick) {
    XINPUT_GAMEPAD g{};
    auto set = [&](bool on, WORD bit) {
        if (on) g.wButtons |= bit;
    };
    // Face buttons: by position (SDL3's south/east/west/north, like an Xbox pad), or swapped A<->B and X<->Y when
    // asked (Nintendo layout: the printed A is the right button). The swap doesn't depend on SDL's button labels, so
    // it also works for DirectInput pads SDL can't identify.
    const SDL_GamepadButton face[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                      SDL_GAMEPAD_BUTTON_NORTH};
    const WORD by_position[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
    const WORD swapped[] = {XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X};
    for (int i = 0; i < 4; ++i)
        set(SDL_GetGamepadButton(pad, face[i]), g_settings.nintendo_labels ? swapped[i] : by_position[i]);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK), XINPUT_GAMEPAD_BACK);
    if (g_settings.touchpad_back) set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD), XINPUT_GAMEPAD_BACK);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START), XINPUT_GAMEPAD_START);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE), kGuide);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK), XINPUT_GAMEPAD_LEFT_THUMB);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK), XINPUT_GAMEPAD_RIGHT_THUMB);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), XINPUT_GAMEPAD_LEFT_SHOULDER);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), XINPUT_GAMEPAD_RIGHT_SHOULDER);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP), XINPUT_GAMEPAD_DPAD_UP);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN), XINPUT_GAMEPAD_DPAD_DOWN);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT), XINPUT_GAMEPAD_DPAD_LEFT);
    set(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT), XINPUT_GAMEPAD_DPAD_RIGHT);
    g.bLeftTrigger = trigger(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    g.bRightTrigger = trigger(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    g.sThumbLX = axis(pad, SDL_GAMEPAD_AXIS_LEFTX, false);
    g.sThumbLY = axis(pad, SDL_GAMEPAD_AXIS_LEFTY, true);
    if (!suppress_right_stick) {  // GyroLib's flick stick owns the right stick
        g.sThumbRX = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX, false);
        g.sThumbRY = axis(pad, SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    return g;
}

std::wstring widen(const std::string& text) {
    std::wstring out(text.size(), L' ');
    out.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()))));
    return out;
}

void stop_rumble(SDL_Gamepad* pad) {
    if (g_rumble_sent && pad) SDL_RumbleGamepad(pad, 0, 0, 0);
    g_rumble_sent = 0;
}

}  // namespace

// UE only checks empty XInput slots again after WM_DEVICECHANGE (XInputInterface's bNeedsControllerStateUpdate), so
// tell its windows (on this, the game thread) when our controller appears or goes.
bool sony_library() { return g_native[Sony]; }

void announce_device_change() {
    int posted = 0;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(hwnd, cls, 64) && std::wcscmp(cls, L"UnrealWindow") == 0 &&
                PostMessageW(hwnd, WM_DEVICECHANGE, 0x0007 /* DBT_DEVNODES_CHANGED */, 0))
                ++*reinterpret_cast<int*>(param);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&posted));
    log("xinput: WM_DEVICECHANGE posted to %d game window(s)", posted);
}

void install(const Settings& settings) {
    g_settings = settings;
    const char* modes[] = {"off", "auto", "always"};
    g_settings.mode = std::clamp(g_settings.mode, 0, 2);
    log("settings: XInput=%d (%s) XInputNintendoLabels=%d XInputTouchpadBack=%d", g_settings.mode, modes[g_settings.mode],
        g_settings.nintendo_labels ? 1 : 0, g_settings.touchpad_back ? 1 : 0);
    if (g_settings.mode == Off) return;
    if (hooks::inline_mode()) {
        log("xinput: not available for this game (protected exe, no import-table hooks); XInput translation off");
        g_settings.mode = Off;
        return;
    }
    bool any = hooks::add(nullptr, "XInputGetState", reinterpret_cast<void*>(&hk_get_state), reinterpret_cast<void**>(&g_get_state));
    hooks::add(nullptr, "XInputSetState", reinterpret_cast<void*>(&hk_set_state), reinterpret_cast<void**>(&g_set_state));
    hooks::add(nullptr, "XInputGetCapabilities", reinterpret_cast<void*>(&hk_get_caps), reinterpret_cast<void**>(&g_get_caps));
    hooks::add(nullptr, "XInputEnable", reinterpret_cast<void*>(&hk_enable), reinterpret_cast<void**>(&g_enable));
    // The Windows SDK's xinput.lib imports by ordinal; xinput1_3 and xinput1_4 share these ordinals (xinput9_1_0 has
    // other ones, but exports everything by name and has no XInputGetStateEx).
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll"}) {
        any = hooks::add_ordinal(dll, 2, "XInputGetState", reinterpret_cast<void*>(&hk_get_state), reinterpret_cast<void**>(&g_get_state)) || any;
        hooks::add_ordinal(dll, 3, "XInputSetState", reinterpret_cast<void*>(&hk_set_state), reinterpret_cast<void**>(&g_set_state));
        hooks::add_ordinal(dll, 4, "XInputGetCapabilities", reinterpret_cast<void*>(&hk_get_caps), reinterpret_cast<void**>(&g_get_caps));
        hooks::add_ordinal(dll, 5, "XInputEnable", reinterpret_cast<void*>(&hk_enable), reinterpret_cast<void**>(&g_enable));
        hooks::add_ordinal(dll, 100, "XInputGetStateEx", reinterpret_cast<void*>(&hk_get_state_ex), reinterpret_cast<void**>(&g_get_state_ex));
    }
    log("xinput: XInputGetState hook %s (%s)", any ? "installed" : "registered, no game module imports it yet",
        any ? hooks::source_dll("XInputGetState").c_str() : "patched when a module that imports it loads");
    // Auto watches how the game reads controllers; XInput=2 keeps it from reading them itself (HID devices, also
    // through Sony's pad library DLL: nativepad.cpp; and Raw Input for game controllers).
    hooks::add(nullptr, "RegisterRawInputDevices", reinterpret_cast<void*>(&hk_register_raw), reinterpret_cast<void**>(&g_register_raw));
    hooks::add(nullptr, "GetRawInputData", reinterpret_cast<void*>(&hk_get_raw_data), reinterpret_cast<void**>(&g_get_raw_data));
    nativepad::refuse_pads(g_settings.mode == Always);
    if (g_settings.mode == Always)
        log("xinput: XInput=2: the game's own access to PlayStation/Nintendo controllers is refused; it only gets the Xbox controller");
    if (g_settings.mode == Auto) {
        hooks::add(nullptr, "DirectInput8Create", reinterpret_cast<void*>(&hk_di8_create), reinterpret_cast<void**>(&g_di8_create));
        check_imports();
    }
}

SDL_Gamepad* selected_gamepad(gl_context* ctx, gl_sdl* reader, const gl_output& out) { return find_pad(ctx, reader, out); }

void set_neutral(bool neutral) { g_neutral = neutral; }

bool frame(gl_context* ctx, gl_sdl* reader, const gl_output& out) {
    if (g_settings.mode == Off) return false;
    if (!g_watch_started) {
        g_watch_started = true;
        g_our_sdl = static_cast<HMODULE>(gl_runtime_sdl_handle());
        std::thread(watch).detach();
    }
    const ULONGLONG now = GetTickCount64();
    SDL_Gamepad* pad = find_pad(ctx, reader, out);
    if (!pad) g_seen_since = 0;
    else if (!g_seen_since) g_seen_since = now;

    // Why the pad is not shown to the game ("" = it is).
    std::string reason;
    uint32_t free_slots = ~g_real_mask.load() & 0xF;
    if (!pad) {
        reason = "no controller";
    } else if (const SDL_GamepadType type = SDL_GetGamepadType(pad);
               type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE) {
        reason = "the controller is already an Xbox (XInput) controller";
    } else if (SDL_GetGamepadSteamHandle(pad)) {
        reason = "Steam Input handles the controller";
    } else if (!g_watch_ready) {
        reason = "waiting for the first XInput check";
    } else if (g_real_mask) {  // also with XInput=2: the same pad through a remapper would act twice
        reason = "a real XInput controller is connected (Xbox pad, Steam Input or a remapper such as DS4Windows/DSX)";
    } else if (g_settings.mode == Auto && g_native[Any]) {
        reason = native_reason(Any);
    } else if (g_settings.mode == Auto && g_native[Sony] && SDL_GetGamepadVendor(pad) == 0x054C &&
               !nativepad::dualsense_ignored()) {
        // A PlayStation controller; other controllers are translated, and so is a DualSense the game's Sony library
        // closed unread (it only knows the DualShock 4: from the next start the DualSense is shown to it as one).
        reason = native_reason(Sony);
    } else if (!free_slots) {
        reason = "no free XInput slot";
    } else if (g_settings.mode == Auto && !g_native[Sony]) {
        // The game opening the controller's HID device: not with a Sony library, whose enumeration opens every HID
        // device (Mortal Shell opens keyboards and lighting controllers for reading too).
        if (const uint16_t vendor = SDL_GetGamepadVendor(pad); nativepad::opened_vendor(vendor)) {
            native_sign("it opens the controller's HID device itself (vendor " + std::to_string(vendor) + ")");
            reason = native_reason(Any);
        }
    }
    if (reason.empty() && now - g_seen_since < kGraceMs) reason = "waiting for the game to start reading controllers";
    const bool show = reason.empty();

    // State for the game: neutral while GyroLib's panel has the controller (set_neutral).
    XINPUT_GAMEPAD g{};
    if (show && !g_neutral) g = read_pad(pad, out.suppress_native_right_stick != 0);

    bool was_shown = false;
    DWORD slot = 0;
    {
        std::lock_guard lock(g_mutex);
        was_shown = g_exposed;
        if (show && (!g_exposed || !(free_slots & (1u << g_slot)))) {
            DWORD first = 0;
            while (!(free_slots & (1u << first))) ++first;
            g_slot = first;
        }
        g_exposed = show;
        if (show && std::memcmp(&g, &g_state.Gamepad, sizeof g) != 0) {
            g_state.Gamepad = g;
            ++g_state.dwPacketNumber;
        }
        if (!show) g_rumble = 0;
        slot = g_slot;
    }

    // Rumble: XInput keeps a motor speed until changed, SDL rumbles for a duration: refresh while nonzero.
    const uint32_t want = show && g_enabled ? g_rumble.load() : 0;
    if (show && (want != g_rumble_sent || (want && now >= g_rumble_refresh))) {
        SDL_RumbleGamepad(pad, Uint16(want >> 16), Uint16(want & 0xFFFF), want ? 1000 : 0);
        g_rumble_sent = want;
        g_rumble_refresh = now + 500;
    } else if (!show) {
        stop_rumble(pad);
    }

    // Log every decision change; notices only when the game gains or loses the controller.
    const std::string decision = show ? "shown as XInput controller " + std::to_string(slot) : "not shown: " + reason;
    if (decision != g_last_decision && reason.rfind("waiting", 0) != 0) {
        g_last_decision = decision;
        log("xinput: %s%s%s", pad ? SDL_GetGamepadName(pad) : "", pad ? " " : "", decision.c_str());
    }
    if (show != was_shown) {
        announce_device_change();
        if (show)
            notify::show(L"Controller shown to the game as an Xbox controller (XInput).", 6);
        else if (pad && reason != "no controller")
            notify::show(L"Xbox controller translation stopped: " + widen(reason) + L".", 10);
    }
    return show;
}

}  // namespace gyroue::xinput
