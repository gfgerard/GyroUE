#include "ue.hpp"

#include "hooks.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

namespace gyroue::ue {
namespace {

// ---------------------------------------------------------------- guarded memory access

// Only handle what a stray read can cause, and put PAGE_GUARD back. A plausible-looking pointer can land on another
// thread's stack guard page; swallowing STATUS_GUARD_PAGE_VIOLATION clears the guard, that thread can no longer grow
// its stack and the game later dies in ntdll (seen in PSButtonsUE, which shares this core: Luto, UE 5.5). Anything
// else (a real bug) still crashes normally.
int guard_filter(const EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        if (r->NumberParameters >= 2) {
            MEMORY_BASIC_INFORMATION mbi{};
            const auto page = reinterpret_cast<void*>(r->ExceptionInformation[1] & ~uintptr_t(0xFFF));
            DWORD old = 0;
            if (VirtualQuery(page, &mbi, sizeof(mbi))) VirtualProtect(page, 1, mbi.Protect | PAGE_GUARD, &old);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool guarded_copy(void* dst, uintptr_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (guard_filter(GetExceptionInformation())) {
        return false;
    }
}

bool guarded_store(uintptr_t dst, const void* src, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (guard_filter(GetExceptionInformation())) {
        return false;
    }
}

bool plausible(uintptr_t p, uintptr_t align = 8) {
    return p >= 0x10000 && p < 0x7FFFFFFF0000ull && (p & (align - 1)) == 0;
}

template <class T>
bool rd(uintptr_t address, T& out) {
    return guarded_copy(&out, address, sizeof(T));
}

uintptr_t rdp(uintptr_t address, uintptr_t align = 8) {
    uintptr_t p = 0;
    return rd(address, p) && plausible(p, align) ? p : 0;
}

// ---------------------------------------------------------------- module layout

struct Range {
    uintptr_t begin, end;
};

// Writable, non-executable sections of every game module (exe + "<Project>-*.dll" in modular builds).
// GUObjectArray lives in CoreUObject and FNamePool in Core; in monolithic builds both are in the exe.
// Writable data sections of the game modules. Protected exes may merge data into writable+executable
// sections; those are only scanned with include_exec (slower: they can be hundreds of MB).
std::vector<Range> writable_sections(bool include_exec = false) {
    std::vector<Range> out;
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            const DWORD c = s->Characteristics;
            if ((c & IMAGE_SCN_MEM_WRITE) && (include_exec || !(c & IMAGE_SCN_MEM_EXECUTE)))
                out.push_back({base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize});
        }
    }
    return out;
}

bool in_module(uintptr_t address) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &m) != 0;
}

std::string module_offset(uintptr_t address) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    char buffer[64];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &m)) {
        std::snprintf(buffer, sizeof(buffer), "heap 0x%llX", (unsigned long long)address);
        return buffer;
    }
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    std::snprintf(buffer, sizeof(buffer), "+0x%llX", (unsigned long long)(address - reinterpret_cast<uintptr_t>(m)));
    return narrow(name) + buffer;
}

// ---------------------------------------------------------------- state

constexpr int kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18;
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;

uintptr_t g_obj_array = 0;   // TUObjectArray: Objects**, PreAllocated*, Max, Num, MaxChunks, NumChunks
int g_item_stride = 0;       // sizeof(FUObjectItem)
uintptr_t g_name_blocks = 0; // FNamePool Blocks[]
bool g_header_len_high = true;  // FNameEntryHeader: Len in bits 6..15 (true) or 0..9

int g_super = -1;        // UStruct::SuperStruct
int g_child_props = -1;  // UStruct::ChildProperties
int g_field_class = 0x08, g_field_next = -1, g_field_name = -1;
// Where a property's type name lives in its class object: FFieldClass::Name at +0 (UE 4.25+ FField properties) or
// the UClass's UObject name (UE <= 4.24 UProperty objects, e.g. Life is Strange Remastered).
int g_field_class_name = 0;
int g_array_dim = -1, g_elem_size = -1, g_prop_offset = -1, g_prop_size = -1;

uintptr_t g_class_class = 0;
bool g_ready = false;
ULONGLONG g_next_try = 0;
int g_array_tries = 0;  // init() attempts without GUObjectArray

std::unordered_map<uint64_t, std::string> g_names;
std::map<std::pair<uintptr_t, std::string>, Prop> g_props;

// ---------------------------------------------------------------- names

std::string entry_string(uint32_t index) {
    const uint32_t block = index >> 16, offset = (index & 0xFFFF) * 2;
    if (block >= 8192) return {};
    const uintptr_t base = rdp(g_name_blocks + block * 8ull, 2);
    if (!base) return {};
    uint16_t h = 0;
    if (!rd(base + offset, h)) return {};
    const bool wide = g_header_len_high ? (h & 1) : (h >> 15) & 1;
    const uint32_t len = g_header_len_high ? (h >> 6) : (h & 0x3FF);
    if (len == 0 || len > 1024) return {};
    if (!wide) {
        std::string s(len, '\0');
        return guarded_copy(s.data(), base + offset + 2, len) ? s : std::string{};
    }
    std::wstring w(len, L'\0');
    if (!guarded_copy(w.data(), base + offset + 2, len * 2)) return {};
    return narrow(w);
}

std::string fname(uintptr_t address) {  // FName at address: ComparisonIndex, Number
    uint32_t pair[2] = {};
    if (!guarded_copy(pair, address, 8)) return {};
    const uint64_t key = (uint64_t(pair[1]) << 32) | pair[0];
    if (auto it = g_names.find(key); it != g_names.end()) return it->second;
    std::string s = entry_string(pair[0]);
    if (!s.empty() && pair[1] > 0) s += "_" + std::to_string(pair[1] - 1);
    g_names.emplace(key, s);
    return s;
}

// ---------------------------------------------------------------- object array

int32_t object_count() {
    int32_t n = 0;
    return rd(g_obj_array + 0x14, n) ? n : 0;
}

Obj object_at(int32_t i) {
    const uintptr_t chunks = rdp(g_obj_array);
    if (!chunks) return 0;
    const uintptr_t chunk = rdp(chunks + (i / 65536) * 8ull);
    if (!chunk) return 0;
    return rdp(chunk + uintptr_t(i % 65536) * g_item_stride);
}

