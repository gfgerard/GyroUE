#include "hooks.hpp"

#include "log.hpp"

#include <MinHook.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>

namespace gyroue::hooks {
namespace {

struct Hook {
    const char* dll;  // null = any
    const char* function;
    void* replacement;
    void** original;
    bool warned = false;
};

std::recursive_mutex g_mutex;
std::vector<Hook> g_hooks;
std::set<std::pair<HMODULE, size_t>> g_done;  // (module, hook index) already patched
std::wstring g_prefix;                        // lower-case "<project>-"
// The exe's import table is a protector stub (no name tables; the real imports are resolved later by
// the protector, after DllMain), so hooks on a named DLL go inline on that DLL's export instead.
bool g_inline = false;

using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryExA_t = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
LoadLibraryW_t g_load_w = nullptr;
LoadLibraryA_t g_load_a = nullptr;
LoadLibraryExW_t g_load_ex_w = nullptr;
LoadLibraryExA_t g_load_ex_a = nullptr;

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

bool is_game_module(HMODULE m) {
    if (m == GetModuleHandleW(nullptr)) return true;
    if (g_prefix.empty()) return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    std::wstring name = path;
    name = lower(name.substr(name.find_last_of(L"\\/") + 1));
    return name.rfind(g_prefix, 0) == 0;
}

// API-set names (api-ms-win-*, ext-ms-win-*) stand for system DLLs: GDK/OneCore builds import user32
// functions as e.g. ext-ms-win-ntuser-message-l1-1-0.dll!PeekMessageW. The function name decides then.
bool dll_matches(const char* imported, const char* wanted) {
    if (!wanted || _stricmp(imported, wanted) == 0) return true;
    return _strnicmp(imported, "api-ms-", 7) == 0 || _strnicmp(imported, "ext-ms-", 7) == 0;
}

// Where an import slot points, as "module+0xoffset" (diagnostics).
std::string describe(const void* address) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &owner))
        return "unmapped/dynamic code";
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(owner, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    char offset[32];
    std::snprintf(offset, sizeof offset, "+0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(owner)));
    return narrow(name) + offset;
}

// The resolved system function a hook with a named dll replaces (for import tables without names).
void* system_target(const Hook& hook) {
    if (!hook.dll) return nullptr;
    HMODULE m = GetModuleHandleA(hook.dll);
    return m ? reinterpret_cast<void*>(GetProcAddress(m, hook.function)) : nullptr;
}

// Patch one hook in one module's import table. Returns true if the slot now points to the hook.
bool patch_module(HMODULE module, Hook& hook) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool patched = false;
    void* const target = system_target(hook);
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!dll_matches(reinterpret_cast<const char*>(base + imp->Name), hook.dll)) continue;
        auto* names = imp->OriginalFirstThunk ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk) : nullptr;
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; slots->u1.Function; ++slots, names = names ? names + 1 : nullptr) {
            // Match by name; without a name table (stripped by some linkers/protectors), by resolved address.
            if (names && names->u1.AddressOfData && !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(by_name->Name, hook.function) != 0) continue;
            } else if (!target || reinterpret_cast<void*>(slots->u1.Function) != target) {
                continue;
            }
            void* current = reinterpret_cast<void*>(slots->u1.Function);
            if (current == hook.replacement) {
                patched = true;
                continue;
            }
            if (!*hook.original) *hook.original = current;
            else if (current != *hook.original) {
                if (!hook.warned)
                    log("hooks: %s in module %p resolves elsewhere (%p); such modules are left alone", hook.function,
                        static_cast<void*>(module), current);
                hook.warned = true;
                continue;
            }
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            slots->u1.Function = reinterpret_cast<ULONGLONG>(hook.replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

// Logs every message-loop import with where its slot points (by name, or by user32's real address).
// Returns true if the import table is a protector stub: imports present but none has a name table.
// Then every imported DLL is logged too (slot counts), for diagnosis.
bool scan_imports(HMODULE module) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool any_names = false;
    std::string summary;
    const char* pumps[] = {"PeekMessageW", "PeekMessageA", "GetMessageW", "GetMessageA", "MsgWaitForMultipleObjects",
                           "MsgWaitForMultipleObjectsEx", "DispatchMessageW", "DispatchMessageA"};
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* real[std::size(pumps)] = {};
    for (size_t i = 0; i < std::size(pumps); ++i)
        real[i] = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, pumps[i])) : nullptr;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        auto* names = imp->OriginalFirstThunk ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk) : nullptr;
        auto* slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        unsigned named = 0, ordinals = 0, total = 0;
        any_names = any_names || names;
        for (; slots->u1.Function; ++slots, names = names ? names + 1 : nullptr) {
            ++total;
            const void* to = reinterpret_cast<const void*>(slots->u1.Function);
            const char* name = nullptr;
            if (names && names->u1.AddressOfData) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) ++ordinals;
                else {
                    ++named;
                    name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
                }
            }
            for (size_t i = 0; i < std::size(pumps); ++i) {
                const bool by_name = name && std::strcmp(name, pumps[i]) == 0;
                const bool by_address = real[i] && to == real[i];
                if (by_name || by_address)
                    log("imports:   %s!%s -> %s%s", dll, name ? name : "(no name)", describe(to).c_str(),
                        by_address ? "" : " (not user32's own address)");
            }
        }
        char line[160];
        std::snprintf(line, sizeof line, "\n  %s: %u slots (%u by name, %u by ordinal)%s", dll, total, named, ordinals,
                      names ? "" : ", no name table");
        summary += line;
    }
    if (any_names) return false;
    log("imports: exe import table (rva 0x%lx) has no name tables: protected exe, real imports resolved later%s",
        dir.VirtualAddress, summary.c_str());
    return true;
}

