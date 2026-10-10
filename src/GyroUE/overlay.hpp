#pragma once
// Optional in-game settings panel: GyroLib's DX12 overlay. Enabled only when gyrolib.ini has a
// menu key or the gamepad shortcut; otherwise no graphics hooks are installed at all.
#include <string>

struct gl_context;

namespace gyroue::overlay {

// DllMain (before the renderer starts): decide from gyrolib.ini and hook DXGI factory creation.
void install(const std::wstring& settings_path);

// Game thread, after GyroLib's context exists. Returns true while a renderer is available.
bool attach(gl_context* ctx);
// Game thread, before every gl_update.
void process();
// True when the panel renderer is usable (DX12 swapchain captured and overlay created).
bool available();
// Mask of GL_OVERLAY_CAPTURE_* bits while the panel is open.
unsigned capture();
// Game thread: poll the configured F-key (gl_get_menu_key) and toggle the panel on its press.
// Window messages for that key are no longer forwarded to GyroLib, so it never toggles twice.
void poll_menu_key(gl_context* ctx, bool focused);

}  // namespace gyroue::overlay
