#pragma once
// Import-table hooks applied to every module of the game: the exe plus "<Project>-*.dll" engine
// modules of modular builds. Modules loaded later (FModuleManager -> LoadLibrary) are patched as
// they load, so hooks registered in DllMain also reach renderer modules that load after startup.
#include <windows.h>

#include <string>
#include <vector>

namespace gyroue::hooks {

// Call once from DllMain with the game exe stem (e.g. "Returnal-Win64-Shipping").
void init(const wchar_t* exe_stem);

// Register an import to redirect in all game modules (regular and delay-load import tables). dll may be
// null to match any DLL name. *original receives the first resolved original function; it stays null
// when the only imports are delay-loads of a DLL that isn't loaded yet (resolve it from source_dll()).
// Patches already-loaded modules now. Returns true if at least one module was patched so far.
bool add(const char* dll, const char* function, void* replacement, void** original);

// Same for imports by ordinal from exactly `dll` (e.g. the Windows SDK's xinput.lib imports XINPUT1_4.dll!2).
// `function` is the export's name, used for logs, source_dll() and resolving the original.
bool add_ordinal(const char* dll, WORD ordinal, const char* function, void* replacement, void** original);

// Same as add(), and also in Sony's pad library when the game ships it as a separate DLL (libScePad*.dll, patched as
// it loads), for hooks that must see how that library opens the controller.
bool add_with_pad_library(const char* dll, const char* function, void* replacement, void** original);

// True when the exe's import table is a protector stub (or GYROUE_INLINE=1): add() then hooks the
// named DLL's export inline instead of import slots.
bool inline_mode();

// Name of the DLL whose import of `function` was patched first (e.g. "XINPUT1_4.dll"), or "" if none yet.
std::string source_dll(const char* function);

// DLLs imported by the loaded game modules (regular and delay-load tables), as written in the import table.
struct ImportedDll {
    std::string name;
    bool delay = false;
};
std::vector<ImportedDll> imported_dlls();

// Game modules (exe first). Used for memory scans.
std::vector<HMODULE> game_modules();

}  // namespace gyroue::hooks