// Inline hook on the export itself, for protected exes. *original receives the trampoline.
bool inline_hook(Hook& hook) {
    HMODULE m = GetModuleHandleA(hook.dll);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, hook.function)) : nullptr;
    if (!target) {
        log("hooks: inline %s!%s: export not found (module %s)", hook.dll, hook.function, m ? "loaded" : "not loaded");
        return false;
    }
    static const MH_STATUS init = MH_Initialize();
    MH_STATUS status = init;
    if (status == MH_OK) status = MH_CreateHook(target, hook.replacement, reinterpret_cast<LPVOID*>(hook.original));
    if (status == MH_OK) status = MH_EnableHook(target);
    log("hooks: inline %s!%s at %s: %s", hook.dll, hook.function, describe(target).c_str(), MH_StatusToString(status));
    return status == MH_OK;
}

std::vector<HMODULE> loaded_game_modules() {
    std::vector<HMODULE> out;
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 2048);
    out.push_back(GetModuleHandleW(nullptr));
    for (size_t i = 0; i < n; ++i)
        if (mods[i] != out.front() && is_game_module(mods[i])) out.push_back(mods[i]);
    return out;
}

void patch_all() {
    std::lock_guard lock(g_mutex);
    for (HMODULE m : loaded_game_modules()) {
        for (size_t i = 0; i < g_hooks.size(); ++i) {
            if (g_done.count({m, i})) continue;
            if (patch_module(m, g_hooks[i])) g_done.insert({m, i});
        }
    }
}

HMODULE WINAPI hk_load_w(LPCWSTR name) {
    HMODULE m = g_load_w(name);
    if (m) patch_all();
    return m;
}
HMODULE WINAPI hk_load_a(LPCSTR name) {
    HMODULE m = g_load_a(name);
    if (m) patch_all();
    return m;
}
HMODULE WINAPI hk_load_ex_w(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE m = g_load_ex_w(name, file, flags);
    if (m && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        patch_all();
    return m;
}
HMODULE WINAPI hk_load_ex_a(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE m = g_load_ex_a(name, file, flags);
    if (m && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        patch_all();
    return m;
}

}  // namespace

void init(const wchar_t* exe_stem) {
    // "<Project>-Win64-Shipping" -> modules are "<Project>-<Module>-Win64-Shipping.dll"
    std::wstring stem = lower(exe_stem);
    const size_t dash = stem.find(L'-');
    if (dash != std::wstring::npos && dash > 0) g_prefix = stem.substr(0, dash + 1);
    g_inline = scan_imports(GetModuleHandleW(nullptr));
    // GYROUE_INLINE=1 forces inline hooks (testing, or a protector this check doesn't recognize).
    wchar_t force[4] = {};
    if (GetEnvironmentVariableW(L"GYROUE_INLINE", force, 4) && force[0] == L'1') g_inline = true;
    add(nullptr, "LoadLibraryW", reinterpret_cast<void*>(&hk_load_w), reinterpret_cast<void**>(&g_load_w));
    add(nullptr, "LoadLibraryA", reinterpret_cast<void*>(&hk_load_a), reinterpret_cast<void**>(&g_load_a));
    add(nullptr, "LoadLibraryExW", reinterpret_cast<void*>(&hk_load_ex_w), reinterpret_cast<void**>(&g_load_ex_w));
    add(nullptr, "LoadLibraryExA", reinterpret_cast<void*>(&hk_load_ex_a), reinterpret_cast<void**>(&g_load_ex_a));
    log("hooks: game modules prefix '%s', %zu loaded, %s", narrow(g_prefix).c_str(), loaded_game_modules().size(),
        g_inline ? "inline hooks on exports" : "import-table hooks");
}

bool add(const char* dll, const char* function, void* replacement, void** original) {
    std::lock_guard lock(g_mutex);
    g_hooks.push_back({dll, function, replacement, original});
    const size_t index = g_hooks.size() - 1;
    // Protected exe: its import slots are filled by the protector later, so hook the export itself.
    // LoadLibrary hooks (dll null) only serve modular builds' later modules and are skipped there.
    if (g_inline) return g_hooks[index].dll && inline_hook(g_hooks[index]);
    bool any = false;
    for (HMODULE m : loaded_game_modules()) {
        if (patch_module(m, g_hooks[index])) {
            g_done.insert({m, index});
            any = true;
        }
    }
    return any;
}

bool inline_mode() { return g_inline; }

std::vector<HMODULE> game_modules() {
    std::lock_guard lock(g_mutex);
    return loaded_game_modules();
}

}  // namespace gyroue::hooks