// FChunkedFixedUObjectArray at a: Objects**, PreAllocatedObjects*, MaxElements, NumElements, MaxChunks,
// NumChunks, with items 1..16 of chunk 0 being live UObjects whose InternalIndex matches. Returns the item
// stride (sizeof(FUObjectItem)), 0 if the header doesn't match, -1 if the header matches but the items don't.
int object_array_stride(uintptr_t a, uintptr_t* chunk0_out = nullptr) {
    int32_t max = 0, num = 0, max_chunks = 0, num_chunks = 0;
    if (!rd(a + 0x10, max) || !rd(a + 0x14, num) || !rd(a + 0x18, max_chunks) || !rd(a + 0x1C, num_chunks)) return 0;
    if (num <= 1000 || num > max || max < 0x10000 || max > 0x10000000) return 0;
    if (max_chunks != (max + 65535) / 65536) return 0;
    if (num_chunks < (num + 65535) / 65536 || num_chunks > max_chunks) return 0;
    const uintptr_t chunks = rdp(a);
    const uintptr_t chunk0 = chunks ? rdp(chunks) : 0;
    if (chunk0_out) *chunk0_out = chunk0;
    if (!chunk0) return -1;
    for (int stride : {0x18, 0x10, 0x20, 0x28, 0x30}) {
        bool ok = true;
        for (int i = 1; i <= 16 && ok; ++i) {
            const uintptr_t o = rdp(chunk0 + uintptr_t(i) * stride);
            int32_t idx = -1;
            uintptr_t vtable = 0;
            ok = o && rd(o + kObjIndex, idx) && idx == i && rd(o, vtable) && in_module(vtable);
        }
        if (ok) return stride;
    }
    return -1;
}

// diagnose: log every candidate that passes the header checks and why its items were rejected.
bool find_object_array(const std::vector<Range>& sections, bool diagnose = false) {
    int candidates = 0;
    for (const Range& r : sections) {
        for (uintptr_t a = r.begin; a + 0x20 <= r.end; a += 8) {
            uintptr_t chunk0 = 0;
            const int stride = object_array_stride(a, &chunk0);
            if (stride > 0) {
                g_obj_array = a;
                g_item_stride = stride;
                return true;
            }
            if (stride == 0 || !diagnose || candidates >= 8) continue;
            ++candidates;
            std::string items;
            for (int i = 0; i < 4 && chunk0; ++i) {
                char buffer[96];
                const uintptr_t o = rdp(chunk0 + uintptr_t(i) * 0x18);
                int32_t idx = -1;
                uintptr_t vtable = 0;
                rd(o + kObjIndex, idx);
                rd(o, vtable);
                std::snprintf(buffer, sizeof buffer, " [%d] obj %p idx %d vtable %s", i, reinterpret_cast<void*>(o), idx,
                              vtable ? module_offset(vtable).c_str() : "-");
                items += buffer;
            }
            log("ue: object array candidate %s: chunk0 %p (stride 0x18):%s", module_offset(a).c_str(),
                reinterpret_cast<void*>(chunk0), items.c_str());
        }
    }
    return false;
}

// Some builds (Gears of War: E-Day, UE 5.6) allocate FUObjectArray on the heap and keep only an
// XOR-encoded pointer to it in a global; the key is an immediate in the code that reads it:
//     mov r64, [rip+global] ; mov r64, imm64 (key) ; xor r64, r64
// The exe's code is scanned once for that idiom; each (global, key) is decoded on every retry (the
// singleton may not exist yet) and accepted only if it holds a valid ObjObjects within its 0xB8 bytes.
struct EncodedPtr {
    uintptr_t global;
    uint64_t key;
    uintptr_t code;
};
constexpr size_t kMaxEncoded = 8192;
EncodedPtr g_encoded[kMaxEncoded];
size_t g_encoded_count = 0;
bool g_encoded_scanned = false;
uint64_t g_code_bytes = 0;      // scanned by scan_code_for_encoded_pointers (diagnostics)
unsigned g_code_regions = 0, g_code_skipped = 0, g_code_faults = 0;

void add_encoded(uintptr_t global, uint64_t key, uintptr_t code) {
    for (size_t i = 0; i < g_encoded_count; ++i)
        if (g_encoded[i].global == global && g_encoded[i].key == key) return;
    if (g_encoded_count < kMaxEncoded) g_encoded[g_encoded_count++] = {global, key, code};
}

// Raw scan of one committed, readable range (SEH-guarded; no C++ objects with destructors here).
void scan_xor_loads(uintptr_t begin, uintptr_t end) {
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(begin);
        const size_t n = end - begin;
        for (size_t i = 0; i + 20 <= n; ++i) {
            if (p[i + 1] != 0x8B || (p[i] != 0x48 && p[i] != 0x4C) || (p[i + 2] & 0xC7) != 0x05) continue;
            if ((p[i + 7] != 0x48 && p[i + 7] != 0x49) || p[i + 8] < 0xB8 || p[i + 8] > 0xBF) continue;
            if ((p[i + 17] & 0xF2) != 0x48 || (p[i + 18] != 0x33 && p[i + 18] != 0x31)) continue;
            int32_t disp = 0;
            uint64_t key = 0;
            std::memcpy(&disp, p + i + 3, 4);
            std::memcpy(&key, p + i + 9, 8);
            if (key == 0) continue;
            add_encoded(begin + i + 7 + intptr_t(disp), key, begin + i);
        }
    } __except (guard_filter(GetExceptionInformation())) {
        ++g_code_faults;
    }
}

void scan_code_for_encoded_pointers() {
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t a = base + sec->VirtualAddress;
            const uintptr_t end = std::min<uintptr_t>(a + sec->Misc.VirtualSize, image_end);
            while (a < end) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
                const uintptr_t region_end =
                    std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, end);
                const DWORD prot = mbi.Protect & 0xFF;
                const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                                      (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                                       prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_READONLY ||
                                       prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);
                if (readable) {
                    scan_xor_loads(a, region_end);
                    g_code_bytes += region_end - a;
                    ++g_code_regions;
                } else {
                    ++g_code_skipped;
                }
                a = region_end;
            }
        }
    }
}

bool find_encoded_object_array() {
    if (!g_encoded_scanned) {
        g_encoded_scanned = true;
        const ULONGLONG start = GetTickCount64();
        scan_code_for_encoded_pointers();
        log("ue: %zu XOR-encoded pointer loads in the game code (%llu ms; %.1f MB in %u regions, %u regions not "
            "readable, %u faults)",
            g_encoded_count, static_cast<unsigned long long>(GetTickCount64() - start), double(g_code_bytes) / 1048576.0,
            g_code_regions, g_code_skipped, g_code_faults);
    }
    for (size_t i = 0; i < g_encoded_count; ++i) {
        uint64_t encoded = 0;
        if (!rd(g_encoded[i].global, encoded) || !encoded) continue;
        const uintptr_t object_array = uintptr_t(encoded ^ g_encoded[i].key);
        if (!plausible(object_array)) continue;
        for (uintptr_t off = 0; off + 0x20 <= 0xB8; off += 8) {
            if (object_array_stride(object_array + off) <= 0) continue;
            g_obj_array = object_array + off;
            g_item_stride = object_array_stride(g_obj_array);
            log("ue: FUObjectArray %p decoded from %s ^ key (code at %s); ObjObjects at +0x%llX",
                reinterpret_cast<void*>(object_array), module_offset(g_encoded[i].global).c_str(),
                module_offset(g_encoded[i].code).c_str(), static_cast<unsigned long long>(off));
            return true;
        }
    }
    return false;
}


