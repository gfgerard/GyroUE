#pragma once
#include <string>

namespace gyroue {

// Called once from DllMain with the mod folder (…\Win64\GyroUE) and the game exe stem.
void host_configure(const std::wstring& mod_dir, const std::wstring& exe_stem);

// Called on the game thread once per message pump (≈ once per frame), outside any engine lock.
// Returns true when the gyro applied a camera rotation this frame.
bool host_frame();

// A WM_INPUT message the game's thread received (before the game handles it): relative mouse motion counts as the
// player turning the camera, like the right stick (camera probe, camera checks). Reads it only, never consumes it.
void host_raw_input(void* raw_input_handle);

}  // namespace gyroue
