#pragma once
// The controller GyroLib reads through SDL (DualSense, Switch Pro, DirectInput/HID pads), shown to the game as an
// Xbox (XInput) controller: the game's own imports of XInputGetState/SetState/GetCapabilities/Enable are redirected
// (import-table hooks on game modules only, so SDL's own XInput use never sees it). For games that only support
// XInput and run without Steam Input (e.g. Epic). Off when the game already reads the pad itself (see xinput.cpp).
#include <cstdint>

struct gl_context;
struct gl_sdl;
struct gl_output;
struct SDL_Gamepad;

namespace gyroue::xinput {

enum Mode { Off = 0, Auto = 1, Always = 2 };

struct Settings {
    int mode = Auto;
    bool nintendo_labels = false;  // swap face buttons A<->B and X<->Y (Nintendo A = Xbox A) instead of by position
    bool touchpad_back = true;     // touchpad click also presses View (Back), like the Create/Share button
    bool trace = false;            // XInputTrace: log every module that calls XInput (inline hooks on the system DLLs)
};

// The game links Sony's pad library (auto mode's check: libScePad or hid.dll among its imports, or libScePad loaded).
bool sony_library();

// Game thread: posts WM_DEVICECHANGE (DBT_DEVNODES_CHANGED) to the game's UnrealWindows, as Windows does when a
// controller is plugged in or out: Unreal (and Sony's pad libraries linked into it) then look for controllers again.
void announce_device_change();

// DllMain, after hooks::init: registers the hooks the mode needs (none for Off).
void install(const Settings& settings);

// Game thread: the SDL gamepad of GyroLib's selected controller (GyroLib owns it), or null. Cached; any mode.
SDL_Gamepad* selected_gamepad(gl_context* ctx, gl_sdl* reader, const gl_output& out);

// Game thread, once per frame before frame(): while GyroLib's panel has the controller, the game reads an idle
// controller in every XInput slot (ours and real ones).
void set_neutral(bool neutral);

// Game thread, once per frame before frame(): long-press blocking's button mask (longpress.cpp, SDL button bits) for
// the controller shown as XInput and, when the selected pad is itself a real XInput controller the game polls (Xbox
// pad, Steam Input), for real XInput slots. Returns true in that second case (the filter works there).
bool set_buttons(SDL_Gamepad* pad, uint32_t hide, uint32_t tap);

// Game thread, once per frame after the SDL reader was polled and gl_update ran (out is that update's output).
// Updates the state the game reads and applies its rumble. Returns true while the pad is shown to the game.
bool frame(gl_context* ctx, gl_sdl* reader, const gl_output& out);

}  // namespace gyroue::xinput