// Last resort, independent of how the code is compiled or protected: the engine's FUObjectArray lives
// somewhere in the process heap, so every committed private read-write region is scanned for a valid
// ObjObjects (same checks as above). Several GB, so it runs on its own thread, once every 30 s at most,
// and the game thread picks the result up in init().
std::atomic<uintptr_t> g_heap_array{0};
std::atomic<bool> g_heap_scan_running{false};
int g_heap_scan_runs = 0;
ULONGLONG g_next_heap_scan = 0;

// Raw prefilter over one region (SEH-guarded); full validation through object_array_stride().
uintptr_t scan_heap_region(uintptr_t begin, uintptr_t end) {
    __try {
        for (uintptr_t a = begin; a + 0x20 <= end; a += 8) {
            const auto* q = reinterpret_cast<const int32_t*>(a + 0x10);  // Max, Num, MaxChunks, NumChunks
            const int32_t max = q[0], num = q[1], max_chunks = q[2], num_chunks = q[3];
            if (num_chunks <= 0 || num_chunks > max_chunks || max_chunks > 4096) continue;
            if (max < 0x10000 || max_chunks != (max + 65535) / 65536 || num <= 1000 || num > max) continue;
            if (object_array_stride(a) > 0) return a;
        }
    } __except (guard_filter(GetExceptionInformation())) {
    }
    return 0;
}

DWORD WINAPI heap_scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    uint64_t bytes = 0;
    unsigned regions = 0;
    uintptr_t found = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && !found;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            found = scan_heap_region(base, end);
            bytes += mbi.RegionSize;
            ++regions;
        }
        a = end;
    }
    log("ue: heap scan %s (%.2f GB in %u regions, %llu ms)", found ? "found ObjObjects" : "found nothing",
        double(bytes) / (1024.0 * 1024.0 * 1024.0), regions, static_cast<unsigned long long>(GetTickCount64() - start));
    if (found) g_heap_array = found;
    g_heap_scan_running = false;
    return 0;
}

// Starts a background heap scan if none is running and the last one was long enough ago.
void start_heap_scan() {
    const ULONGLONG now = GetTickCount64();
    if (g_heap_scan_running || now < g_next_heap_scan || g_heap_scan_runs >= 10) return;
    g_next_heap_scan = now + 30000;
    ++g_heap_scan_runs;
    g_heap_scan_running = true;
    if (HANDLE thread = CreateThread(nullptr, 0, &heap_scan_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    else g_heap_scan_running = false;
}

void log_sections() {
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            char name[9] = {};
            std::memcpy(name, sec->Name, 8);
            const DWORD c = sec->Characteristics;
            log("ue: section %-8s rva 0x%08lX size 0x%08lX %c%c%c", name, sec->VirtualAddress, sec->Misc.VirtualSize,
                (c & IMAGE_SCN_MEM_READ) ? 'R' : '-', (c & IMAGE_SCN_MEM_WRITE) ? 'W' : '-', (c & IMAGE_SCN_MEM_EXECUTE) ? 'X' : '-');
        }
    }
}

bool find_name_pool(const std::vector<Range>& sections) {
    for (const Range& r : sections) {
        for (uintptr_t a = r.begin + 8; a + 8 <= r.end; a += 8) {
            const uintptr_t block0 = rdp(a, 2);
            if (!block0) continue;
            char bytes[24] = {};
            if (!guarded_copy(bytes, block0, sizeof(bytes))) continue;
            if (std::memcmp(bytes + 2, "None", 4) != 0 || std::memcmp(bytes + 8, "ByteProperty", 12) != 0) continue;
            uint32_t current_block = 0;
            if (!rd(a - 8, current_block) || current_block >= 8192) continue;
            if (!rdp(a + current_block * 8ull, 2)) continue;
            const uint16_t h = uint16_t(uint8_t(bytes[0]) | (uint8_t(bytes[1]) << 8));
            if ((h >> 6) == 4 && !(h & 1)) g_header_len_high = true;
            else if ((h & 0x3FF) == 4) g_header_len_high = false;
            else continue;
            g_name_blocks = a;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- reflection layout

std::string field_name(uintptr_t field) { return fname(field + g_field_name); }

std::string field_type(uintptr_t field) {
    const uintptr_t cls = rdp(field + g_field_class);
    return cls ? fname(cls + g_field_class_name) : std::string{};
}

Obj find_class(const char* name) {
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + kObjClass) == g_class_class && fname(o + kObjName) == name) return o;
    }
    return 0;
}

uintptr_t find_field(uintptr_t first, int next_off, int name_off, const char* name, int limit = 400) {
    uintptr_t f = first;
    for (int i = 0; f && i < limit; ++i) {
        if (fname(f + name_off) == name) return f;
        f = rdp(f + next_off);
    }
    return 0;
}

