#pragma once
#include <string>

namespace gyroue {

// Called once from DllMain with the mod folder (…\Win64\GyroUE) and the game exe stem.
void host_configure(const std::wstring& mod_dir, const std::wstring& exe_stem);

// Called on the game thread once per message pump (≈ once per frame), outside any engine lock.
// Returns true when the gyro applied a camera rotation this frame.
bool host_frame();

}  // namespace gyroue
