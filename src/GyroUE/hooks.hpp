#pragma once
// Import-table hooks applied to every module of the game: the exe plus "<Project>-*.dll" engine
// modules of modular builds. Modules loaded later (FModuleManager -> LoadLibrary) are patched as
// they load, so hooks registered in DllMain also reach renderer modules that load after startup.
#include <windows.h>

#include <vector>

namespace gyroue::hooks {

// Call once from DllMain with the game exe stem (e.g. "Returnal-Win64-Shipping").
void init(const wchar_t* exe_stem);

// Register an import to redirect in all game modules. dll may be null to match any DLL name.
// *original receives the first resolved original function. Patches already-loaded modules now.
// Returns true if at least one module was patched so far.
bool add(const char* dll, const char* function, void* replacement, void** original);

// True when the exe's import table is a protector stub (or GYROUE_INLINE=1): add() then hooks the
// named DLL's export inline instead of import slots.
bool inline_mode();

// Game modules (exe first). Used for memory scans.
std::vector<HMODULE> game_modules();

}  // namespace gyroue::hooks