bool detect_layout() {
    // UClass "Class" is its own class.
    const int32_t n = object_count();
    for (int32_t i = 0; i < n && !g_class_class; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + kObjClass) == o && fname(o + kObjName) == "Class") g_class_class = o;
    }
    if (!g_class_class) return log("ue: UClass 'Class' not found"), false;

    const Obj object_cls = find_class("Object"), actor_cls = find_class("Actor"), controller_cls = find_class("Controller");
    if (!object_cls || !actor_cls || !controller_cls) return log("ue: core classes not found"), false;

    for (int off = 0x28; off <= 0x80 && g_super < 0; off += 8)
        if (rdp(actor_cls + off) == object_cls && rdp(controller_cls + off) == actor_cls) g_super = off;
    if (g_super < 0) return log("ue: SuperStruct offset not found"), false;

    // ChildProperties + FField::Next/NamePrivate: walk Controller's own properties to "ControlRotation".
    uintptr_t control_rotation = 0;
    for (int child = 0x38; child <= 0x80 && !control_rotation; child += 8) {
        const uintptr_t first = rdp(controller_cls + child);
        if (!first || child == g_super) continue;
        for (int name_off : {0x28, 0x20, 0x18, 0x30}) {
            for (int next_off : {0x20, 0x18, 0x10, 0x28}) {
                if (next_off == name_off) continue;
                if (uintptr_t f = find_field(first, next_off, name_off, "ControlRotation")) {
                    control_rotation = f;
                    g_child_props = child;
                    g_field_next = next_off;
                    g_field_name = name_off;
                    break;
                }
            }
            if (control_rotation) break;
        }
    }
    if (!control_rotation) {
        static bool logged = false;  // retried every 5 s; early in startup this is normal, so say it once
        if (!logged) log("ue: property layout not found yet (retrying every 5 s)");
        logged = true;
        return false;
    }
    const uintptr_t pawn = find_field(rdp(controller_cls + g_child_props), g_field_next, g_field_name, "Pawn");

    // FField::ClassPrivate, the FFieldClass* whose first member is its FName ("StructProperty"...). +0x08 in
    // stock UE 4.25-5.x, elsewhere in some builds (Life is Strange Remastered: Next 0x28, Name 0x18). Without it
    // every property type reads as "" and the probe finds no values.
    // UE <= 4.24 has no FField: properties are UObjects (UProperty) chained through UField::Next in
    // UStruct::Children, so the "FField" layout found above is the UObject one (Name 0x18, Next 0x28) and the type
    // is the UClass at +0x10, named by its own UObject name.
    g_field_class = -1;
    for (int name_in_class : {0, kObjName}) {
        for (int off = 0x08; off <= 0x40 && g_field_class < 0; off += 8) {
            if (off == g_field_next || off == g_field_name) continue;
            const uintptr_t cls = rdp(control_rotation + off);
            if (cls && fname(cls + name_in_class) == "StructProperty" &&
                (!pawn || fname(rdp(pawn + off) + name_in_class) == "ObjectProperty")) {
                g_field_class = off;
                g_field_class_name = name_in_class;
            }
        }
        if (g_field_class >= 0) break;
    }
    if (g_field_class < 0) {
        g_field_class = 0x08;
        g_field_class_name = 0;
        log("ue: property class offset not found, assuming 0x08 (property types may read empty)");
    }

    // FProperty: ArrayDim, ElementSize, Offset_Internal, and the size of FProperty (where subclass data starts).
    auto valid_rotator = [](uintptr_t f, int dim_off, int elem_off, int offset_off) {
        int32_t dim = 0, elem = 0, offset = 0;
        return rd(f + dim_off, dim) && dim == 1 && rd(f + elem_off, elem) && (elem == 12 || elem == 24) &&
               rd(f + offset_off, offset) && offset >= 0x28 && offset < 0x4000;
    };
    for (int elem_off = g_field_name + 0x10; elem_off <= g_field_name + 0x30 && g_elem_size < 0; elem_off += 4) {
        int32_t pawn_elem = 0;
        if (pawn && (!rd(pawn + elem_off, pawn_elem) || pawn_elem != 8)) continue;
        for (int offset_off = elem_off + 4; offset_off <= elem_off + 0x20; offset_off += 4) {
            if (!valid_rotator(control_rotation, elem_off - 4, elem_off, offset_off)) continue;
            // Validate FProperty size: a StructProperty's UScriptStruct follows it and is named "Rotator".
            // sizeof(FProperty) is pointer-aligned (0x78 in UE 4.25-5.x).
            for (int size = (offset_off + 0x20 + 7) & ~7; size <= offset_off + 0x40; size += 8) {
                const uintptr_t st = rdp(control_rotation + size);
                if (st && fname(st + kObjName) == "Rotator") {
                    g_array_dim = elem_off - 4;
                    g_elem_size = elem_off;
                    g_prop_offset = offset_off;
                    g_prop_size = size;
                    break;
                }
            }
            if (g_prop_size > 0) break;
        }
    }
    if (g_prop_size < 0) return log("ue: FProperty layout not found"), false;

    log("ue: layout (%s) SuperStruct=0x%X ChildProperties=0x%X Field.Class=0x%X Field.Next=0x%X Field.Name=0x%X "
        "Property.ElementSize=0x%X Offset_Internal=0x%X sizeof(Property)=0x%X ControlRotation type=%s",
        g_field_class_name ? "UProperty objects, UE <= 4.24" : "FField properties, UE 4.25+", g_super, g_child_props,
        g_field_class, g_field_next, g_field_name, g_elem_size, g_prop_offset, g_prop_size,
        field_type(control_rotation).c_str());
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public API

bool ready() { return g_ready; }

bool init() {
    if (g_ready) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_try) return false;
    g_next_try = now + 1000;

    const std::vector<Range> sections = writable_sections();
    if (!g_obj_array) {
        bool found = find_object_array(sections);
        // Heap FUObjectArray behind an XOR-encoded global (cheap after the one-time code scan).
        if (!found && g_array_tries >= 2) found = find_encoded_object_array();
        // Otherwise look for it in the whole heap, in the background.
        if (!found && g_heap_array) {
            g_obj_array = g_heap_array;
            g_item_stride = object_array_stride(g_obj_array);
            found = g_item_stride > 0;
            if (!found) g_obj_array = g_heap_array = 0;
        }
        if (!found && g_array_tries >= 3) start_heap_scan();
        // Not in the plain data sections after ~10 s: protected exes can merge data into writable+executable
        // sections. Scan those every 10 s (slow), and the first time log the section table and the candidates.
        if (!found && ++g_array_tries % 10 == 0) {
            const bool diagnose = g_array_tries == 10;
            if (diagnose) log_sections();
            found = find_object_array(writable_sections(true), diagnose);
            if (!found && diagnose) log("ue: GUObjectArray not found yet (retrying every 10 s)");
        }
        if (found)
            log("ue: GUObjectArray.ObjObjects at %s (item stride 0x%X, %d objects)", module_offset(g_obj_array).c_str(),
                g_item_stride, object_count());
    }
    if (!g_name_blocks && find_name_pool(sections))
        log("ue: FNamePool.Blocks at %s (header len %s)", module_offset(g_name_blocks).c_str(),
            g_header_len_high ? "high bits" : "low bits");
    if (!g_obj_array || !g_name_blocks) return false;
    if (!detect_layout()) {
        g_next_try = now + 5000;
        g_class_class = 0;
        g_super = g_child_props = g_field_next = g_field_name = -1;
        g_array_dim = g_elem_size = g_prop_offset = g_prop_size = -1;
        return false;
    }
    g_ready = true;
    return true;
}

std::string name_of(Obj o) { return o ? fname(o + kObjName) : std::string{}; }

Obj class_of(Obj o) { return o ? rdp(o + kObjClass) : 0; }

bool is_cdo(Obj o) {
    uint32_t flags = 0;
    return !rd(o + kObjFlags, flags) || (flags & (RF_ClassDefaultObject | RF_ArchetypeObject));
}

bool is_a(Obj o, const char* class_name) {
    Obj c = class_of(o);
    for (int i = 0; c && i < 64; ++i) {
        if (name_of(c) == class_name) return true;
        c = rdp(c + g_super);
    }
    return false;
}

Obj find_instance(const char* class_name) {
    if (!g_ready) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && !is_cdo(o) && is_a(o, class_name)) return o;
    }
    return 0;
}

Obj outer_of(Obj o) { return o ? rdp(o + 0x20) : 0; }

Obj find_subobject(Obj outer, const char* name) {
    if (!g_ready || !outer) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + 0x20) == outer && fname(o + kObjName) == name) return o;
    }
    return 0;
}

Obj find_subobject_of_class(Obj outer, const char* class_name) {
    if (!g_ready || !outer) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + 0x20) == outer && !is_cdo(o) && is_a(o, class_name)) return o;
    }
    return 0;
}

