#include "nativepad.hpp"

#include "hooks.hpp"
#include "log.hpp"

#include <windows.h>
#include <hidsdi.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <string>

namespace gyroue::nativepad {
namespace {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultEx_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);

CreateFileW_t g_create_file = nullptr;
ReadFile_t g_read_file = nullptr;
GetOverlappedResult_t g_get_overlapped = nullptr;
GetOverlappedResultEx_t g_get_overlapped_ex = nullptr;
CloseHandle_t g_close_handle = nullptr;

// The original of an import: from the hook, else from the DLL the game imports it from (never called from DllMain).
template <typename F>
F original(F& slot, const char* function, const char* fallback_dll) {
    if (F f = reinterpret_cast<F>(InterlockedCompareExchangePointer(reinterpret_cast<void**>(&slot), nullptr, nullptr)))
        return f;
    std::string dll = hooks::source_dll(function);
    if (dll.empty()) dll = fallback_dll;
    HMODULE m = LoadLibraryA(dll.c_str());
    F f = m ? reinterpret_cast<F>(GetProcAddress(m, function)) : nullptr;
    if (f) InterlockedExchangePointer(reinterpret_cast<void**>(&slot), reinterpret_cast<void*>(f));
    return f;
}

constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;

bool g_suppression = false;            // NativeFlickStick: the read hooks are installed
std::atomic<bool> g_refuse{false};     // XInput=2
std::atomic<uint32_t> g_pad_ids{0};    // selected controller: vendor << 16 | product
std::atomic<int> g_refused{0};

// Vendor IDs of HID devices the game opened itself (auto mode's check).
constexpr int kMaxVendors = 8;
std::atomic<uint16_t> g_vendors[kMaxVendors];

// ------------------------------------------------------------------ HID paths

bool is_hid_path(const wchar_t* path) {
    return path && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\' &&
           std::towlower(path[4]) == L'h' && std::towlower(path[5]) == L'i' && std::towlower(path[6]) == L'd';
}

// Vendor or product ID in a HID device path: USB "\\?\hid#vid_054c&pid_0ce6&...", Bluetooth
// "\\?\hid#{00001124-...}_vid&0002054c_pid&0ce6#..." (there "0002" says where the ID comes from). 0 if absent.
uint16_t path_id(const wchar_t* path, const wchar_t* key) {
    for (const wchar_t* p = path; p[0] && p[1] && p[2] && p[3]; ++p) {
        if (std::towlower(p[0]) != key[0] || std::towlower(p[1]) != key[1] || std::towlower(p[2]) != key[2] ||
            (p[3] != L'_' && p[3] != L'&'))
            continue;
        uint32_t value = 0;
        int digits = 0;
        for (const wchar_t* d = p + 4; digits < 8 && std::iswxdigit(*d); ++d, ++digits)
            value = value << 4 | uint32_t(std::iswdigit(*d) ? *d - L'0' : std::towlower(*d) - L'a' + 10);
        if (digits == 4 || digits == 8) return uint16_t(value);
    }
    return 0;
}

// XInput=2: PlayStation and Nintendo pads, plus the selected controller.
bool refused_device(uint16_t vendor, uint16_t product) {
    if (vendor == kSony || vendor == kNintendo) return true;
    const uint32_t ids = g_pad_ids;
    return ids && vendor == ids >> 16 && product == (ids & 0xFFFF);
}

// ------------------------------------------------------------------ controllers the game reads

// A Sony controller handle the game opened for reading. The handle is published last (release) and cleared first,
// so the read hooks only need an atomic compare to skip every other file.
struct Device {
    std::atomic<HANDLE> handle{nullptr};
    uint16_t vendor = 0, product = 0;
    DWORD report_length = 0;                // the device's input report length (HidP_GetCaps), 0 = unknown
    std::atomic<ULONGLONG> last_report{0};  // tick of the last report with a known layout
    std::atomic<int32_t> stick{-1};         // right stick in that report before any change: x | y << 8
    std::atomic<bool> logged{false};        // first report logged
    std::atomic<bool> crc_warned{false};
};
constexpr int kMaxDevices = 8;
Device g_devices[kMaxDevices];
std::atomic<int> g_device_count{0};

// Overlapped reads of those handles still in flight: the data arrives later, when GetOverlappedResult(Ex) reports
// the read done, and is changed then (before the game copies it out of its buffer).
struct Pending {
    OVERLAPPED* ol = nullptr;
    HANDLE handle = nullptr;
    uint8_t* buffer = nullptr;
    DWORD size = 0;
};
constexpr int kMaxPending = 16;
Pending g_pending[kMaxPending];
std::atomic<int> g_pending_count{0};
std::mutex g_mutex;  // guards g_devices slot changes and g_pending

std::atomic<bool> g_suppress{false};  // centre the selected controller's right stick in the reports (set per frame)
std::atomic<uint32_t> g_changed{0};   // reports changed so far (diagnostics)

int find_device(HANDLE h) {
    for (int i = 0; i < kMaxDevices; ++i)
        if (g_devices[i].handle.load(std::memory_order_acquire) == h) return i;
    return -1;
}

// The device's input report length: one ReadFile can return several reports back to back (Pacific Drive's
// libScePad reads 640 bytes, ten 64-byte USB reports, at once). hid.dll is resolved here, never in DllMain.
DWORD input_report_length(HANDLE h) {
    using GetPreparsedData_t = BOOLEAN(__stdcall*)(HANDLE, PHIDP_PREPARSED_DATA*);
    using FreePreparsedData_t = BOOLEAN(__stdcall*)(PHIDP_PREPARSED_DATA);
    using GetCaps_t = NTSTATUS(__stdcall*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
    HMODULE hid = LoadLibraryW(L"hid.dll");
    if (!hid) return 0;
    auto get_data = reinterpret_cast<GetPreparsedData_t>(GetProcAddress(hid, "HidD_GetPreparsedData"));
    auto free_data = reinterpret_cast<FreePreparsedData_t>(GetProcAddress(hid, "HidD_FreePreparsedData"));
    auto get_caps = reinterpret_cast<GetCaps_t>(GetProcAddress(hid, "HidP_GetCaps"));
    PHIDP_PREPARSED_DATA data = nullptr;
    if (!get_data || !free_data || !get_caps || !get_data(h, &data)) return 0;
    HIDP_CAPS caps{};
    const bool ok = get_caps(data, &caps) == HIDP_STATUS_SUCCESS;
    free_data(data);
    return ok ? caps.InputReportByteLength : 0;
}

void track(HANDLE h, uint16_t vendor, uint16_t product) {
    const DWORD length = input_report_length(h);
    std::lock_guard lock(g_mutex);
    if (find_device(h) >= 0) return;
    for (Device& d : g_devices) {
        if (d.handle.load()) continue;
        d.vendor = vendor;
        d.product = product;
        d.report_length = length;
        d.last_report = 0;
        d.stick = -1;
        d.logged = false;
        d.crc_warned = false;
        d.handle.store(h, std::memory_order_release);
        ++g_device_count;
        log("nativepad: the game opened controller %04X:%04X for reading (handle %p, %lu-byte input reports)", vendor,
            product, h, length);
        return;
    }
    log("nativepad: more than %d controller handles open; %04X:%04X not followed", kMaxDevices, vendor, product);
}

void untrack(HANDLE h) {
    std::lock_guard lock(g_mutex);
    const int i = find_device(h);
    if (i < 0) return;
    g_devices[i].handle.store(nullptr, std::memory_order_release);
    --g_device_count;
    for (Pending& p : g_pending)
        if (p.ol && p.handle == h) {
            p = {};
            --g_pending_count;
        }
    log("nativepad: the game closed controller %04X:%04X (handle %p)", g_devices[i].vendor, g_devices[i].product, h);
}

void remember(OVERLAPPED* ol, HANDLE h, void* buffer, DWORD size) {
    std::lock_guard lock(g_mutex);
    Pending* free = nullptr;
    for (Pending& p : g_pending) {
        if (p.ol == ol) {
            p = {ol, h, static_cast<uint8_t*>(buffer), size};
            return;
        }
        if (!p.ol && !free) free = &p;
    }
    if (!free) {
        static bool warned = false;
        if (!warned) log("nativepad: more than %d controller reads in flight; some reports left alone", kMaxPending);
        warned = true;
        return;
    }
    *free = {ol, h, static_cast<uint8_t*>(buffer), size};
    ++g_pending_count;
}

// ------------------------------------------------------------------ input reports

// Where the right stick is in an input report as the game reads it from Windows (byte 0 = report ID), and where its
// CRC32 is (Bluetooth reports: CRC of 0xA1 + the bytes before it; 0 = none).
//   0x01  DualSense / DualShock 4 over USB, or their basic Bluetooth report: LX LY RX RY at bytes 1-4
//   0x31  DualSense over Bluetooth: sequence byte, then LX LY RX RY at bytes 2-5, CRC at 74
//   0x11  DualShock 4 over Bluetooth: two flag bytes, then LX LY RX RY at bytes 3-6, CRC at 74
// Axes are bytes 0..255 (128 = centre, Y down = 255).
struct Layout {
    int stick = 0;  // right stick X offset (Y follows); 0 = unknown report
    int crc = 0;
    const char* name = "unknown layout, left alone";
};

Layout layout_of(const uint8_t* r, DWORD n) {
    if (n >= 10 && r[0] == 0x01) return {3, 0, "report 0x01 (USB or basic Bluetooth)"};
    if (n >= 78 && r[0] == 0x31) return {4, 74, "report 0x31 (DualSense, Bluetooth)"};
    if (n >= 78 && r[0] == 0x11) return {5, 74, "report 0x11 (DualShock 4, Bluetooth)"};
    return {};
}

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t report_crc(const uint8_t* r, int length) {
    const uint8_t seed = 0xA1;  // Bluetooth HID input header, part of the checksum but not of the report
    return ~crc32(r, size_t(length), crc32(&seed, 1, 0xFFFFFFFFu));
}

uint32_t stored_crc(const uint8_t* r, int at) {
    return uint32_t(r[at]) | uint32_t(r[at + 1]) << 8 | uint32_t(r[at + 2]) << 16 | uint32_t(r[at + 3]) << 24;
}

// One input report (length bytes at r) of controller d: returns the right stick before any change (x | y << 8), or
// -1 if the layout is unknown or the CRC doesn't verify (left alone). Centres the stick when change is set.
int32_t handle_report(Device& d, uint8_t* r, DWORD length, bool change) {
    const Layout layout = layout_of(r, length);
    if (!layout.stick) return -1;
    if (layout.crc && stored_crc(r, layout.crc) != report_crc(r, layout.crc)) {
        if (!d.crc_warned.exchange(true))
            log("nativepad: %04X:%04X: the report's CRC isn't the expected one; its reports are left alone", d.vendor, d.product);
        return -1;
    }
    const int32_t stick = int32_t(r[layout.stick]) | int32_t(r[layout.stick + 1]) << 8;
    if (change) {
        r[layout.stick] = r[layout.stick + 1] = 0x80;
        if (layout.crc) {
            const uint32_t crc = report_crc(r, layout.crc);
            for (int k = 0; k < 4; ++k) r[layout.crc + k] = uint8_t(crc >> (8 * k));
        }
        ++g_changed;
    }
    return stick;
}

// What the game just received from controller i (n bytes at r, one or more reports of the device's report length):
// note the newest right stick, and centre it in every report while GyroLib's flick stick asks for that.
void on_report(int i, uint8_t* r, DWORD n, const char* how) {
    Device& d = g_devices[i];
    if (!r || !n) return;
    DWORD length = d.report_length;
    if (!length) length = r[0] == 0x01 && n % 64 == 0 ? 64 : r[0] == 0x31 && n % 78 == 0 ? 78 : n;  // usual sizes
    if (length > n || n % length) length = n;  // not whole reports: treat it as one
    if (!d.logged.exchange(true))
        log("nativepad: the game reads %04X:%04X (%s): %lu bytes (%lu report(s) of %lu), first byte 0x%02X, %s", d.vendor,
            d.product, how, n, n / length, length, r[0], layout_of(r, length).name);
    const bool change =
        g_suppress.load(std::memory_order_relaxed) && (uint32_t(d.vendor) << 16 | d.product) == g_pad_ids.load();
    int32_t newest = -1;
    for (DWORD at = 0; at + length <= n; at += length)
        if (const int32_t stick = handle_report(d, r + at, length, change); stick >= 0) newest = stick;
    if (newest < 0) return;
    d.stick = newest;
    d.last_report = GetTickCount64();
}

// GetOverlappedResult(Ex) said how an overlapped read ended.
void finish(HANDLE h, OVERLAPPED* ol, BOOL ok, DWORD n, DWORD error) {
    Pending p;
    {
        std::lock_guard lock(g_mutex);
        Pending* found = nullptr;
        for (Pending& q : g_pending)
            if (q.ol == ol && q.handle == h) found = &q;
        if (!found || (!ok && error == ERROR_IO_INCOMPLETE)) return;  // not ours, or still in flight
        p = *found;
        *found = {};
        --g_pending_count;
    }
    if (!ok) return;
    if (const int i = find_device(h); i >= 0) on_report(i, p.buffer, std::min(n, p.size), "overlapped reads");
}

// ------------------------------------------------------------------ hooks (any game thread)

// Cheap prefix test first: this runs for every file the game opens.
HANDLE WINAPI hk_create_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE templ) {
    const bool hid = is_hid_path(path);
    uint16_t vendor = 0, product = 0;
    if (hid) {
        vendor = path_id(path, L"vid");
        product = path_id(path, L"pid");
        if (g_refuse && refused_device(vendor, product)) {
            const int n = ++g_refused;
            if (n <= 3)
                log("xinput: XInput=2: the game's access to HID device %04X:%04X refused (it gets the Xbox controller)%s",
                    vendor, product, n == 3 ? "; further refusals not logged" : "");
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (vendor)
            for (auto& slot : g_vendors) {
                uint16_t expected = 0;
                if (slot.load() == vendor || slot.compare_exchange_strong(expected, vendor)) break;
            }
    }
    CreateFileW_t real = original(g_create_file, "CreateFileW", "kernel32.dll");
    HANDLE h = real ? real(path, access, share, sa, disposition, flags, templ) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE || !g_suppression) return h;
    // An enumeration opens each device without access rights; only handles that can read are followed.
    const bool reads = (access & (GENERIC_READ | GENERIC_ALL | MAXIMUM_ALLOWED | FILE_READ_DATA)) != 0;
    if (hid && vendor == kSony && reads) {
        const DWORD error = GetLastError();
        track(h, vendor, product);
        SetLastError(error);
    } else if (g_device_count.load(std::memory_order_relaxed) && find_device(h) >= 0) {
        const DWORD error = GetLastError();
        untrack(h);  // the handle was closed without CloseHandle being seen, and its value is reused
        SetLastError(error);
    }
    return h;
}

BOOL WINAPI hk_read_file(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED ol) {
    ReadFile_t real = original(g_read_file, "ReadFile", "kernel32.dll");
    const BOOL ok = real ? real(h, buffer, size, read, ol) : FALSE;
    if (!g_device_count.load(std::memory_order_relaxed)) return ok;
    const int i = find_device(h);
    if (i < 0) return ok;
    const DWORD error = GetLastError();
    if (ok) {
        const DWORD n = read ? *read : ol ? DWORD(ol->InternalHigh) : 0;
        on_report(i, static_cast<uint8_t*>(buffer), n, ol ? "overlapped reads, done at once" : "synchronous reads");
    } else if (ol && error == ERROR_IO_PENDING) {
        remember(ol, h, buffer, size);
    }
    SetLastError(error);
    return ok;
}

BOOL WINAPI hk_get_overlapped(HANDLE h, LPOVERLAPPED ol, LPDWORD n, BOOL wait) {
    GetOverlappedResult_t real = original(g_get_overlapped, "GetOverlappedResult", "kernel32.dll");
    const BOOL ok = real ? real(h, ol, n, wait) : FALSE;
    if (!g_pending_count.load(std::memory_order_relaxed)) return ok;
    const DWORD error = GetLastError();
    finish(h, ol, ok, ok && n ? *n : 0, error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI hk_get_overlapped_ex(HANDLE h, LPOVERLAPPED ol, LPDWORD n, DWORD timeout, BOOL alertable) {
    GetOverlappedResultEx_t real = original(g_get_overlapped_ex, "GetOverlappedResultEx", "kernel32.dll");
    const BOOL ok = real ? real(h, ol, n, timeout, alertable) : FALSE;
    if (!g_pending_count.load(std::memory_order_relaxed)) return ok;
    const DWORD error = GetLastError();
    finish(h, ol, ok, ok && n ? *n : 0, error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI hk_close_handle(HANDLE h) {
    if (g_device_count.load(std::memory_order_relaxed) && find_device(h) >= 0) untrack(h);
    CloseHandle_t real = original(g_close_handle, "CloseHandle", "kernel32.dll");
    return real ? real(h) : FALSE;
}

}  // namespace

void install(bool suppression) {
    if (hooks::inline_mode()) {
        log("nativepad: not available for this game (protected exe, no import-table hooks)");
        return;
    }
    g_suppression = suppression;
    hooks::add_with_pad_library(nullptr, "CreateFileW", reinterpret_cast<void*>(&hk_create_file), reinterpret_cast<void**>(&g_create_file));
    if (suppression) {
        hooks::add_with_pad_library(nullptr, "ReadFile", reinterpret_cast<void*>(&hk_read_file), reinterpret_cast<void**>(&g_read_file));
        hooks::add_with_pad_library(nullptr, "GetOverlappedResult", reinterpret_cast<void*>(&hk_get_overlapped),
                                    reinterpret_cast<void**>(&g_get_overlapped));
        hooks::add_with_pad_library(nullptr, "GetOverlappedResultEx", reinterpret_cast<void*>(&hk_get_overlapped_ex),
                                    reinterpret_cast<void**>(&g_get_overlapped_ex));
        hooks::add_with_pad_library(nullptr, "CloseHandle", reinterpret_cast<void*>(&hk_close_handle), reinterpret_cast<void**>(&g_close_handle));
    }
    log("settings: NativeFlickStick=%d (%s)", suppression ? 1 : 0,
        suppression ? "flick stick also with the game's own PlayStation controller support, once its reports are checked"
                    : "flick stick only while the controller is shown as an Xbox one");
}

void refuse_pads(bool refuse) { g_refuse = refuse; }

bool opened_vendor(uint16_t vendor) {
    if (!vendor) return false;
    for (auto& v : g_vendors)
        if (v.load() == vendor) return true;
    return false;
}

bool frame(SDL_Gamepad* pad, bool suppress) {
    const uint32_t ids = pad ? uint32_t(SDL_GetGamepadVendor(pad)) << 16 | SDL_GetGamepadProduct(pad) : 0;
    g_pad_ids = ids;

    // The layout is trusted once the right stick in the game's reports agrees with SDL's while the stick is held
    // away from the centre; a clear disagreement turns suppression off for this controller.
    static uint32_t checked = 0;
    static int matches = 0, misses = 0;
    static bool verified = false, mismatch = false, available = false;
    static int transitions = 0;
    if (ids != checked) {
        checked = ids;
        matches = misses = 0;
        verified = mismatch = false;
    }
    bool reading = false;
    if (g_suppression && ids) {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG last = 0;
        int32_t stick = -1;
        for (Device& d : g_devices) {
            if (!d.handle.load(std::memory_order_acquire) || (uint32_t(d.vendor) << 16 | d.product) != ids) continue;
            const ULONGLONG t = d.last_report;
            if (t > last) {
                last = t;
                stick = d.stick;
            }
        }
        reading = last && now - last < 2000;
        if (reading && !verified && !mismatch && stick >= 0 && now - last < 100) {
            const int sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
            if (std::abs(sx) > 12000 || std::abs(sy) > 12000) {
                const int rx = stick & 0xFF, ry = stick >> 8 & 0xFF;
                const int ex = (sx + 32768) / 257, ey = (sy + 32768) / 257;  // SDL: byte * 257 - 32768
                if (std::abs(rx - ex) < 48 && std::abs(ry - ey) < 48) ++matches;
                else ++misses;
                if (matches >= 15 && matches >= 4 * misses) {
                    verified = true;
                    log("nativepad: %04X:%04X: the right stick in the game's reports matches SDL (%d of %d samples)",
                        ids >> 16, ids & 0xFFFF, matches, matches + misses);
                } else if (misses >= 30 && misses > matches) {
                    mismatch = true;
                    log("nativepad: %04X:%04X: the right stick in the game's reports doesn't match SDL (report %d,%d, "
                        "SDL %d,%d; %d of %d samples off): no native suppression for it",
                        ids >> 16, ids & 0xFFFF, rx, ry, ex, ey, misses, matches + misses);
                }
            }
        }
    }
    const bool now_available = reading && verified;
    if (now_available != available) {
        available = now_available;
        log(available ? "nativepad: native right-stick suppression available (flick stick with the game's own support)"
                      : "nativepad: native right-stick suppression unavailable: the game stopped reading the controller");
    }
    const bool want = suppress && available;
    if (want != g_suppress.load()) {
        g_suppress = want;
        if (++transitions <= 10)
            log("nativepad: right stick %s in the game's reports (%u reports changed so far)%s",
                want ? "centred" : "passed through", g_changed.load(), transitions == 10 ? "; further changes not logged" : "");
    }
    return available;
}

}  // namespace gyroue::nativepad
