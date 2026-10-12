#pragma once
// The game's own access to PlayStation controllers (Sony's PC pad library, as libScePad.dll or linked into the exe,
// opens the controller's HID device with CreateFileW and reads its input reports with ReadFile /
// GetOverlappedResult). Used for:
// - XInput=2: refusing those opens, so the game only gets the Xbox controller (xinput.cpp);
// - auto mode: knowing which controllers the game opens itself (xinput.cpp);
// - GyroLib's flick stick with the game's native controller support: while GyroLib asks to suppress the native right
//   stick, the right stick in the reports the game reads is centred (the controller itself, and GyroLib's own SDL
//   reading, are untouched). Offered only once the report layout was checked against SDL's view of the stick.
#include <cstdint>

struct SDL_Gamepad;

namespace gyroue::nativepad {

// DllMain (after hooks::init): CreateFileW in the game modules and a separate libScePad*.dll; with suppression also
// ReadFile, GetOverlappedResult(Ex) and CloseHandle. Nothing on protected exes (inline-hook mode).
void install(bool suppression);

// XInput=2: refuse the game's opens of Sony/Nintendo HID devices and of the selected controller.
void refuse_pads(bool refuse);

// The game opened a HID device of this vendor itself (auto mode's native-support check).
bool opened_vendor(uint16_t vendor);

// Game thread, once per frame: pad = GyroLib's selected controller (null if none), suppress = GyroLib's
// suppress_native_right_stick for it. Returns true while the game reads that controller itself through a checked
// report layout, i.e. native right-stick suppression works (declare GL_HOST_NATIVE_STICK_SUPPRESSION then).
bool frame(SDL_Gamepad* pad, bool suppress);

}  // namespace gyroue::nativepad