Prop find_prop(Obj cls, const char* name) {
    if (!g_ready || !cls) return {};
    const auto key = std::make_pair(cls, std::string(name));
    if (auto it = g_props.find(key); it != g_props.end()) return it->second;
    Prop p;
    Obj c = cls;
    for (int depth = 0; c && depth < 64 && !p.field; ++depth) {
        if (uintptr_t f = find_field(rdp(c + g_child_props), g_field_next, g_field_name, name)) {
            p.field = f;
            rd(f + g_prop_offset, p.offset);
            rd(f + g_elem_size, p.element_size);
            p.type = field_type(f);
        }
        c = rdp(c + g_super);
    }
    g_props.emplace(key, p);
    return p;
}

Prop prop_of(Obj o, const char* name) { return find_prop(class_of(o), name); }

Obj read_obj(Obj o, const char* name) {
    const Prop p = prop_of(o, name);
    return p ? rdp(o + p.offset) : 0;
}

bool read_bool(Obj o, const char* name, bool fallback) {
    const Prop p = prop_of(o, name);
    if (!p) return fallback;
    uint8_t info[4] = {};  // FieldSize, ByteOffset, ByteMask, FieldMask
    if (p.type != "BoolProperty" || !guarded_copy(info, p.field + g_prop_size, 4)) return fallback;
    uint8_t byte = 0;
    if (!rd(o + p.offset + info[1], byte)) return fallback;
    return (byte & info[3]) != 0;
}

Obj struct_type(const Prop& p) { return p ? rdp(p.field + g_prop_size) : 0; }

bool read_rotator(uintptr_t address, int32_t element_size, double& pitch, double& yaw, double& roll) {
    if (element_size == 24) {
        double v[3];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        pitch = v[0], yaw = v[1], roll = v[2];
        return true;
    }
    if (element_size == 12) {
        float v[3];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        pitch = v[0], yaw = v[1], roll = v[2];
        return true;
    }
    return false;
}

bool add_rotator(uintptr_t address, int32_t element_size, double pitch, double yaw) {
    if (element_size == 24) {
        double v[2];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        v[0] += pitch, v[1] += yaw;
        return guarded_store(address, v, sizeof(v));
    }
    if (element_size == 12) {
        float v[2];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        v[0] += float(pitch), v[1] += float(yaw);
        return guarded_store(address, v, sizeof(v));
    }
    return false;
}

void dump_props(Obj o, const char* stop_at_class) {
    Obj c = class_of(o);
    for (int depth = 0; c && depth < 64; ++depth) {
        const std::string cname = name_of(c);
        if (cname == stop_at_class) break;
        int count = 0;
        for (uintptr_t f = rdp(c + g_child_props); f && count < 2000; f = rdp(f + g_field_next), ++count) {
            int32_t offset = -1, size = 0;
            rd(f + g_prop_offset, offset);
            rd(f + g_elem_size, size);
            const std::string name = field_name(f);
            const std::string value = value_string(o, name.c_str());
            log("  %s.%s %s @0x%X size %d%s%s", cname.c_str(), name.c_str(), field_type(f).c_str(), offset, size,
                value.empty() ? "" : " = ", value.c_str());
        }
        c = rdp(c + g_super);
    }
}

std::string value_string(Obj o, const char* name) {
    const Prop p = prop_of(o, name);
    if (!p) return {};
    const uintptr_t a = o + p.offset;
    char buffer[96] = {};
    if (p.type == "FloatProperty") {
        float v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    } else if (p.type == "DoubleProperty") {
        double v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    } else if (p.type == "IntProperty") {
        int32_t v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%d", v);
    } else if (p.type == "ByteProperty" || p.type == "EnumProperty") {
        uint8_t v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%u", v);
    } else if (p.type == "BoolProperty") {
        return read_bool(o, name, false) ? "true" : "false";
    } else if (p.type == "StructProperty" && (p.element_size == 12 || p.element_size == 24)) {
        double pitch = 0, yaw = 0, roll = 0;
        if (name_of(struct_type(p)) == "Rotator" && read_rotator(a, p.element_size, pitch, yaw, roll))
            std::snprintf(buffer, sizeof(buffer), "(P %.2f Y %.2f R %.2f)", pitch, yaw, roll);
    }
    return buffer;
}

bool write_number(Obj o, const char* name, double value) {
    const Prop p = prop_of(o, name);
    if (!p) return false;
    const uintptr_t a = o + p.offset;
    if (p.type == "FloatProperty") {
        const float v = float(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "DoubleProperty") return guarded_store(a, &value, sizeof(value));
    if (p.type == "IntProperty") {
        const int32_t v = int32_t(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "ByteProperty") {
        const uint8_t v = uint8_t(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "BoolProperty") {
        uint8_t info[4] = {}, byte = 0;  // FieldSize, ByteOffset, ByteMask, FieldMask
        if (!guarded_copy(info, p.field + g_prop_size, 4) || !rd(a + info[1], byte)) return false;
        byte = value != 0 ? uint8_t(byte | info[3]) : uint8_t(byte & ~info[3]);
        return guarded_store(a + info[1], &byte, 1);
    }
    return false;
}

// ---------------------------------------------------------------- probe (snapshot diff)

namespace {

constexpr int kObjOuter = 0x20;

struct Leaf {
    std::string label;
    uintptr_t address;
    int kind;  // 0 float, 1 double, 2 rotator (float), 3 rotator (double)
    double base[3];
    Obj obj = 0;  // object the value lives in
};
std::vector<Leaf> g_leaves;

// Raw memory of each probed object (up to its class PropertiesSize), to find non-reflected C++ state.
struct RawObj {
    Obj o;
    std::string label;
    std::vector<uint8_t> bytes;
};
std::vector<RawObj> g_raw;
int g_snapshots = 0;  // snapshots this session (labels each diff in GyroUE-probe.log)

// A rotation that followed the camera in a diff: a reflected rotator property, or a raw rotator
// (pitch, yaw) at an offset of a non-reflected object. Accumulated over every scored diff.
struct Candidate {
    std::string object;    // profile path ("Pawn.View")
    std::string cls;       // class name (informative)
    std::string property;  // reflected rotator ("" = raw)
    int32_t offset = -1;   // raw rotator offset (pitch, then yaw)
    int size = 0;          // 12 = float rotator, 24 = double rotator
    int hits = 0;          // scored diffs in which it held the camera's pitch and yaw
    std::string why_not;   // "" = usable in a profile, else why it is ignored
};
std::map<std::string, Candidate> g_candidates;
int g_scored_diffs = 0;

int32_t properties_size(Obj cls) {  // UStruct::PropertiesSize follows ChildProperties (UE 4.25-5.x)
    int32_t n = 0;
    return rd(cls + g_child_props + 8, n) && n > 0 && n <= 0x20000 ? n : 0;
}

// Size of the engine-class part of o (e.g. "SceneComponent": its transform state), 0 if o isn't one.
int32_t engine_part_size(Obj o, const char* engine_class) {
    Obj c = class_of(o);
    for (int i = 0; c && i < 64; ++i, c = rdp(c + g_super))
        if (name_of(c) == engine_class) return properties_size(c);
    return 0;
}

int32_t scene_component_size(Obj o) { return engine_part_size(o, "SceneComponent"); }

double wrap180(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180) a -= 360;
    if (a < -180) a += 360;
    return a;
}

bool near_angle(double v, double target) { return std::isfinite(v) && std::abs(wrap180(v - target)) < 0.6; }

bool read_leaf(const Leaf& l, double v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (l.kind == 0) { float f; if (!rd(l.address, f)) return false; v[0] = f; return true; }
    if (l.kind == 1) return rd(l.address, v[0]);
    return read_rotator(l.address, l.kind == 2 ? 12 : 24, v[0], v[1], v[2]);
}

// "Pawn.View(SHCharacterPlayViewComponent).Some.Prop" -> object "Pawn.View", class, property "Some.Prop"
void split_label(const std::string& label, std::string& object, std::string& cls, std::string& property) {
    const size_t open = label.find('('), close = label.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) {
        object = label, cls.clear(), property.clear();
        return;
    }
    object = label.substr(0, open);
    cls = label.substr(open + 1, close - open - 1);
    property = close + 2 <= label.size() ? label.substr(close + 2) : std::string{};
}

// Records one hit for a candidate (once per diff).
void score(const std::string& key, Candidate c, std::vector<std::string>& seen) {
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    auto [it, inserted] = g_candidates.try_emplace(key, std::move(c));
    ++it->second.hits;
}

void collect_struct(uintptr_t base, Obj st, const std::string& prefix, int depth);

void collect_field(uintptr_t base, uintptr_t f, const std::string& prefix, int depth) {
    int32_t offset = -1, size = 0, dim = 1;
    rd(f + g_prop_offset, offset);
    rd(f + g_elem_size, size);
    rd(f + g_array_dim, dim);
    if (offset < 0 || dim != 1) return;
    const std::string type = field_type(f), label = prefix + field_name(f);
    const uintptr_t a = base + offset;
    if (type == "FloatProperty") g_leaves.push_back({label, a, 0, {}});
    else if (type == "DoubleProperty") g_leaves.push_back({label, a, 1, {}});
    else if (type == "StructProperty") {
        const Obj st = rdp(f + g_prop_size);
        const std::string sname = name_of(st);
        if (sname == "Rotator" && (size == 12 || size == 24)) g_leaves.push_back({label, a, size == 12 ? 2 : 3, {}});
        else if (depth < 3 && st && sname != "Vector" && sname != "Quat" && sname != "Transform" &&
                 sname != "LinearColor" && sname != "Color")
            collect_struct(a, st, label + ".", depth + 1);
    }
}

void collect_struct(uintptr_t base, Obj st, const std::string& prefix, int depth) {
    for (Obj c = st; c; c = rdp(c + g_super)) {
        int count = 0;
        for (uintptr_t f = rdp(c + g_child_props); f && count < 2000; f = rdp(f + g_field_next), ++count)
            collect_field(base, f, prefix, depth);
    }
}

}  // namespace

bool camera_pov(Obj camera, double& pitch, double& yaw) {
    if (!g_ready || !camera) return false;
    static Obj cached_class = 0;
    static int32_t offset = -1, size = 0;
    const Obj cls = class_of(camera);
    if (cls != cached_class) {
        cached_class = cls;
        offset = -1;
        for (const char* cache : {"CameraCachePrivate", "CameraCache"}) {
            const Prop c = find_prop(cls, cache);
            const Prop pov = c ? find_prop(struct_type(c), "POV") : Prop{};
            const Prop rot = pov ? find_prop(struct_type(pov), "Rotation") : Prop{};
            if (rot && (rot.element_size == 12 || rot.element_size == 24)) {
                offset = c.offset + pov.offset + rot.offset;
                size = rot.element_size;
                break;
            }
        }
    }
    double roll = 0;
    return offset >= 0 && read_rotator(camera + offset, size, pitch, yaw, roll) && std::isfinite(pitch) &&
           std::isfinite(yaw);
}

size_t probe_snapshot(const std::vector<std::pair<std::string, Obj>>& roots) {
    g_leaves.clear();
    if (!g_ready) return 0;
    // Each object is labelled with its profile path, e.g. "Pawn.View(SHCharacterPlayViewComponent)":
    // the part before "(" can be used as Object= in a profile view.
    std::vector<std::pair<Obj, std::string>> objects;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (!o || is_cdo(o)) continue;
        std::string path;
        Obj x = o;
        for (int d = 0; x && d < 8 && path.empty(); ++d) {
            for (const auto& [role, r] : roots)
                if (r && x == r && path.empty()) path = role;
            if (path.empty()) x = rdp(x + kObjOuter);
        }
        if (path.empty()) continue;
        for (Obj y = o; y && y != x; y = rdp(y + kObjOuter)) {
            const size_t root_len = path.find('.') == std::string::npos ? path.size() : path.find('.');
            path.insert(root_len, "." + name_of(y));
        }
        objects.emplace_back(o, path + "(" + name_of(class_of(o)) + ")");
    }
    for (const auto& [o, label] : objects) {
        const std::string prefix = label + ".";
        const size_t first = g_leaves.size();
        Obj c = class_of(o);
        for (int depth = 0; c && depth < 64; ++depth) {
            const std::string cname = name_of(c);
            if (cname == "Object") break;
            int count = 0;
            for (uintptr_t f = rdp(c + g_child_props); f && count < 4000; f = rdp(f + g_field_next), ++count)
                collect_field(o, f, prefix, 0);
            c = rdp(c + g_super);
        }
        for (size_t i = first; i < g_leaves.size(); ++i) g_leaves[i].obj = o;
    }
    for (Leaf& l : g_leaves) read_leaf(l, l.base);
    g_raw.clear();
    for (const auto& [o, label] : objects) {
        const int32_t size = properties_size(class_of(o));
        RawObj r{o, label, std::vector<uint8_t>(size_t(size))};
        if (size && guarded_copy(r.bytes.data(), o, r.bytes.size())) g_raw.push_back(std::move(r));
    }
    ++g_snapshots;
    probe_log("probe: snapshot #%d of %zu objects, %zu values", g_snapshots, objects.size(), g_leaves.size());
    log("probe: snapshot #%d taken (%zu objects, %zu values) -> GyroUE-probe.log", g_snapshots, objects.size(),
        g_leaves.size());
    return g_leaves.size();
}

int probe_diff(bool verbose) {
    if (g_leaves.empty()) {
        if (g_snapshots == 0) {
            log("probe: F8 pressed but there is no snapshot yet: press F7 first");
            probe_log("probe: no snapshot (press F7 first)");
        } else {
            log("probe: snapshot #%d holds no values, nothing to compare (the engine's property layout wasn't read; "
                "see the 'ue: layout' line)", g_snapshots);
            probe_log("probe: snapshot #%d holds no values, nothing to compare", g_snapshots);
        }
        return 0;
    }
    probe_log("probe: %s diff against snapshot #%d", verbose ? "manual" : "automatic", g_snapshots);
    int rotators = 0, scalars = 0;
    if (verbose) {
        for (int pass = 0; pass < 2; ++pass) {
            for (const Leaf& l : g_leaves) {
                const bool rot = l.kind >= 2;
                if (rot != (pass == 0)) continue;
                double v[3];
                if (!read_leaf(l, v)) continue;
                double change = 0;
                for (int k = 0; k < 3; ++k) change = (std::max)(change, std::abs(v[k] - l.base[k]));
                if (rot) {
                    if (change < 0.5 || ++rotators > 150) continue;
                    probe_log("  rot %s (P %.2f Y %.2f R %.2f) -> (P %.2f Y %.2f R %.2f)", l.label.c_str(), l.base[0],
                              l.base[1], l.base[2], v[0], v[1], v[2]);
                } else {
                    if (change < 0.001 || ++scalars > 250) continue;
                    probe_log("  num %s %.4f -> %.4f", l.label.c_str(), l.base[0], v[0]);
                }
            }
        }
        probe_log("probe: diff done (%d rotators, %d numbers changed)", rotators, scalars);
    }

    // The camera's final view rotation, before and now.
    const Leaf* pov = nullptr;
    for (const Leaf& l : g_leaves)
        if (l.kind >= 2 && l.label.size() > 28 && l.label.ends_with("CameraCachePrivate.POV.Rotation")) pov = &l;
    double now[3] = {};
    if (!pov || !read_leaf(*pov, now)) {
        log("probe: diff of snapshot #%d (%d rotators, %d numbers changed; no camera POV, raw diff skipped) -> "
            "GyroUE-probe.log", g_snapshots, rotators, scalars);
        probe_log("probe: no camera POV rotation, raw diff skipped");
        return 0;
    }
    const double dp = now[0] - pov->base[0], dy = wrap180(now[1] - pov->base[1]);
    probe_log("probe: raw diff, POV pitch %.2f -> %.2f (d %.2f), yaw %.2f -> %.2f (d %.2f)", pov->base[0], now[0], dp,
              pov->base[1], now[1], dy);
    const double kRad = 57.29577951308232;
    int matches = 0;
    if (verbose) {
        for (const RawObj& r : g_raw) {
            std::vector<uint8_t> cur(r.bytes.size());
            if (!guarded_copy(cur.data(), r.o, cur.size())) continue;
            for (size_t off = 0x28; off + 4 <= cur.size() && matches < 300; off += 4) {
                for (int wide = 0; wide < 2; ++wide) {
                    if (wide && (off % 8 != 0 || off + 8 > cur.size())) continue;
                    double a = 0, b = 0;
                    if (wide) {
                        std::memcpy(&a, &r.bytes[off], 8);
                        std::memcpy(&b, &cur[off], 8);
                    } else {
                        float fa, fb;
                        std::memcpy(&fa, &r.bytes[off], 4);
                        std::memcpy(&fb, &cur[off], 4);
                        a = fa, b = fb;
                    }
                    if (!std::isfinite(a) || !std::isfinite(b) || a == b || std::abs(b) > 1e5) continue;
                    const char* what = nullptr;
                    if (near_angle(b, now[1]) && std::abs(dy) > 2) what = "= POV yaw";
                    else if (std::abs(b - now[0]) < 0.6 && std::abs(dp) > 2) what = "= POV pitch";
                    else if (std::abs(dy) > 2 && near_angle(b - a, dy)) what = "d = POV d yaw";
                    else if (std::abs(dp) > 2 && std::abs((b - a) - dp) < 0.6) what = "d = POV d pitch";
                    else if (std::abs(dy) > 2 && near_angle(b * kRad, now[1])) what = "= POV yaw (rad)";
                    else if (std::abs(dp) > 2 && std::abs(b * kRad - now[0]) < 0.6) what = "= POV pitch (rad)";
                    if (!what) continue;
                    ++matches;
                    probe_log("  raw %s+0x%zX %s %.4f -> %.4f  %s", r.label.c_str(), off, wide ? "double" : "float", a, b,
                              what);
                }
            }
        }
        probe_log("probe: raw diff done (%d matches)", matches);
    }

    // Scoring for the suggested profile: only turns large enough that matching the camera's pitch and
    // yaw at the same time can't be a coincidence.
    if (std::abs(dy) < 10) {
        probe_log("probe: turn too small to score (yaw %.1f, needs more than 10 degrees)", dy);
        log("probe: diff of snapshot #%d (%d rotators, %d numbers, %d raw matches; not scored, yaw turn %.0f < 10) -> "
            "GyroUE-probe.log", g_snapshots, rotators, scalars, matches, std::abs(dy));
        return 0;
    }
    ++g_scored_diffs;
    std::vector<std::string> seen;
    std::vector<uintptr_t> reflected;  // addresses of reflected rotators (the raw scan skips them)
    int found = 0;
    for (const Leaf& l : g_leaves) {
        if (l.kind < 2) continue;
        reflected.push_back(l.address);
        double v[3];
        if (&l == pov || !read_leaf(l, v)) continue;
        if (!near_angle(v[1], now[1]) || std::abs(v[0] - now[0]) > 0.6 || near_angle(l.base[1], v[1])) continue;
        Candidate c;
        split_label(l.label, c.object, c.cls, c.property);
        c.size = l.kind == 2 ? 12 : 24;
        if (c.object.rfind("Camera", 0) == 0) c.why_not = "camera output";
        else if (c.property.find("RelativeRotation") != std::string::npos) c.why_not = "component transform (output)";
        else if (c.object == "PC" && c.property == "ControlRotation") c.why_not = "the default view (already tried)";
        else if (c.property.find('.') != std::string::npos) c.why_not = "nested in a struct (not usable as Property=)";
        score(c.object + "|" + c.property, c, seen);
        ++found;
        if (!verbose) probe_log("  candidate %s (%s rotator)", l.label.c_str(), l.kind == 2 ? "float" : "double");
    }
    for (const RawObj& r : g_raw) {
        std::vector<uint8_t> cur(r.bytes.size());
        if (!guarded_copy(cur.data(), r.o, cur.size())) continue;
        const int32_t scene = scene_component_size(r.o);
        // The camera manager is mostly output (POV caches), but a game's own subclass may keep its look state past
        // the engine's PlayerCameraManager part (Life is Strange: LiSPlayerCameraManager +0x33CC/+0x33FC).
        const bool camera_root = r.label.rfind("Camera(", 0) == 0;
        const int32_t camera_engine = camera_root ? engine_part_size(r.o, "PlayerCameraManager") : 0;
        for (int wide = 0; wide < 2; ++wide) {
            const size_t elem = wide ? 8 : 4;
            for (size_t off = 0x28; off + 2 * elem <= cur.size(); off += elem) {
                double p = 0, y = 0, y0 = 0;
                if (wide) {
                    std::memcpy(&p, &cur[off], 8);
                    std::memcpy(&y, &cur[off + 8], 8);
                    std::memcpy(&y0, &r.bytes[off + 8], 8);
                } else {
                    float fp, fy, fy0;
                    std::memcpy(&fp, &cur[off], 4);
                    std::memcpy(&fy, &cur[off + 4], 4);
                    std::memcpy(&fy0, &r.bytes[off + 4], 4);
                    p = fp, y = fy, y0 = fy0;
                }
                if (!near_angle(y, now[1]) || std::abs(p - now[0]) > 0.6 || near_angle(y0, y)) continue;
                if (std::find(reflected.begin(), reflected.end(), r.o + off) != reflected.end()) continue;
                Candidate c;
                std::string ignored;
                split_label(r.label, c.object, c.cls, ignored);
                c.offset = int32_t(off);
                c.size = wide ? 24 : 12;
                if (c.object.rfind("Camera", 0) == 0 && !(camera_root && camera_engine > 0 && int32_t(off) >= camera_engine))
                    c.why_not = "camera output";
                else if (scene > 0 && int32_t(off) < scene) c.why_not = "component transform (output)";
                char key[32];
                std::snprintf(key, sizeof key, "|+0x%X|%d", c.offset, c.size);
                score(c.object + key, c, seen);
                ++found;
                if (!verbose)
                    probe_log("  candidate %s+0x%zX (%s rotator)", r.label.c_str(), off, wide ? "double" : "float");
            }
        }
    }
    probe_log("probe: %d rotations followed the camera (scored diff #%d)", found, g_scored_diffs);
    log("probe: diff of snapshot #%d (%d rotators, %d numbers, %d raw matches, %d candidates, scored #%d) -> "
        "GyroUE-probe.log", g_snapshots, rotators, scalars, matches, found, g_scored_diffs);
    return g_scored_diffs;
}

std::string probe_suggestion(const std::string& exe_stem, std::string& summary, bool as_profile) {
    summary.clear();
    if (g_scored_diffs < 2) return {};
    // Usable candidates that followed the camera in every scored diff (or all but one, from 4 on).
    const int needed = g_scored_diffs >= 4 ? g_scored_diffs - 1 : g_scored_diffs;
    std::vector<const Candidate*> usable, ignored;
    for (const auto& [key, c] : g_candidates) {
        if (c.hits < needed) continue;
        (c.why_not.empty() ? usable : ignored).push_back(&c);
    }
    // Group by object: one view per object (all its raw rotators get the delta, like Cronos' two copies).
    struct Group {
        std::string object, cls, property;
        std::vector<int32_t> offsets;
        int size = 0, hits = 0, rank = 0;
    };
    std::vector<Group> groups;
    for (const Candidate* c : usable) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
            return g.object == c->object && g.property.empty() == c->property.empty() && (!c->property.empty() || g.size == c->size);
        });
        if (it == groups.end() || !c->property.empty()) {
            Group g{c->object, c->cls, c->property, {}, c->size, 0, 0};
            // Inputs usually live in the pawn or its components; then the view target, then the controller and the
            // camera manager's own fields.
            g.rank = c->object.rfind("Pawn", 0) == 0 ? 3 : c->object.rfind("ViewTarget", 0) == 0 ? 2 : 1;
            groups.push_back(g);
            it = groups.end() - 1;
        }
        if (c->property.empty()) it->offsets.push_back(c->offset);
        it->hits = (std::max)(it->hits, c->hits);
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        if (a.offsets.size() != b.offsets.size()) return a.offsets.size() > b.offsets.size();
        return a.property.empty() < b.property.empty();  // reflected (named) before raw at equal rank
    });
    auto view_lines = [](const Group& g, const char* indent) {
        std::string s = std::string(indent) + "Object=" + g.object + "\n";
        if (!g.property.empty()) return s + indent + "Property=" + g.property + "\n";
        std::string offsets;
        for (int32_t off : g.offsets) offsets += (offsets.empty() ? "" : ",") + std::format("0x{:X}", off);
        return s + indent + "Offsets=" + offsets + "\n" + indent + "RotatorSize=" + std::to_string(g.size) + "\n";
    };

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string text =
        as_profile
            ? std::format("; GyroUE {} profile for {}, created by the camera probe ({} scored captures, "
                          "{:04}-{:02}-{:02} {:02}:{:02}).\n"
                          "; NOT VERIFIED. Restart the game to use it, then set Probe=0 in GyroUE\\GyroUE.ini.\n"
                          "; If the camera still ignores the gyro, try the alternatives below one at a time (or delete "
                          "this file).\n\n",
                          GYROUE_VERSION, exe_stem, g_scored_diffs, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute)
            : std::format("; GyroUE {} probe suggestion for {} ({} scored captures, {:04}-{:02}-{:02} {:02}:{:02}).\n"
                          "; The game already has a profile ({}.ini), so this was written next to it. NOT VERIFIED.\n"
                          "; To try it: rename this file to {}.ini (replacing the old one) and restart the game.\n"
                          "; If the camera still ignores the gyro, try the alternatives below one at a time.\n\n",
                          GYROUE_VERSION, exe_stem, g_scored_diffs, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                          exe_stem, exe_stem);
    if (groups.empty()) {
        text += "; No usable rotation followed the camera in every capture. The camera may be driven by state the probe\n"
                "; can't see (quaternions, input accumulators, animation). See ADVANCED.md, \"Finding the camera of a new game\".\n";
        summary = "no usable candidate";
    } else {
        const Group& best = groups.front();
        text += "[View.Default]\nLabel=Camera\n" + view_lines(best, "");
        summary = best.object + (best.property.empty() ? std::format(" raw x{}", best.offsets.size()) : "." + best.property);
        if (groups.size() > 1) {
            text += "\n; Alternatives (also followed the camera):\n";
            for (size_t i = 1; i < groups.size() && i < 8; ++i) text += view_lines(groups[i], ";   ") + ";\n";
        }
    }
    if (!ignored.empty()) {
        text += "\n; Also followed the camera but ignored:\n";
        for (size_t i = 0; i < ignored.size() && i < 12; ++i) {
            const Candidate* c = ignored[i];
            text += "  ; " + c->object + "(" + c->cls + ")" +
                    (c->property.empty() ? std::format("+0x{:X}", c->offset) : "." + c->property) + ": " + c->why_not + "\n";
        }
    }
    return text;
}

bool read_ptr(uintptr_t address, uintptr_t& out) {
    out = rdp(address);
    return out != 0;
}

bool read_i32(uintptr_t address, int32_t& out) { return rd(address, out); }

}  // namespace gyroue::ue
