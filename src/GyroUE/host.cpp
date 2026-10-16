#include "host.hpp"

#include "camera_match.hpp"
#include "deps.hpp"
#include "log.hpp"
#include "longpress.hpp"
#include "nativepad.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "perf.hpp"
#include "profile.hpp"
#include "ue.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.hpp>
#include <gyrolib/version.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <vector>

namespace gyroue {
namespace {

// Sensitivity profiles (ADS, ALT_ADS, OTHER): extra GyroLib views (tabs in the panel, inheriting the default view's
// settings) that replace the default view's settings while their button is held (Hold) or after it is pressed (Toggle).
// Only in the default view: other camera views (vehicles) keep their own tab. The camera rotation is still written
// through the default view. A Toggle profile turns off when gameplay is interrupted (no default view, pause, cursor,
// panel, focus), since GyroUE can't read the game's own aim state. All are off by default: a profile needs both its
// button and its mode in GyroUE.ini; until then its tab is labelled "<label> - disabled" and says how to enable it.
struct Profile {
    uint32_t id = 0;
    const char* key;  // GyroUE.ini prefix: <key>ProfileButton, <key>ProfileMode
    const char* label;
    uint32_t parent;  // settings inheritance (ALT_ADS from ADS)
    int button = -1;  // SDL_GamepadButton, or kLeftTrigger / kRightTrigger; -1 = disabled
    std::string button_name;
    bool registered = false;  // has its tab in the panel
    bool toggle = false;
    bool down = false, on = false;
    uint64_t since = 0;  // activation order: the latest active profile wins
};
constexpr int kLeftTrigger = 100, kRightTrigger = 101;
// ALT_ADS: a second aim (e.g. a scope or an alternative weapon mode) in games that have two.
Profile g_profiles[] = {{1001, "Ads", "ADS", 1}, {1002, "AltAds", "ALT_ADS", 1001}, {1003, "Other", "OTHER", 1}};
uint32_t g_last_profile = 0;  // sensitivity profile reported last frame (0 = none)

std::wstring g_mod_dir, g_profile_path, g_exe_stem;
std::wstring g_settings_path;  // GyroUE\GyroUE.ini (all games)
// The game had a profile when it started; the probe started with Probe=1 writes the profile itself only if it didn't
// (and then keeps updating the one it created this session), else it adds its camera as a view (see finish_tests).
bool g_profile_existed = false, g_profile_created = false;
std::vector<View> g_views;           // read at start and again whenever the profile file changes
FILETIME g_profile_time{};           // the profile's last write time when it was read
std::vector<uint32_t> g_registered;  // view ids registered with GyroLib (tabs in the panel)
// Diagnostics: [GyroUE] Probe=1 captures the camera state automatically on stick turns (see auto_probe) and
// enables F7 (snapshot) / F8 (log what changed since the snapshot) for manual captures.
bool g_probe = false, g_probe_snap_down = false, g_probe_diff_down = false;
// The probe can also start during play, without Probe=1 or a restart: from ProbeKey, or by itself (AutoProbe=1) when the
// camera stops following the gyro. Its verified camera is then added to the profile as a Match=Camera view.
bool g_probe_runtime = false;       // the running probe was started during play
bool g_probe_automatic = false;     // ... by AutoProbe (it gives up after a while without captures)
bool g_auto_probe_setting = true;   // AutoProbe=
int g_auto_probe_starts = 0;        // automatic starts this session (limited)
ULONGLONG g_probe_started_at = 0, g_next_auto_probe = 0;
struct Key {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
    std::string name;
};
Key g_probe_key;  // ProbeKey=
bool g_probe_key_down = false;
// Mouse look (Raw Input reaching the game's window): counts like the right stick for the probe and the camera checks.
ULONGLONG g_mouse_moved_at = 0;

std::optional<gyrolib::Context> g_gyro;
std::optional<gyrolib::SdlInput> g_input;
bool g_failed = false;

ue::Obj g_engine = 0;
ULONGLONG g_next_engine_try = 0, g_next_status = 0, g_next_ini_check = 0;
FILETIME g_ini_time{};
uint32_t g_last_view = UINT32_MAX;
double g_applied_yaw = 0, g_applied_pitch = 0;
bool g_applied_this_frame = false;
bool g_stick_suppression = false;  // GL_HOST_NATIVE_STICK_SUPPRESSION declared (xinput.cpp or nativepad.cpp can do it)
bool g_long_press = false;         // GL_HOST_LONG_PRESS_BLOCKING declared (longpress.cpp, applied by xinput/nativepad)

// Camera recenter (GyroLib's camera.recenter_button, Off until the player binds a button in the panel). Every view
// whose rotation GyroUE can read supports it (level = the view's RecenterPitch, default 0); the callback is withdrawn
// while a view with Recenter=0, or one whose rotation can't be read, is active, which also hides the setting in the
// panel. GyroLib calls the step callback inside gl_update; the fractions are combined and applied after the gyro delta.
// Custom cameras (raw offsets, relative rotators) are best effort: their level may not be 0, see RecenterPitch=.
bool g_recenter_supported = false;
uint32_t g_recenter_view = 0;  // view the support was last decided for
double g_recenter_fraction = 0;  // fraction of the remaining pitch to remove this frame

void GL_CALL on_recenter_step(void*, double fraction) {
    g_recenter_fraction = 1 - (1 - g_recenter_fraction) * (1 - std::clamp(fraction, 0.0, 1.0));
}

// Motion delivery, for the status line. Samples and distinct arrival times come from GyroLib's sample observer; the
// camera updates count how often the gyro turned the camera, and how often it did so without a new sample. The Switch
// Pro over Bluetooth measures a sample every ~5 ms but its reports reach the PC 1-4 at a time with gaps of 3-68 ms;
// since 1.3.1 GyroLib spreads such bursts over the following updates itself ("burst delivery"), which shows here as
// rotation on updates without new samples. (GyroUE 0.6.14-0.6.19 had its own pacer for this, Pacing=1.)
struct Delivery {
    uint64_t samples = 0, arrivals = 0, last_arrival_ns = 0;
    uint64_t updates = 0, turned = 0, spread = 0;  // camera updates; with rotation; with rotation but no new sample
    uint64_t since_ns = 0;

    void on_sample(const gl_sample& sample) {
        ++samples;
        if (sample.arrival_ns != last_arrival_ns) ++arrivals, last_arrival_ns = sample.arrival_ns;
    }
    void on_update(bool camera, uint32_t new_samples, bool rotation) {
        if (!camera) return;
        ++updates;
        if (rotation) ++turned, spread += new_samples == 0;
    }
} g_delivery;

// Motion trace (MotionTrace=1 in GyroUE.ini): 30 s of per-sample and per-frame timing in GyroUE\motion-trace.csv,
// to see where motion gets bunched (controller, transport, frame) and how GyroLib's burst delivery spreads it.
//   S,sensor_ns,arrival_ns,gyro_x,gyro_y,gyro_z         every accepted sample (GyroLib sample observer)
//   F,now_ns,new_samples,raw_yaw,raw_pitch,yaw,pitch     every frame while the gyro may turn the camera (raw = GyroLib
//                                                        output, yaw/pitch = applied; equal since GyroUE has no pacer)
struct Trace {
    bool wanted = false, done = false;
    FILE* file = nullptr;
    uint64_t start_ns = 0;
} g_trace;

void GL_CALL on_sample(void*, uint64_t, const gl_sample* sample) {
    if (!sample) return;
    g_delivery.on_sample(*sample);
    if (g_trace.file)
        std::fprintf(g_trace.file, "S,%llu,%llu,%.3f,%.3f,%.3f\n", (unsigned long long)sample->sensor_ns,
                     (unsigned long long)sample->arrival_ns, sample->gyro_dps.x, sample->gyro_dps.y, sample->gyro_dps.z);
}

void trace_frame(uint64_t now_ns, bool camera, uint32_t new_samples, const gl_output& out, double pitch, double yaw) {
    if (!g_trace.wanted || g_trace.done) return;
    if (!g_trace.file) {
        if (!camera) return;  // start with the first gameplay frame
        const std::wstring path = g_mod_dir + L"\\motion-trace.csv";
        if (_wfopen_s(&g_trace.file, path.c_str(), L"wb") != 0 || !g_trace.file) {
            log("trace: cannot create %s", narrow(path).c_str());
            g_trace.done = true;
            return;
        }
        std::fprintf(g_trace.file, "kind,a,b,c,d,e,f\n");
        g_trace.start_ns = now_ns;
        log("trace: recording 30 s of motion timing to %s", narrow(path).c_str());
    }
    if (camera)
        std::fprintf(g_trace.file, "F,%llu,%u,%.5f,%.5f,%.5f,%.5f\n", (unsigned long long)now_ns, new_samples,
                     out.yaw_degrees, out.pitch_degrees, yaw, pitch);
    if (now_ns - g_trace.start_ns >= 30'000'000'000ull) {
        std::fclose(g_trace.file);
        g_trace.file = nullptr;
        g_trace.done = true;
        log("trace: done (motion-trace.csv)");
    }
}

std::string probe_header() {
    return "GyroUE " GYROUE_VERSION " camera probe (recreated every game session)\n"
           "\n"
           "How to use it (automatic):\n"
           "  1. In gameplay, stand still for a second. The gyro is paused while the probe captures.\n"
           "  2. Turn the camera with the RIGHT STICK or the MOUSE: more than 30 degrees sideways (and a bit up or\n"
           "     down), then stop. GyroUE captures the turn by itself (turns without them, e.g. cutscenes, are ignored).\n"
           "  3. Repeat 3 or 4 times. From the second capture on, the game's profile is written to\n"
           "     GyroUE\\profiles\\<game>.ini (and copied at the end of this file). Once a camera is verified live,\n"
           "     GyroUE sets Probe=0 in GyroUE\\GyroUE.ini by itself: restart the game and the gyro turns the camera.\n"
           "     If the game already had a profile, or the probe was started during play (ProbeKey, or AutoProbe when\n"
           "     a camera stops following the gyro), the verified camera is added to the profile as a new view\n"
           "     (Match=Camera) and used at once, without a restart.\n"
           "  Manual alternative: F7 = snapshot, turn with the stick, F8 = diff (pressing F7/F8 turns the automatic\n"
           "  capture off for the session).\n"
           "\n"
           "How to read it (ADVANCED.md, \"Finding the camera of a new game\"):\n"
           "  rot <path>.<Property>         a reflected rotator that changed  -> profile: Object=<path>, Property=<Property>\n"
           "  raw <path>+0xNN double/float  non-reflected memory that follows the camera -> Object=<path>, Offsets=0xNN\n"
           "  Ignore Camera(...) and transform components: they are the camera's output, not its input (except a\n"
           "  component's RelativeRotation that the suggested profile marks WorldRotation=1).\n"
           "  The path is the part before \"(\"; the class in parentheses is only informative.\n"
           "\n";
}

// GyroUE\GyroUE.ini: settings for every game in this folder, created with the defaults on the first run and never
// overwritten (releases don't ship it, so updates keep the user's values).
const char kDefaultSettings[] =
    "; GyroUE settings for every game in this folder. Created on the first run; edit freely, it is never overwritten.\r\n"
    "; A game's profile (GyroUE\\profiles\\<game exe>.ini) can override any of these in its own [GyroUE] section.\r\n"
    "\r\n"
    "[GyroUE]\r\n"
    "; 1 = find the camera of a game whose camera ignores the gyro: in gameplay, turn the camera with the\r\n"
    ";     RIGHT STICK or MOUSE (more than 30 degrees) and stop, 3-4 times (the gyro is paused meanwhile). GyroUE then\r\n"
    ";     writes the game's profile (GyroUE\\profiles\\<game exe>.ini; to one that exists, it adds the camera as a view).\r\n"
    ";     Once a camera is verified, GyroUE sets this back to 0 by itself. Results: GyroUE\\GyroUE-probe.log.\r\n"
    "Probe=0\r\n"
    "; Learning a camera during the game, without restarting (results also in GyroUE\\GyroUE-probe.log):\r\n"
    ";   ProbeKey: starts the probe right where you are, and cancels it. F1..F24, optionally with Ctrl+, Shift+ or Alt+;\r\n"
    ";     empty = no key.\r\n"
    ";   AutoProbe=1: when the camera stops following the gyro while you turn it yourself, the probe starts by itself.\r\n"
    ";   A camera found this way is added to the game's profile as a new view and used at once. 0 = off.\r\n"
    "ProbeKey=Ctrl+F7\r\n"
    "AutoProbe=1\r\n"
    "; 0 = no on-screen notices (they still go to GyroUE.log).\r\n"
    "Notices=1\r\n"
    "\r\n"
    "; Show a non-Xbox controller (DualSense, Switch Pro, DirectInput pads) to the game as an Xbox (XInput) controller, for\r\n"
    "; games that only support Xbox controllers without Steam Input. 0 = off, 1 = auto (only when the game doesn't read\r\n"
    ";     the controller itself and no Xbox/Steam Input/DS4Windows controller is present), 2 = always (unless the\r\n"
    ";     controller already is an Xbox one). Restart the game after changing it.\r\n"
    "XInput=1\r\n"
    "; Face buttons: 0 = by position (bottom = Xbox A), 1 = swap A<->B and X<->Y (Nintendo A = Xbox A; any controller).\r\n"
    "XInputNintendoLabels=0\r\n"
    "; 1 = the DualSense touchpad click also presses View (Back), like the Create button. 0 = the game ignores it.\r\n"
    "XInputTouchpadBack=1\r\n"
    "\r\n"
    "; 1 = GyroLib's flick stick (F10 panel) also in games with their own PlayStation controller support: GyroUE centres\r\n"
    ";     the right stick in what the game reads while flick stick is on, keeping PlayStation icons, haptics and\r\n"
    ";     adaptive triggers. Offered after you move the right stick once in game. Needs XInput=1 (with XInput=2 the\r\n"
    ";     game doesn't read the controller itself; flick stick then works through the Xbox controller). 0 = off.\r\n"
    "NativeFlickStick=0\r\n"
    "\r\n"
    "; Sensitivity profiles, all off by default: extra tabs in the F10 panel (ADS, ALT_ADS for games with a second aim,\r\n"
    ";     OTHER) that start with the Exploration settings (ALT_ADS: with the ADS settings); change them there. A profile\r\n"
    ";     needs both its button and its mode (until then its tab says \"- disabled\"). Mode: Hold = while the button is\r\n"
    ";     held, Toggle = from one press to the next (also off on pause, menus and cutscenes). Not used in other camera\r\n"
    ";     views (vehicles). Buttons: LT RT LB RB LS RS A B X Y (or L2 R2 L1 R1 L3 R3 Cross Circle Square Triangle),\r\n"
    ";     Back Start Up Down Left Right Touchpad Misc LP1 LP2 RP1 RP2 (paddles). Restart the game after a change.\r\n"
    ";     Example: AdsProfileButton=LT and AdsProfileMode=Hold.\r\n"
    "AdsProfileButton=\r\n"
    "AdsProfileMode=\r\n"
    "AltAdsProfileButton=\r\n"
    "AltAdsProfileMode=\r\n"
    "OtherProfileButton=\r\n"
    "OtherProfileMode=\r\n";

void ensure_settings_file() {
    if (GetFileAttributesW(g_settings_path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_settings_path.c_str(), L"wb") != 0 || !file) {
        log("settings: could not create %s (defaults used)", narrow(g_settings_path).c_str());
        return;
    }
    std::fwrite(kDefaultSettings, 1, sizeof(kDefaultSettings) - 1, file);
    std::fclose(file);
    log("settings: %s created with the defaults", narrow(g_settings_path).c_str());
}

// A [GyroUE] setting: the game's profile wins over GyroUE.ini, then the default.
int setting(const wchar_t* key, int fallback) {
    wchar_t value[32];
    for (const std::wstring* path : {&g_profile_path, &g_settings_path})
        if (GetPrivateProfileStringW(L"GyroUE", key, L"\x01", value, 32, path->c_str()) && value[0] != L'\x01')
            return _wtoi(value);
    return fallback;
}

std::string setting_text(const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[64];
    for (const std::wstring* path : {&g_profile_path, &g_settings_path})
        if (GetPrivateProfileStringW(L"GyroUE", key, L"\x01", value, 64, path->c_str()) && value[0] != L'\x01')
            return narrow(value);
    return narrow(fallback);
}

// A key setting: F1..F24, optionally after Ctrl+, Shift+ and/or Alt+ (case-insensitive). Empty or unknown = no key.
Key parse_key(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    Key k;
    for (const std::string& part : split(text, '+')) {
        if (part == "CTRL" || part == "CONTROL") k.ctrl = true;
        else if (part == "SHIFT") k.shift = true;
        else if (part == "ALT") k.alt = true;
        else if (part.size() >= 2 && part[0] == 'F' && std::atoi(part.c_str() + 1) >= 1 && std::atoi(part.c_str() + 1) <= 24)
            k.vk = VK_F1 + std::atoi(part.c_str() + 1) - 1;
        else return {};
    }
    if (!k.vk) return {};
    k.name = std::string(k.ctrl ? "Ctrl+" : "") + (k.shift ? "Shift+" : "") + (k.alt ? "Alt+" : "") + "F" +
             std::to_string(k.vk - VK_F1 + 1);
    return k;
}

// A profile button name: Xbox or PlayStation names (face buttons by position), case-insensitive. -1 = off/unknown.
int parse_button(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    static const struct {
        const char* name;
        int button;
    } names[] = {
        {"LT", kLeftTrigger}, {"L2", kLeftTrigger}, {"RT", kRightTrigger}, {"R2", kRightTrigger},
        {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"A", SDL_GAMEPAD_BUTTON_SOUTH}, {"CROSS", SDL_GAMEPAD_BUTTON_SOUTH},
        {"B", SDL_GAMEPAD_BUTTON_EAST}, {"CIRCLE", SDL_GAMEPAD_BUTTON_EAST},
        {"X", SDL_GAMEPAD_BUTTON_WEST}, {"SQUARE", SDL_GAMEPAD_BUTTON_WEST},
        {"Y", SDL_GAMEPAD_BUTTON_NORTH}, {"TRIANGLE", SDL_GAMEPAD_BUTTON_NORTH},
        {"BACK", SDL_GAMEPAD_BUTTON_BACK}, {"VIEW", SDL_GAMEPAD_BUTTON_BACK}, {"CREATE", SDL_GAMEPAD_BUTTON_BACK},
        {"START", SDL_GAMEPAD_BUTTON_START}, {"MENU", SDL_GAMEPAD_BUTTON_START}, {"OPTIONS", SDL_GAMEPAD_BUTTON_START},
        {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"TOUCHPAD", SDL_GAMEPAD_BUTTON_TOUCHPAD}, {"MISC", SDL_GAMEPAD_BUTTON_MISC1},
        {"RP1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"LP1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
        {"RP2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"LP2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    };
    for (const auto& n : names)
        if (name == n.name) return n.button;
    return -1;
}

void load_sensitivity_profiles() {
    for (Profile& p : g_profiles) {
        const std::wstring key(p.key, p.key + std::strlen(p.key));
        const std::string button = setting_text((key + L"ProfileButton").c_str(), L"");
        std::string mode = setting_text((key + L"ProfileMode").c_str(), L"");
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool mode_ok = mode == "hold" || mode == "toggle";
        p.button = mode_ok ? parse_button(button) : -1;
        p.button_name = button;
        p.toggle = mode == "toggle";
        if (!button.empty() && parse_button(button) < 0)
            log("settings: %sProfileButton=%s not recognised", p.key, button.c_str());
        if (!mode.empty() && !mode_ok) log("settings: %sProfileMode=%s not recognised (Hold or Toggle)", p.key, mode.c_str());
        if (p.button < 0) log("settings: profile '%s' (view %u) disabled: needs %sProfileButton= and %sProfileMode=", p.label, p.id, p.key, p.key);
        else log("settings: profile '%s' (view %u) button=%s mode=%s", p.label, p.id, button.c_str(), p.toggle ? "Toggle" : "Hold");
    }
}

bool profile_button_down(SDL_Gamepad* pad, int button) {
    if (button == kLeftTrigger || button == kRightTrigger)
        return SDL_GetGamepadAxis(pad, button == kLeftTrigger ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    return SDL_GetGamepadButton(pad, SDL_GamepadButton(button));
}

// Updates the profiles from the selected controller after gl_update: the result is reported on the next frame.
// gameplay = the default view may turn the camera now.
void update_sensitivity_profiles(SDL_Gamepad* pad, bool gameplay) {
    static uint64_t order = 0;
    for (Profile& p : g_profiles) {
        const bool down = gameplay && pad && p.button >= 0 && profile_button_down(pad, p.button);
        const bool pressed = down && !p.down;
        p.down = down;
        if (!gameplay) p.on = false;
        else if (!p.toggle) p.on = down;
        else if (pressed) p.on = !p.on;
        if (pressed && p.on) {
            p.since = ++order;
            if (p.toggle)  // a newly toggled profile replaces the other toggled one
                for (Profile& other : g_profiles)
                    if (&other != &p && other.toggle) other.on = false;
        }
    }
}

uint32_t sensitivity_profile() {
    const Profile* best = nullptr;
    for (const Profile& p : g_profiles)
        if (p.on && (!best || p.since > best->since)) best = &p;
    return best ? best->id : 0;
}

// The game's profile describes camera views (a profile can also hold only [GyroUE] settings, e.g. the DualSense
// learning below): the probe then writes its result as a suggestion instead.
// GyroUE-probe.log, opened when the probe first runs this session (at start with Probe=1, or later from the probe key
// or the automatic start).
bool open_probe_log() {
    static bool opened = false, tried = false;
    if (tried) return opened;
    tried = true;
    const std::wstring probe_path = g_mod_dir + L"\\GyroUE-probe.log";
    opened = probe_log_open(probe_path, probe_header());
    log("probe: results in %s", opened ? narrow(probe_path).c_str() : "GyroUE.log (probe file failed)");
    return opened;
}

FILETIME profile_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(g_profile_path.c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime : FILETIME{};
}

void load_profile() {
    ensure_settings_file();
    g_profile_existed = has_views(g_profile_path);
    g_probe = setting(L"Probe", 0) != 0;
    g_auto_probe_setting = setting(L"AutoProbe", 1) != 0;
    g_probe_key = parse_key(setting_text(L"ProbeKey", L"Ctrl+F7"));
    g_trace.wanted = setting(L"MotionTrace", 0) != 0;
    if (g_trace.wanted) log("settings: MotionTrace=1 (30 s from the first gameplay frame)");
    // On-screen notices (camera check, probe progress); Notices=0 keeps them in the log only.
    const bool notices = setting(L"Notices", 1) != 0;
    log("settings: Probe=%d AutoProbe=%d ProbeKey=%s Notices=%d (GyroUE.ini, the game's profile overrides)", g_probe ? 1 : 0,
        g_auto_probe_setting ? 1 : 0, g_probe_key.name.empty() ? "(none)" : g_probe_key.name.c_str(), notices ? 1 : 0);
    notify::set_enabled(notices);
    load_sensitivity_profiles();
    if (g_probe) {
        open_probe_log();
        log("profile: probe enabled (automatic capture on stick or mouse turns, F7/F8 manual)");
    }
    g_views = load_views(g_profile_path);
    g_profile_time = profile_stamp();
}

// Gives every profile view its GyroLib view (a tab in the panel, inheriting its parent's settings) and withdraws the
// ones the profile no longer has: at start, and after the profile file changed during play.
void register_views() {
    gl_context* ctx = g_gyro->get();
    for (const View& v : g_views) {
        if (std::find(g_registered.begin(), g_registered.end(), v.id) != g_registered.end()) continue;
        try {
            g_gyro->register_view(v.id, v.label.c_str());
        } catch (const std::exception& e) {
            log("profile: view %u '%s' not registered with GyroLib: %s", v.id, v.label.c_str(), e.what());
            continue;
        }
        if (v.parent) gl_set_context_parent(ctx, v.id, v.parent);
        g_registered.push_back(v.id);
    }
    for (auto it = g_registered.begin(); it != g_registered.end();) {
        const uint32_t id = *it;
        if (id == 1 || std::any_of(g_views.begin(), g_views.end(), [&](const View& v) { return v.id == id; })) {
            ++it;
            continue;
        }
        gl_unregister_gameplay_context(ctx, id);
        log("profile: view %u withdrawn (no longer in the profile)", id);
        it = g_registered.erase(it);
    }
}

// The profile file changed (edited by hand, or a view added by the probe): read its views again and use them at once.
void check_profile_file() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    const FILETIME stamp = profile_stamp();
    if (CompareFileTime(&stamp, &g_profile_time) == 0) return;
    g_profile_time = stamp;
    log("profile: the profile file changed, views read again");
    g_views = load_views(g_profile_path);
    // A new view can't take a sensitivity profile's id once that profile has its tab (at start the view wins).
    std::erase_if(g_views, [](const View& v) {
        const bool taken = std::any_of(std::begin(g_profiles), std::end(g_profiles),
                                       [&](const Profile& p) { return p.id == v.id && p.registered; });
        if (taken) log("profile: view %s ignored, Id=%u belongs to a sensitivity profile", v.key.c_str(), v.id);
        return taken;
    });
    register_views();
    g_last_view = UINT32_MAX;  // log the active view again
    g_recenter_view = 0;       // decide recenter support again
}

bool start_gyro() {
    const std::wstring dll = g_mod_dir + L"\\gyrolib.dll";
    if (!deps::check_gyrolib(dll)) return false;  // the game's own runtime DLLs would crash GyroLib
    if (!LoadLibraryW(dll.c_str())) {
        log("gyro: cannot load %s (error %lu)", narrow(dll).c_str(), GetLastError());
        return false;
    }
    log("gyro: GyroLib ABI %u (built against SDK %s)", gl_abi_version(), GL_VERSION_STRING);
    g_gyro.emplace();
    gl_context* ctx = g_gyro->get();
    register_views();
    // Every profile gets its tab, so the player sees it exists; a disabled one says how to enable it.
    for (Profile& p : g_profiles) {
        if (std::any_of(g_views.begin(), g_views.end(), [&](const View& v) { return v.id == p.id; })) {
            log("profile: view Id=%u is reserved for the '%s' sensitivity profile, profile off", p.id, p.label);
            p.button = -1;
            continue;
        }
        const std::string label = p.button < 0 ? std::string(p.label) + " - disabled" : p.label;
        const std::string description =
            p.button < 0 ? std::format("Disabled. To use it, set {0}ProfileButton= (e.g. LT) and {0}ProfileMode= (Hold or "
                                       "Toggle) in GyroUE\\GyroUE.ini, then restart the game.", p.key)
            : p.toggle   ? std::format("Pressing {} turns these settings on, pressing it again turns them off.", p.button_name)
                         : std::format("These settings are used while {} is held.", p.button_name);
        g_gyro->register_view(p.id, label.c_str(), GL_OUTPUT_CAMERA, 0, description.c_str());
        p.registered = true;
        const bool parent_registered = std::any_of(std::begin(g_profiles), std::end(g_profiles),
                                                   [&](const Profile& o) { return o.id == p.parent && o.registered; });
        gl_set_context_parent(ctx, p.id, parent_registered ? p.parent : 1);
    }
    gl_set_host_capabilities(ctx, GL_HOST_MENU_STATE);

    // Mod defaults (an existing gyrolib.ini overrides them): always on, calibrate in menus, no flick stick.
    gl_setting_set(ctx, "gyro.activation", 0);
    gl_setting_set(ctx, "calibration.automatic", 1);
    gl_setting_set(ctx, "flick.mode", 0);
    const std::string dir = narrow(g_mod_dir);
    g_gyro->initialize_settings(dir.c_str());
    log("gyro: settings %s (menu key F%u, gamepad shortcut %u)", gl_get_settings_path(ctx), gl_get_menu_key(ctx),
        gl_get_gamepad_menu_shortcut(ctx));

    // Recenter is offered from the start, so the panel shows it in the main menu too; a gameplay view that can't
    // support it withdraws it (update_recenter_support).
    gl_set_recenter_step_callback(ctx, &on_recenter_step, nullptr);
    g_recenter_supported = true;

    g_input.emplace(*g_gyro);
    gl_set_sample_observer(ctx, &on_sample, nullptr);  // delivery status (and the motion trace) need every sample
    log("gyro: SDL reader started");
    return true;
}

void check_settings_file() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_ini_check) return;
    g_next_ini_check = now + 1000;
    const char* path = gl_get_settings_path(g_gyro->get());
    if (!path || !*path) return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return;
    if (g_ini_time.dwLowDateTime == 0 && g_ini_time.dwHighDateTime == 0) {
        g_ini_time = data.ftLastWriteTime;
        return;
    }
    if (CompareFileTime(&data.ftLastWriteTime, &g_ini_time) != 0) {
        g_ini_time = data.ftLastWriteTime;
        const int32_t r = gl_load_settings(g_gyro->get(), path);
        log("gyro: gyrolib.ini changed, reloaded (result %d)", r);
    }
}

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Everything the frame needs from the engine; any missing link leaves the rest at 0.
struct Scene {
    ue::Obj pc = 0, pawn = 0, view_target = 0, camera = 0;
    bool paused = false, cursor = false;
    bool pov_ok = false;  // the camera's final rotation (PlayerCameraManager POV) was read
    double pov_pitch = 0, pov_yaw = 0;
    bool loc_ok = false;  // ... and its location (probe only)
    double pov_loc[3] = {};
};

Scene read_scene() {
    Scene s;
    if (!ue::init()) return s;
    if (!g_engine) {
        const ULONGLONG now = GetTickCount64();
        if (now < g_next_engine_try) return s;
        g_next_engine_try = now + 1000;
        g_engine = ue::find_instance("GameEngine");
        if (!g_engine) return s;
        log("ue: engine %s (%s)", ue::name_of(g_engine).c_str(), ue::name_of(ue::class_of(g_engine)).c_str());
    }
    const ue::Obj viewport = ue::read_obj(g_engine, "GameViewport");
    const ue::Obj world = ue::read_obj(viewport, "World");
    const ue::Obj instance = ue::read_obj(world, "OwningGameInstance");
    const ue::Prop players = ue::prop_of(instance, "LocalPlayers");
    if (!players) return s;
    uintptr_t data = 0;
    int32_t count = 0;
    if (!ue::read_ptr(instance + players.offset, data) || !ue::read_i32(instance + players.offset + 8, count) || count < 1)
        return s;
    uintptr_t local_player = 0;
    if (!ue::read_ptr(data, local_player)) return s;
    s.pc = ue::read_obj(local_player, "PlayerController");
    if (!s.pc) return s;
    s.pawn = ue::read_obj(s.pc, "Pawn");
    s.cursor = ue::read_bool(s.pc, "bShowMouseCursor", false);

    const ue::Obj camera = ue::read_obj(s.pc, "PlayerCameraManager");
    s.camera = camera;
    // Read every frame: the follow check keeps watching after its verdict (offsets cached, a few guarded reads).
    s.pov_ok = ue::camera_pov(camera, s.pov_pitch, s.pov_yaw);
    s.loc_ok = g_probe && s.pov_ok && ue::camera_pov_location(camera, s.pov_loc);
    const ue::Prop view_target = ue::prop_of(camera, "ViewTarget");
    const ue::Prop target = ue::find_prop(ue::struct_type(view_target), "Target");
    if (view_target && target) ue::read_ptr(camera + view_target.offset + target.offset, s.view_target);

    const ue::Obj level = ue::read_obj(world, "PersistentLevel");
    const ue::Obj settings = ue::read_obj(level, "WorldSettings");
    // UWorld::IsPaused(): WorldSettings has a pauser (PauserPlayerState in 4.2x/5.x, Pauser in older engines).
    s.paused = ue::prop_of(settings, "PauserPlayerState") ? ue::read_obj(settings, "PauserPlayerState") != 0
                                                          : ue::read_obj(settings, "Pauser") != 0;
    return s;
}

View* find_view(uint32_t id) {
    for (View& v : g_views)
        if (v.id == id) return &v;
    return nullptr;
}

// The view for the camera's view target: a ViewTargetClass view (not Match=Camera), else the default one.
View* base_view(const Scene& s) {
    if (s.view_target) {
        for (View& v : g_views)
            if (!v.match_camera && !v.view_target_class.empty() && ue::is_a(s.view_target, v.view_target_class.c_str()))
                return &v;
    }
    // Main menus usually possess a placeholder DefaultPawn (SpectatorPawn derives from it): no gameplay camera.
    if (!s.pawn || ue::is_a(s.pawn, "DefaultPawn")) return nullptr;
    return &g_views.front();
}

CameraMatch g_match;

// The view the gyro turns: the base view, or the Match=Camera view for the same view target that the camera follows
// now (camera_match.hpp).
View* active_view(const Scene& s, View* base) {
    if (!base) return nullptr;
    std::vector<uint32_t> members = {base->id};
    for (const View& v : g_views)
        if (v.match_camera && (v.view_target_class.empty() || ue::is_a(s.view_target, v.view_target_class.c_str())))
            members.push_back(v.id);
    View* view = find_view(g_match.select(members));
    return view ? view : base;
}

// Subobject lookups need a full object scan: cache them and re-check the cached object cheaply.
struct SubCache {
    ue::Obj outer = 0, found = 0;
    std::string name;
    ULONGLONG retry = 0;
};
std::vector<SubCache> g_sub_cache;

// One path step: an object property of outer, else a subobject of outer by name, or by class with "@Class"
// (first subobject whose class chain has it: for instance names that change every session).
ue::Obj resolve_part(ue::Obj outer, const std::string& name) {
    const bool by_class = !name.empty() && name.front() == '@';
    const std::string key = by_class ? name.substr(1) : name;
    if (!by_class)
        if (const ue::Obj o = ue::read_obj(outer, name.c_str())) return o;
    auto find = [&] { return by_class ? ue::find_subobject_of_class(outer, key.c_str()) : ue::find_subobject(outer, key.c_str()); };
    auto still_valid = [&](ue::Obj o) {
        return o && ue::outer_of(o) == outer && (by_class ? ue::is_a(o, key.c_str()) : ue::name_of(o) == key);
    };
    for (SubCache& c : g_sub_cache) {
        if (c.outer != outer || c.name != name) continue;
        if (still_valid(c.found)) return c.found;
        if (GetTickCount64() < c.retry) return 0;
        c.found = find();
        c.retry = GetTickCount64() + 2000;
        return c.found;
    }
    if (g_sub_cache.size() > 32) g_sub_cache.clear();
    g_sub_cache.push_back({outer, find(), name, GetTickCount64() + 2000});
    return g_sub_cache.back().found;
}

// A profile object path: Root (PC, Pawn, Camera, ViewTarget) then steps (see resolve_part).
ue::Obj resolve_path(const std::vector<std::string>& path, const Scene& s) {
    if (path.empty()) return 0;
    const std::string& root = path.front();
    ue::Obj o = root == "PC"     ? s.pc
              : root == "Pawn"   ? s.pawn
              : root == "Camera" ? ue::read_obj(s.pc, "PlayerCameraManager")
                                 : s.view_target;
    for (size_t i = 1; i < path.size() && o; ++i) o = resolve_part(o, path[i]);
    return o;
}

ue::Obj view_object(const View& view, const Scene& s) {
    if (view.object.empty()) return view.id == 1 ? s.pc : s.view_target;
    return resolve_path(view.object, s);
}

bool apply(View& view, const Scene& s, double pitch, double yaw) {
    const ue::Obj target = view_object(view, s);
    if (!target) return false;
    if (!view.offsets.empty()) {
        bool ok = true;
        for (int32_t off : view.offsets) ok &= ue::add_rotator(target + off, view.rotator_size, pitch, yaw);
        if (!ok) return false;
    } else {
        const ue::Prop p = ue::prop_of(target, view.property.empty() ? "ControlRotation" : view.property.c_str());
        if (!p) return false;
        if (view.world_rotation) {
            const char* why = nullptr;
            if (!ue::add_world_rotation(target, p, pitch, yaw, why)) {
                if (why != view.world_why)
                    log("profile: view %u WorldRotation not applied to %s: %s", view.id, ue::name_of(target).c_str(), why);
                view.world_why = why;
                return false;
            }
        } else if (!ue::add_rotator(target + p.offset, p.element_size, pitch, yaw)) {
            return false;
        }
    }
    for (View::OnGyro& w : view.on_gyro) {
        const ue::Obj o = w.object.empty() ? target : resolve_path(w.object, s);
        if ((!o || !ue::write_number(o, w.property.c_str(), w.value)) && !w.warned) {
            w.warned = true;
            std::string path;
            for (const std::string& part : w.object) path += part + ".";
            log("profile: OnGyro %s%s not written (%s)", path.c_str(), w.property.c_str(),
                o ? "property missing or not a number" : "object not found");
        }
    }
    return true;
}

void diagnostics(View& view, const Scene& s) {
    const ue::Obj target = view_object(view, s);
    if (!target) return;
    if (view.dump && !view.dumped) {
        view.dumped = true;
        log("dump: properties of %s (%s)", ue::name_of(target).c_str(), ue::name_of(ue::class_of(target)).c_str());
        ue::dump_props(target, "Actor");
    }
    for (size_t i = 0; i < view.watch.size(); ++i) {
        std::string value = ue::value_string(target, view.watch[i].c_str());
        if (value != view.watched_values[i]) {
            log("watch: %s.%s = %s", view.label.c_str(), view.watch[i].c_str(), value.empty() ? "?" : value.c_str());
            view.watched_values[i] = std::move(value);
        }
    }
}

double wrap180(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180) a -= 360;
    if (a < -180) a += 360;
    return a;
}

// The pitch of the rotator the gyro turns in this view (the first one for raw offsets, which hold the same value).
// For WorldRotation=1 views it is the absolute RelativeRotation, which equals the world rotation.
bool read_view_pitch(const View& view, const Scene& s, double& pitch) {
    const ue::Obj target = view_object(view, s);
    if (!target) return false;
    double yaw = 0, roll = 0;
    if (!view.offsets.empty()) return ue::read_rotator(target + view.offsets.front(), view.rotator_size, pitch, yaw, roll);
    const ue::Prop p = ue::prop_of(target, view.property.empty() ? "ControlRotation" : view.property.c_str());
    return p && ue::read_rotator(target + p.offset, p.element_size, pitch, yaw, roll);
}

// Offers (or withdraws) the recenter button for the active view: decided when the view changes, and retried while the
// view's rotation can't be read yet (no withdrawal on a transient read failure, which would cancel a transition).
// It starts offered (start_gyro). No gameplay view keeps the current state: GyroLib discards requests in menus anyway,
// and withdrawing there would hide the setting in the panel opened from a menu.
void update_recenter_support(const View* view, const Scene& s) {
    if (!view) return;
    if (view->id == g_recenter_view && (g_recenter_supported || !view->recenter)) return;
    double pitch = 0;
    const bool supported = view->recenter && read_view_pitch(*view, s, pitch);
    const bool changed = supported != g_recenter_supported || view->id != g_recenter_view;
    g_recenter_view = view->id;
    if (!changed) return;
    if (supported != g_recenter_supported)
        gl_set_recenter_step_callback(g_gyro->get(), supported ? &on_recenter_step : nullptr, nullptr);
    g_recenter_supported = supported;
    // Match=Camera views take turns with their base view: each view's support is logged once, then only changes.
    static std::vector<std::pair<uint32_t, bool>> logged;
    const std::pair<uint32_t, bool> entry{view->id, supported};
    if (std::find(logged.begin(), logged.end(), entry) != logged.end()) return;
    logged.push_back(entry);
    if (supported) log("recenter: available in view %u '%s' (level pitch %.1f)", view->id, view->label.c_str(), view->recenter_pitch);
    else log("recenter: unavailable in view %u '%s' (%s)", view->id, view->label.c_str(),
             view->recenter ? "its rotation can't be read yet" : "Recenter=0 in the profile");
}

// Removes `fraction` of the remaining pitch to the view's level, through the same writes as the gyro (all raw offsets,
// WorldRotation, OnGyro). UE keeps pitch as 0..360 (350 = 10 degrees up), hence the wrap.
bool apply_recenter(View& view, const Scene& s, double fraction) {
    double pitch = 0;
    if (!read_view_pitch(view, s, pitch)) return false;
    const double delta = wrap180(view.recenter_pitch - pitch) * fraction;
    return delta == 0 || apply(view, s, delta, 0);
}

void take_snapshot(const Scene& s) {
    probe_log("probe: roots pc=%s pawn=%s camera=%s target=%s", ue::name_of(s.pc).c_str(), ue::name_of(s.pawn).c_str(),
              ue::name_of(s.camera).c_str(), ue::name_of(s.view_target).c_str());
    ue::probe_snapshot({{"PC", s.pc}, {"Pawn", s.pawn}, {"Camera", s.camera}, {"ViewTarget", s.view_target}});
}

// Writes the probe's result: the game's profile itself only when the live test verified it and the game had no
// profile when it started (or the probe created it this session); otherwise <exe>.suggested.ini next to it. An
// unverified guess never becomes the profile. Returns where it went (Nothing = no result yet).
// The game's own PlayStation support closed the DualSense unread (it only knows the DualShock 4): remember it in the
// game's profile, so from the next start GyroUE shows the DualSense to it as a DualShock 4 (PlayStation prompts, rumble,
// light bar). Until then the auto mode shows it as an Xbox controller. Once per session.
void learn_dualsense() {
    static bool done = false;
    if (done || !nativepad::dualsense_ignored() || !xinput::sony_library()) return;
    done = true;
    CreateDirectoryW((g_mod_dir + L"\\profiles").c_str(), nullptr);
    const bool created = GetFileAttributesW(g_profile_path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (created) {
        const std::string header = "; GyroUE profile for " + narrow(g_exe_stem) + ", written by GyroUE.\r\n"
                                   "; The game's own PlayStation support only knows the DualShock 4: the DualSense is shown to it as one.\r\n\r\n";
        if (FILE* file = nullptr; _wfopen_s(&file, g_profile_path.c_str(), L"wb") == 0 && file) {
            std::fwrite(header.data(), 1, header.size(), file);
            std::fclose(file);
        }
    }
    const bool ok = WritePrivateProfileStringW(L"GyroUE", L"DualSenseAsDualShock4", L"1", g_profile_path.c_str()) != 0;
    log("dualsense: the game's own PlayStation support closed the DualSense unread (it only knows the DualShock 4): %s %s",
        ok ? (created ? "profile created," : "profile updated,") : "could not write", narrow(g_profile_path).c_str());
    if (ok)
        notify::show(L"This game's PlayStation support doesn't know the DualSense: it works as an Xbox controller for now.\n"
                     L"Restart the game to use it as a PlayStation controller (DualShock 4).", 15);
}

enum class Written { Nothing, Profile, Suggested };

// The probe's verified camera becomes a new Match=Camera view in the existing (or new) profile, rather than the
// profile's default view: when it was started during play (that camera is an extra one, the default view works
// elsewhere), or when the game already has a profile (never overwritten).
bool probe_adds_view() { return g_probe_runtime || g_profile_existed; }

Written write_suggestion(bool& usable, bool& verified) {
    usable = verified = false;
    std::string summary;
    const std::string probe = ue::probe_suggestion(narrow(g_exe_stem), summary, false, verified);
    if (probe.empty()) return Written::Nothing;
    usable = probe.find("[View.Default]") != std::string::npos;
    // A probe started during play, or in a game that already has a profile, adds its camera as a view instead.
    const bool as_profile = usable && verified && !probe_adds_view() && (!g_profile_existed || g_profile_created);
    const std::string text = as_profile ? ue::probe_suggestion(narrow(g_exe_stem), summary, true, verified) : probe;
    const std::wstring path = g_mod_dir + L"\\profiles\\" + g_exe_stem + (as_profile ? L".ini" : L".suggested.ini");
    // The profile's [GyroUE] settings (e.g. DualSenseAsDualShock4, learned earlier) survive the probe rewriting it.
    std::wstring kept(4096, L'\0');
    kept.resize(as_profile ? GetPrivateProfileSectionW(L"GyroUE", kept.data(), DWORD(kept.size()), path.c_str()) : 0);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0) file = nullptr;
    const bool written = file && std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (file) std::fclose(file);
    if (written && !kept.empty()) WritePrivateProfileSectionW(L"GyroUE", kept.c_str(), path.c_str());
    if (written && as_profile) g_profile_created = true;
    const char* what = as_profile ? "game profile" : "suggested profile";
    probe_log("probe: %s (%s)%s:\n%s", what, summary.c_str(),
              written ? (" written to " + narrow(path)).c_str() : " (could not write the file)", text.c_str());
    log("probe: %s: %s -> %s", what, summary.c_str(), written ? narrow(path).c_str() : "write failed, see GyroUE-probe.log");
    if (!written) return Written::Nothing;
    return as_profile ? Written::Profile : Written::Suggested;
}

// On-screen result of a manual diff (F8): no live test there, so only an unverified suggestion is written.
void notify_manual_diff() {
    bool usable = false, verified = false;
    const Written written = write_suggestion(usable, verified);
    std::wstring text = L"GyroUE probe: diff done.\n";
    if (written == Written::Nothing)
        text += L"Turn the camera with the right stick, then F7/F8 again (two diffs are needed).";
    else if (usable)
        text += L"Unverified guess written to GyroUE\\profiles\\" + g_exe_stem +
                L".suggested.ini\nFor a verified profile, use the automatic probe (Probe=1 without F7/F8).";
    else
        text += L"No camera rotation found yet (see GyroUE-probe.log).";
    notify::show(text, written == Written::Nothing ? 10 : 20);
}

// Automatic probe (Probe=1), guided by on-screen notices:
// 1. Captures: a snapshot once the camera rests, a scored diff when the player has turned it more than 30 degrees
//    with the right stick and stopped again. Every rotation that followed the camera becomes a candidate.
// 2. Live test, after kCapturesForTest captures, while the player doesn't touch the controller: each candidate
//    gets small turns written into it, exactly as a profile would write them. It passes only if the camera turns by
//    that amount and comes back when the turn is undone, and again in a second round. An output (the game rewrites
//    it every frame) or a copy the camera doesn't read fails, which is what the captures alone can't tell apart.
// 3. A verified rotation becomes the game's profile (or <exe>.suggested.ini). Without one: more captures, then the
//    new candidates are tested; after kMaxAutoCaptures only an unverified guess is written, never as the profile.
// Off as soon as F7/F8 are used.
struct AutoProbe {
    bool enabled = true, armed = false, announced = false, have_last = false;
    bool done = false;            // finished: verified, or out of captures
    bool stick_turn = false;      // the right stick was deflected since the snapshot
    bool stick_data = false;      // GyroLib has reported the right stick at least once (else no stick filter)
    double snap_pitch = 0, snap_yaw = 0, last_pitch = 0, last_yaw = 0;
    ue::Obj pawn = 0;
    ue::Obj target_class = 0;     // view target class of the captures: the live test runs on the same camera
    ULONGLONG still_since = 0, snapshot_at = 0, next_snapshot = 0;
    int captures = 0;
    // How far the camera moves when the stick turns it: a third-person camera orbits around the character (its
    // location moves on a circle of this radius), a first-person one turns on the spot (radius about 0). Measured
    // in every capture; the live test checks that a candidate's turn moves the camera the same way.
    bool snap_loc_ok = false;
    double snap_loc[3] = {};
    std::vector<double> radii;
} g_auto;
constexpr int kMaxAutoCaptures = 8, kCapturesForTest = 3, kMaxTests = 40;
constexpr double kTestTurn = 3.0, kTestTolerance = 0.5;
constexpr double kOrbitMin = 30;  // world units: below this the camera turns on the spot (first person), no orbit check
constexpr double kDegToRad = 3.14159265358979323846 / 180;

// Angle in degrees between the view directions of two rotations.
double view_angle(double p0, double y0, double p1, double y1) {
    const double r = kDegToRad;
    const double a[3] = {std::cos(p0 * r) * std::cos(y0 * r), std::cos(p0 * r) * std::sin(y0 * r), std::sin(p0 * r)};
    const double b[3] = {std::cos(p1 * r) * std::cos(y1 * r), std::cos(p1 * r) * std::sin(y1 * r), std::sin(p1 * r)};
    return std::acos(std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0)) / r;
}

double distance(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The camera's orbit radius from the captures (median), 0 if unknown.
double orbit_radius() {
    if (g_auto.radii.empty()) return 0;
    std::vector<double> r = g_auto.radii;
    std::sort(r.begin(), r.end());
    return r[r.size() / 2];
}

// The right stick is deflected (GyroLib's view of the selected controller, SDL or a bound companion).
bool right_stick_moving() {
    gl_flick_input fi{};
    if (gl_get_flick_input(g_gyro->get(), &fi) != GL_OK || !(fi.available & GL_FLICK_INPUT_STICK)) return false;
    g_auto.stick_data = true;
    return std::hypot(fi.stick_x, fi.stick_y) > 0.3f;
}

bool mouse_moving() { return GetTickCount64() - g_mouse_moved_at < 150; }

// The player turns the camera themselves: right stick or mouse.
bool look_input() { return right_stick_moving() || mouse_moving(); }

struct LiveTest {
    enum Phase { Off, Settle, Null, YawOut, YawBack, PitchOut, PitchBack } phase = Off;
    std::vector<ue::ProbeGroup> queue;
    size_t index = 0;
    int round = 1, attempts = 0;
    std::vector<std::string> passed;  // ids that moved the camera in round 1 (tested again in round 2)
    View view;                        // the candidate, as a profile view
    double base_p = 0, base_y = 0;    // camera before the current turn
    double val_y = 0, pawn_yaw = 0, delta_p = 0;
    double written_p = 0, written_y = 0;  // turn currently written into the candidate (undone on abort)
    bool base_loc_ok = false;
    double base_loc[3] = {};
    std::string orbit_note;
    bool yaw_ok = false, pitch_ok = false, turns_pawn = false;
    ULONGLONG phase_at = 0, ok_since = 0, last_pause_notice = 0;
} g_test;

// While the automatic probe runs, the gyro output is paused: a turn must come from the right stick alone, and the
// gyro written into a rotation the camera ignores (or a resting controller's drift) would only spoil it.
// An automatic probe (AutoProbe) keeps the gyro on until its live test: the camera ignores it anyway, a turn without the
// stick or mouse isn't captured, and the follow check must see it when the camera follows the gyro again.
bool probe_capturing() {
    return g_probe && g_auto.enabled && !g_auto.done && (!g_probe_automatic || g_test.phase != LiveTest::Off);
}

View test_view(const ue::ProbeGroup& g) {
    View v;
    v.label = "probe test";
    v.object = split(g.object, '.');
    v.property = g.property;
    v.offsets = g.offsets;
    v.rotator_size = g.size ? g.size : 24;
    v.world_rotation = g.world;
    return v;
}

// The candidate's current rotation (its first rotator for raw groups).
bool read_view_rotation(const View& v, const Scene& s, double& pitch, double& yaw) {
    const ue::Obj target = view_object(v, s);
    if (!target) return false;
    double roll = 0;
    if (!v.offsets.empty()) return ue::read_rotator(target + v.offsets.front(), v.rotator_size, pitch, yaw, roll);
    const ue::Prop p = ue::prop_of(target, v.property.empty() ? "ControlRotation" : v.property.c_str());
    return p && ue::read_rotator(target + p.offset, p.element_size, pitch, yaw, roll);
}

// The pawn's own rotation (root component), NaN if unknown: a candidate that turns the character is noted.
double pawn_yaw(const Scene& s) {
    const ue::Obj root = ue::read_obj(s.pawn, "RootComponent");
    const ue::Prop p = ue::prop_of(root, "RelativeRotation");
    double pitch = 0, yaw = 0, roll = 0;
    return p && ue::read_rotator(root + p.offset, p.element_size, pitch, yaw, roll) ? yaw : std::nan("");
}

std::string group_text(const ue::ProbeGroup& g) {
    std::string s = g.object;
    if (!g.property.empty()) return s + "." + g.property;
    s += " raw";
    for (size_t i = 0; i < g.offsets.size(); ++i) s += std::format("{}0x{:X}", i ? "," : " ", g.offsets[i]);
    return s;
}

void finish_tests();

void next_candidate() {
    ++g_test.index;
    g_test.attempts = 0;
    g_test.phase = LiveTest::Settle;  // a turn still written (undo failed) is undone there, before the next test
    if (g_test.index < g_test.queue.size()) {
        notify::show(std::format(L"GyroUE probe: testing rotation {} of {}{}.\nDON'T TOUCH THE CONTROLLER (the camera "
                                 L"twitches a little).",
                                 g_test.index + 1, g_test.queue.size(), g_test.round == 2 ? L" (second round)" : L""),
                     8);
        return;
    }
    if (g_test.round == 1 && !g_test.passed.empty()) {  // round 2: the ones that moved the camera, once more
        std::vector<ue::ProbeGroup> again;
        for (const ue::ProbeGroup& g : ue::probe_groups())
            if (std::find(g_test.passed.begin(), g_test.passed.end(), g.id) != g_test.passed.end()) again.push_back(g);
        g_test.queue = std::move(again);
        g_test.index = 0;
        g_test.round = 2;
        probe_log("probe test: second round, %zu rotation(s)", g_test.queue.size());
        if (!g_test.queue.empty()) {
            notify::show(std::format(L"GyroUE probe: second round, rotation 1 of {}.\nDON'T TOUCH THE CONTROLLER.",
                                     g_test.queue.size()),
                         8);
            return;
        }
    }
    finish_tests();
}

void test_result(int verdict, const std::string& note) {
    const ue::ProbeGroup& g = g_test.queue[g_test.index];
    const char* what = verdict == ue::kMovesCamera ? "MOVES THE CAMERA" : verdict == ue::kOverwritten ? "overwritten (output)"
                     : verdict == ue::kNoEffect    ? "kept, camera didn't move"
                     : verdict == ue::kInconsistent ? "inconsistent"
                     : verdict == ue::kTurnsInPlace ? "turned the view in place (no orbit)" : "not testable";
    probe_log("probe test (round %d): %s: %s%s%s", g_test.round, group_text(g).c_str(), what, note.empty() ? "" : ": ",
              note.c_str());
    log("probe test (round %d): %s: %s%s%s", g_test.round, group_text(g).c_str(), what, note.empty() ? "" : ": ", note.c_str());
    if (verdict == ue::kMovesCamera && g_test.round == 1) g_test.passed.push_back(g.id);  // verdict after round 2
    else if (verdict != ue::kMovesCamera && g_test.round == 2) ue::probe_verdict(g.id, ue::kInconsistent, note);
    else ue::probe_verdict(g.id, verdict, note);
    next_candidate();
}

// Undo whatever turn is written into the candidate. If it can't be written now (left gameplay), it stays pending
// and Settle retries it before anything else.
bool undo_turn(const Scene& s) {
    if (g_test.written_p == 0 && g_test.written_y == 0) return true;
    if (!apply(g_test.view, s, -g_test.written_p, -g_test.written_y)) return false;
    g_test.written_p = g_test.written_y = 0;
    return true;
}

void abort_test(const Scene& s, const char* why) {
    undo_turn(s);
    g_test.phase = LiveTest::Settle;
    if (++g_test.attempts > 3) {
        test_result(ue::kUntestable, std::string("interrupted 3 times (") + why + ")");
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_test.last_pause_notice > 4000) {
        g_test.last_pause_notice = now;
        notify::show(L"GyroUE probe: test paused. Stay in game and DON'T TOUCH THE CONTROLLER.", 5);
    }
}

bool start_tests() {
    std::vector<ue::ProbeGroup> queue;
    for (const ue::ProbeGroup& g : ue::probe_groups())
        if (g.verdict == ue::kUntested && queue.size() < size_t(kMaxTests)) queue.push_back(g);
    if (queue.empty() || g_test.written_p != 0 || g_test.written_y != 0) return false;
    const ULONGLONG keep_notice = g_test.last_pause_notice;
    g_test = {};
    g_test.last_pause_notice = keep_notice;
    g_test.queue = std::move(queue);
    g_test.phase = LiveTest::Settle;
    g_auto.armed = false;
    probe_log("probe test: %zu rotation(s) to test after %d captures", g_test.queue.size(), g_auto.captures);
    log("probe: live test of %zu rotation(s) that followed the camera (the player must not touch the controller)",
        g_test.queue.size());
    notify::show(std::format(L"GyroUE probe: {} captures done. Now testing {} rotation(s) live.\n"
                             L"Stay in game and DON'T TOUCH THE CONTROLLER: the camera will twitch a little.",
                             g_auto.captures, g_test.queue.size()),
                 10);
    return true;
}

// After a verified camera the probe's job is done: Probe=0 in GyroUE.ini (and in the game's profile, if it sets
// Probe itself), so the next start doesn't keep the gyro paused for another probe run.
bool disable_probe() {
    bool ok = WritePrivateProfileStringW(L"GyroUE", L"Probe", L"0", g_settings_path.c_str()) != 0;
    wchar_t value[8];
    if (GetPrivateProfileStringW(L"GyroUE", L"Probe", L"", value, 8, g_profile_path.c_str()) > 0)
        ok = WritePrivateProfileStringW(L"GyroUE", L"Probe", L"0", g_profile_path.c_str()) != 0 && ok;
    log("probe: %s", ok ? "Probe=0 written (the probe won't run on the next start)" : "couldn't write Probe=0, set it by hand");
    return ok;
}

// The probe is over for this session (until the probe key or AutoProbe starts it again).
void probe_end(const char* why) {
    if (!g_probe) return;
    g_probe = false;
    g_test.phase = LiveTest::Off;
    g_probe_runtime = g_probe_automatic = false;
    g_next_auto_probe = GetTickCount64() + 30000;
    log("probe: stopped (%s)", why);
    probe_log("probe: stopped (%s)", why);
}

// A probe started during play (or in a game with a profile) verified a camera: it becomes a Match=Camera view of the
// profile, used at once (check_profile_file reads the file again). The profile's other views and settings are kept.
void learn_camera(const ue::ProbeGroup& g) {
    LearnedView learned;
    learned.object = g.object;
    learned.property = g.property;
    learned.offsets = g.offsets;
    learned.rotator_size = g.size ? g.size : 24;
    learned.world_rotation = g.world;
    learned.label = g.cls.empty() ? "Learned camera" : g.cls;
    SYSTEMTIME t;
    GetLocalTime(&t);
    learned.note = std::format("Learned by the GyroUE {} camera probe on {:04}-{:02}-{:02} {:02}:{:02} (view target {}).\n"
                               "Live test: {}. Used while the camera follows it (Match=Camera).",
                               GYROUE_VERSION, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                               ue::name_of(g_auto.target_class), g.note.empty() ? "moved the camera" : g.note);
    std::vector<uint32_t> reserved;
    for (const Profile& p : g_profiles) reserved.push_back(p.id);
    uint32_t id = 0;
    std::string key;
    const AddResult r = add_view(g_profile_path, narrow(g_exe_stem), learned, reserved, id, key);
    const std::string rotation =
        g.object + (g.property.empty() ? std::format(" raw x{}", g.offsets.size()) : "." + g.property);
    if (r == AddResult::Added) {
        log("probe: camera learned: %s added to %s as view %u '%s' (Match=Camera)", rotation.c_str(),
            narrow(g_profile_path).c_str(), id, learned.label.c_str());
        probe_log("probe: camera learned: %s added to the profile as [View.%s] (Id=%u, Match=Camera)", rotation.c_str(),
                  key.c_str(), id);
        notify::show(L"GyroUE: camera found and VERIFIED, added to this game's profile.\nThe gyro turns it from now on "
                     L"(no restart needed); the other cameras keep working.", 20);
        g_profile_time = {};  // read the profile again on the next check
    } else if (r == AddResult::Duplicate) {
        log("probe: camera learned: %s is already view %u of the profile", rotation.c_str(), id);
        notify::show(L"GyroUE: camera found and VERIFIED; the profile already has it.\nThe gyro switches to it when "
                     L"the camera follows it.", 15);
    } else {
        log("probe: camera learned (%s) but %s couldn't be written", rotation.c_str(), narrow(g_profile_path).c_str());
        notify::show(L"GyroUE: camera found, but the game's profile couldn't be written.\nSee GyroUE\\GyroUE.log.", 15);
    }
    g_auto.done = true;
    if (!g_probe_runtime) disable_probe();
    probe_end("camera learned");
}

void finish_tests() {
    g_test.phase = LiveTest::Off;
    if (probe_adds_view()) {
        const std::vector<ue::ProbeGroup> groups = ue::probe_groups();
        const auto verified = std::find_if(groups.begin(), groups.end(),
                                           [](const ue::ProbeGroup& g) { return g.verdict == ue::kMovesCamera; });
        if (verified != groups.end()) {
            log("probe: done, a rotation moved the camera in both rounds of the live test");
            return learn_camera(*verified);
        }
    }
    bool usable = false, verified = false;
    const Written written = write_suggestion(usable, verified);
    const std::wstring file = L"GyroUE\\profiles\\" + g_exe_stem + (written == Written::Profile ? L".ini" : L".suggested.ini");
    if (verified) {
        g_auto.done = true;
        log("probe: done, a rotation moved the camera in both rounds of the live test");
        const bool off = written != Written::Nothing && disable_probe();
        const std::wstring probe_off = off ? L"The probe is now off (Probe=0)." : L"Then set Probe=0 in GyroUE\\GyroUE.ini.";
        notify::show(written == Written::Profile
                         ? L"GyroUE: camera found and VERIFIED, profile created.\nRESTART THE GAME: the gyro will then "
                           L"turn the camera.\n" + probe_off
                         : L"GyroUE: camera found and VERIFIED.\nThis game already has a profile, so it went to " + file +
                               L"\nRename it to " + g_exe_stem + L".ini (replacing the old one), then RESTART THE GAME: "
                               L"the gyro will then turn the camera.\n" + probe_off,
                     30);
        return;
    }
    if (g_auto.captures >= kMaxAutoCaptures) {
        g_auto.done = true;
        log("probe: done, no rotation moved the camera in the live test");
        notify::show(L"GyroUE probe: no rotation moved the camera, so no profile was created.\n" +
                         (usable ? L"An unverified guess is in " + file + L" (it will probably not work).\n" : std::wstring()) +
                         L"See GyroUE\\GyroUE-probe.log. The gyro works again (default camera).",
                     30);
        if (g_probe_runtime) probe_end("no rotation moved the camera");
        return;
    }
    notify::show(L"GyroUE probe: none of these rotations moved the camera.\nTurn the camera again with the RIGHT "
                 L"STICK or the MOUSE (more than 30°), then stop, to find more.",
                 15);
}

// One frame of the live test. valid: in game, unpaused, no cursor, focused (the same rules as the captures).
void run_live_test(const Scene& s, bool valid, ULONGLONG still_for) {
    const ULONGLONG now = GetTickCount64();
    const bool writing = g_test.phase != LiveTest::Settle && g_test.phase != LiveTest::Null;
    if (!valid || !s.pov_ok || ue::class_of(s.view_target) != g_auto.target_class || look_input()) {
        if (writing || g_test.phase == LiveTest::Null)
            abort_test(s, !valid ? "left gameplay" : !s.pov_ok ? "no camera" : ue::class_of(s.view_target) != g_auto.target_class
                                                                                  ? "another camera" : "stick moved");
        return;
    }
    const double dy = wrap180(s.pov_yaw - g_test.base_y), dp = wrap180(s.pov_pitch - g_test.base_p);
    // A condition held for `ms` (the camera may ease into a written turn).
    auto held = [&](bool on, ULONGLONG ms) {
        if (!on) return g_test.ok_since = 0, false;
        if (!g_test.ok_since) g_test.ok_since = now;
        return now - g_test.ok_since >= ms;
    };
    auto enter = [&](LiveTest::Phase p) {
        g_test.phase = p;
        g_test.phase_at = now;
        g_test.ok_since = 0;
    };
    const ULONGLONG in_phase = now - g_test.phase_at;
    switch (g_test.phase) {
    case LiveTest::Off:
        return;
    case LiveTest::Settle:  // the camera rests
        if (!undo_turn(s)) return;
        if (still_for < 600) return;
        g_test.base_p = s.pov_pitch, g_test.base_y = s.pov_yaw;
        enter(LiveTest::Null);
        return;
    case LiveTest::Null: {  // and keeps resting without our writes (no drift, no idle animation turning it)
        if (std::abs(dy) > 0.2 || std::abs(dp) > 0.2) {
            enter(LiveTest::Settle);
            return;
        }
        if (in_phase < 250) return;
        const ue::ProbeGroup& g = g_test.queue[g_test.index];
        g_test.view = test_view(g);
        double p0 = 0;
        if (!read_view_rotation(g_test.view, s, p0, g_test.val_y))
            return test_result(ue::kUntestable, "its object path doesn't resolve now");
        g_test.pawn_yaw = pawn_yaw(s);
        g_test.yaw_ok = g_test.pitch_ok = g_test.turns_pawn = false;
        g_test.base_loc_ok = s.loc_ok;
        if (s.loc_ok) std::copy(s.pov_loc, s.pov_loc + 3, g_test.base_loc);
        if (!apply(g_test.view, s, 0, kTestTurn)) return test_result(ue::kUntestable, "couldn't be written");
        g_test.written_y = kTestTurn;
        enter(LiveTest::YawOut);
        return;
    }
    case LiveTest::YawOut: {  // the camera turns by the written yaw, and stays there
        const bool on = std::abs(dy - kTestTurn) < kTestTolerance && std::abs(dp) < kTestTolerance;
        if (held(on, 150)) g_test.yaw_ok = true;
        else if (in_phase < 700) return;
        double p = 0, y = 0;
        const bool kept = read_view_rotation(g_test.view, s, p, y) && std::abs(wrap180(y - g_test.val_y - kTestTurn)) < kTestTolerance;
        const double pawn_now = pawn_yaw(s);
        g_test.turns_pawn = std::isfinite(pawn_now) && std::isfinite(g_test.pawn_yaw) &&
                            std::abs(wrap180(pawn_now - g_test.pawn_yaw - kTestTurn)) < kTestTolerance;
        // Third-person camera: the stick orbits it around the character, so its location moves; a candidate that
        // only turns the view on the spot (a free camera) is not the look input.
        const double radius = orbit_radius();
        g_test.orbit_note.clear();
        if (g_test.yaw_ok && radius >= kOrbitMin && g_test.base_loc_ok && s.loc_ok) {
            const double angle = view_angle(g_test.base_p, g_test.base_y, s.pov_pitch, s.pov_yaw);
            const double expected = 2 * radius * std::sin(angle * kDegToRad / 2);
            const double moved = distance(g_test.base_loc, s.pov_loc);
            g_test.orbit_note = std::format("camera moved {:.1f} for an expected {:.1f} (orbit radius {:.0f})", moved,
                                            expected, radius);
            if (moved < expected * 0.4) {
                undo_turn(s);
                return test_result(ue::kTurnsInPlace, g_test.orbit_note);
            }
        }
        undo_turn(s);
        if (!g_test.yaw_ok)
            return test_result(kept ? ue::kNoEffect : ue::kOverwritten,
                               kept ? "the value kept the turn, the camera didn't follow" : "the game rewrote the value");
        enter(LiveTest::YawBack);
        return;
    }
    case LiveTest::YawBack:  // undoing it brings the camera back
        if (held(std::abs(dy) < kTestTolerance && std::abs(dp) < kTestTolerance, 100)) {
            g_test.base_p = s.pov_pitch, g_test.base_y = s.pov_yaw;
            g_test.delta_p = g_test.base_p > 0 ? -kTestTurn : kTestTurn;  // towards level: away from the pitch limits
            if (!apply(g_test.view, s, g_test.delta_p, 0)) return test_result(ue::kUntestable, "pitch couldn't be written");
            g_test.written_p = g_test.delta_p;
            enter(LiveTest::PitchOut);
            return;
        }
        if (in_phase >= 700) return test_result(ue::kInconsistent, "the camera turned but didn't come back when undone");
        return;
    case LiveTest::PitchOut: {
        const bool on = std::abs(dp - g_test.delta_p) < kTestTolerance && std::abs(dy) < kTestTolerance;
        if (held(on, 150)) g_test.pitch_ok = true;
        else if (in_phase < 700) return;
        undo_turn(s);
        enter(LiveTest::PitchBack);
        return;
    }
    case LiveTest::PitchBack:
        if (!held(std::abs(dy) < kTestTolerance && std::abs(dp) < kTestTolerance, 100) && in_phase < 700) return;
        test_result(ue::kMovesCamera, std::string("yaw followed") +
                                          (g_test.pitch_ok ? ", pitch followed" : ", pitch didn't move (may be kept elsewhere)") +
                                          (g_test.turns_pawn ? ", it also turns the character" : "") +
                                          (g_test.orbit_note.empty() ? "" : ", " + g_test.orbit_note));
        return;
    }
}

void auto_probe(const Scene& s) {
    if (!g_auto.enabled || g_auto.done) return;
    const ULONGLONG now = GetTickCount64();
    const bool valid = s.pov_ok && s.pawn && !s.paused && !s.cursor && game_has_focus();
    if (valid) {
        const double motion = g_auto.have_last ? std::abs(wrap180(s.pov_yaw - g_auto.last_yaw)) +
                                                     std::abs(wrap180(s.pov_pitch - g_auto.last_pitch))
                                               : 1;
        if (motion > 0.05 || !g_auto.have_last) g_auto.still_since = now;
        g_auto.last_pitch = s.pov_pitch, g_auto.last_yaw = s.pov_yaw, g_auto.have_last = true;
    } else {
        g_auto.have_last = false;
    }
    const ULONGLONG still_for = valid ? now - g_auto.still_since : 0;
    if (g_test.phase != LiveTest::Off) return run_live_test(s, valid, still_for);
    if (!valid || g_auto.captures >= kMaxAutoCaptures) {
        g_auto.armed = false;
        return;
    }

    if (g_auto.armed) {
        if (look_input()) g_auto.stick_turn = true;
        if (s.pawn != g_auto.pawn || now - g_auto.snapshot_at > 120000) {  // other pawn, or stale
            g_auto.armed = false;
            return;
        }
        // UE5 keeps pitch as 0..360 (350 = 10 degrees down): both deltas wrap.
        const double dy = std::abs(wrap180(s.pov_yaw - g_auto.snap_yaw)), dp = std::abs(wrap180(s.pov_pitch - g_auto.snap_pitch));
        if (still_for < 300 || dy < 30) return;  // still turning, or not far enough yet
        g_auto.armed = false;
        g_auto.next_snapshot = now + 1500;
        // The camera turned without the right stick: a cutscene or a scripted camera, not the player's look input.
        if (g_auto.stick_data && !g_auto.stick_turn) {
            probe_log("probe: turn of %.0f degrees ignored: neither the right stick nor the mouse was used (cutscene or scripted camera)", dy);
            return;
        }
        ++g_auto.captures;
        probe_log("probe: automatic capture %d: the camera turned %.0f degrees sideways and %.0f up/down", g_auto.captures, dy, dp);
        if (ue::probe_diff(false) == 0) {
            notify::show(L"GyroUE probe: capture " + std::to_wstring(g_auto.captures) +
                         L" could not be compared (see GyroUE-probe.log).\nKeep turning with the right stick.");
            return;
        }
        g_auto.target_class = ue::class_of(s.view_target);
        if (g_auto.snap_loc_ok && s.loc_ok) {
            const double angle = view_angle(g_auto.snap_pitch, g_auto.snap_yaw, s.pov_pitch, s.pov_yaw);
            const double chord = distance(g_auto.snap_loc, s.pov_loc);
            const double radius = chord / (2 * std::sin(angle * kDegToRad / 2));
            g_auto.radii.push_back(radius);
            probe_log("probe: the camera moved %.1f units for a %.0f degree turn: orbit radius %.0f (%s)", chord, angle, radius,
                      radius >= kOrbitMin ? "third person, orbits the character" : "turns on the spot");
        }
        if (g_auto.captures >= kCapturesForTest && start_tests()) return;
        if (g_auto.captures >= kMaxAutoCaptures) return finish_tests();
        notify::show(g_auto.captures < kCapturesForTest
                         ? std::format(L"GyroUE probe: capture {} of {} done.\nTurn the camera again with the RIGHT STICK or MOUSE "
                                       L"(more than 30°, also up or down), then stop.",
                                       g_auto.captures, kCapturesForTest)
                         : std::format(L"GyroUE probe: capture {} done, no new rotation to test yet.\nTurn the camera again "
                                       L"with the RIGHT STICK or MOUSE (more than 30°), then stop.",
                                       g_auto.captures),
                     10);
        return;
    }
    if (still_for >= 500 && now >= g_auto.next_snapshot) {
        take_snapshot(s);
        g_auto.armed = true;
        g_auto.snap_pitch = s.pov_pitch, g_auto.snap_yaw = s.pov_yaw;
        g_auto.snap_loc_ok = s.loc_ok;
        if (s.loc_ok) std::copy(s.pov_loc, s.pov_loc + 3, g_auto.snap_loc);
        g_auto.stick_turn = false;
        g_auto.pawn = s.pawn;
        g_auto.snapshot_at = now;
        if (!g_auto.announced) {
            g_auto.announced = true;
            log("probe: automatic capture ready: turn the camera with the RIGHT STICK or the MOUSE (more than 30 degrees), stop, "
                "and repeat; after %d captures the rotations found are tested live; the gyro is paused meanwhile",
                kCapturesForTest);
            notify::show(L"GyroUE probe ready to find the camera. In game, turn the camera with the RIGHT STICK or MOUSE "
                         L"(more than 30°), then stop. Repeat 3 times.\nThen don't touch the controller while "
                         L"the probe tests what it found.",
                         30);
        }
    }
}

bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void probe_keys(const Scene& s) {
    // Plain F7/F8 only: with Ctrl, Shift or Alt they may be the probe key (Ctrl+F7 by default).
    const bool plain = game_has_focus() && !key_held(VK_CONTROL) && !key_held(VK_SHIFT) && !key_held(VK_MENU);
    const bool snap = plain && key_held(VK_F7);
    const bool diff = plain && key_held(VK_F8);
    if ((snap || diff) && g_auto.enabled) {
        g_auto.enabled = g_auto.armed = false;
        undo_turn(s);
        g_test.phase = LiveTest::Off;
        log("probe: F7/F8 used, automatic probe (captures and live test) off for this session");
    }
    if (snap && !g_probe_snap_down) take_snapshot(s);
    if (diff && !g_probe_diff_down && ue::probe_diff(true) > 0) notify_manual_diff();
    g_probe_snap_down = snap;
    g_probe_diff_down = diff;
}

// Starts the probe during play, for the camera of that moment: from the probe key, or by itself (AutoProbe) when the
// camera stopped following the gyro. Earlier captures and verdicts are forgotten.
void probe_start(bool automatic) {
    if (g_probe) return;
    open_probe_log();
    ue::probe_reset();
    const ULONGLONG keep_notice = g_test.last_pause_notice;
    g_auto = {};
    g_test = {};
    g_test.last_pause_notice = keep_notice;
    g_probe = true;
    g_probe_runtime = true;
    g_probe_automatic = automatic;
    g_probe_started_at = GetTickCount64();
    const char* why = automatic ? "AutoProbe: the camera stopped following the gyro" : "probe key";
    log("probe: started during play (%s)", why);
    probe_log("probe: started during play (%s)", why);
    if (automatic) {
        g_auto.announced = true;  // this notice replaces the usual "probe ready" one
        const std::wstring cancel = g_probe_key.name.empty()
                                        ? std::wstring()
                                        : L" " + std::wstring(g_probe_key.name.begin(), g_probe_key.name.end()) + L" cancels.";
        notify::show(L"GyroUE: this camera ignores the gyro. To teach it to GyroUE, turn the camera with the RIGHT STICK or "
                     L"MOUSE (more than 30°), then stop. Repeat 3 times.\nThen don't touch the controller for a few "
                     L"seconds." + cancel,
                     20);
    }
}

// Probe key: starts the probe during play, or cancels any probe (also one started with Probe=1, for this session).
void probe_hotkey(const Scene& s) {
    const Key& k = g_probe_key;
    const bool down = k.vk && game_has_focus() && key_held(k.vk) && k.ctrl == key_held(VK_CONTROL) &&
                      k.shift == key_held(VK_SHIFT) && k.alt == key_held(VK_MENU);
    if (down && !g_probe_key_down) {
        if (!g_probe) {
            probe_start(false);
        } else {
            undo_turn(s);
            probe_end("probe key");
            notify::show(L"GyroUE probe cancelled.", 5);
        }
    }
    g_probe_key_down = down;
}

// AutoProbe: an automatic probe that hasn't captured a turn for a while gives up (the camera may be a cutscene's),
// and so does one whose camera follows the gyro again before its live test.
void auto_probe_watch() {
    if (!g_probe || !g_probe_automatic || g_test.phase != LiveTest::Off) return;
    const ULONGLONG now = GetTickCount64();
    if (g_auto.captures == 0 && now - g_probe_started_at > 45000) {
        probe_end("AutoProbe: no camera turn with the stick or mouse in 45 s");
        notify::show(L"GyroUE: stopped looking for the camera (no turn with the stick or mouse).", 6);
    }
}

// Returns whether the probe started.
bool request_auto_probe() {
    if (!g_auto_probe_setting || g_probe || g_auto_probe_starts >= 3 || GetTickCount64() < g_next_auto_probe) return false;
    ++g_auto_probe_starts;
    probe_start(true);
    return true;
}

// Step 1 of finding a camera: tells the user when the gyro turns the view's rotation but the camera doesn't
// follow (the game ignores ControlRotation, or the profile's rotation), or confirms that it does. Only on the
// player's own camera (see player_camera): scripted or fixed cameras ignore every rotation by design.
// After the verdict the check keeps running, only to log when the camera stops or starts following again (a
// camera switch inside the same view target, e.g. a game action that takes over the camera).
struct FollowCheck {
    double applied = 0, moved = 0, last_pitch = 0, last_yaw = 0, total_applied = 0, total_moved = 0;
    bool have_last = false, warned = false, confirmed = false;
    int ignored = 0, followed = 0;  // 2 s windows with enough gyro motion where the camera ignored / followed it
    int ignored_in_a_row = 0;       // ... ignored since the last followed window
    bool lost = false;              // after "confirmed": the camera stopped following (logged)
    bool stick = false;             // the right stick moved in this window: the camera motion can't be judged
    ULONGLONG window_end = 0;
    // Some games' gameplay camera is its own actor, not the pawn (Hogwarts Legacy: BP_ThirdPersonCameraStackActor_C).
    // A view target class whose camera turned 30+ degrees while the right stick was held is the player's camera.
    ue::Obj stick_class = 0;        // view target class being measured
    double stick_moved = 0;         // camera rotation seen with the stick held, for stick_class
    std::vector<ue::Obj> player_cameras;
} g_follow;

// The view target is the player's own camera: the pawn, or a camera actor that follows the right stick.
bool player_camera(const Scene& s, double moved) {
    if (s.view_target == s.pawn) return true;
    const ue::Obj cls = ue::class_of(s.view_target);
    if (!cls) return false;
    if (std::find(g_follow.player_cameras.begin(), g_follow.player_cameras.end(), cls) != g_follow.player_cameras.end())
        return true;
    if (cls != g_follow.stick_class) g_follow.stick_class = cls, g_follow.stick_moved = 0;
    if (look_input()) g_follow.stick_moved += moved;
    if (g_follow.stick_moved < 30) return false;
    g_follow.player_cameras.push_back(cls);
    log("camera: view target %s turns with the right stick: treated as the player's camera", ue::name_of(cls).c_str());
    return true;
}

// Which camera the player sees, for the follow check's log lines.
std::string camera_description(const Scene& s) {
    std::string text = std::format("target {}, camera manager {}", ue::name_of(ue::class_of(s.view_target)),
                                   ue::name_of(ue::class_of(s.camera)));
    const std::string style = ue::value_string(s.camera, "CameraStyle");
    if (!style.empty()) text += ", CameraStyle " + style;
    return text;
}

// gyro_yaw: the gyro's yaw written this frame (degrees, absolute). The windows judge yaw only: at the pitch limit
// (looking straight up or down) the camera ignores pitch writes, which isn't the game ignoring the gyro.
void follow_check(const Scene& s, const View* view, double gyro_yaw) {
    if (!s.pov_ok) {
        g_follow.have_last = false;
        return;
    }
    const double moved_yaw = g_follow.have_last ? std::abs(wrap180(s.pov_yaw - g_follow.last_yaw)) : 0;
    const double moved = g_follow.have_last ? moved_yaw + std::abs(wrap180(s.pov_pitch - g_follow.last_pitch)) : 0;
    g_follow.last_pitch = s.pov_pitch, g_follow.last_yaw = s.pov_yaw, g_follow.have_last = true;
    if (!view || s.paused || s.cursor || !s.pawn) return;
    if (view->view_target_class.empty() && !player_camera(s, moved)) return;
    if (gyro_yaw <= 0) return;
    g_follow.applied += gyro_yaw;
    g_follow.moved += moved_yaw;
    if (look_input()) g_follow.stick = true;
    const ULONGLONG now = GetTickCount64();
    if (!g_follow.window_end) g_follow.window_end = now + 2000;
    if (now < g_follow.window_end) return;
    int verdict = 0;  // this window: -1 ignored, 1 followed, 0 unclear or not judged
    const bool judged = g_follow.applied >= 10 && !g_follow.stick;
    if (judged) {  // enough gyro motion in this window to judge, no stick
        const double ratio = g_follow.moved / g_follow.applied;
        if (ratio < 0.2) verdict = -1;
        else if (ratio > 0.5) verdict = 1;
    }
    const double window_applied = g_follow.applied, window_moved = g_follow.moved;
    g_follow.applied = g_follow.moved = 0;
    g_follow.window_end = 0;
    g_follow.stick = false;
    std::string rotation = view->property.empty() ? "ControlRotation" : view->property;
    if (!view->offsets.empty()) rotation = "the profile's raw offsets";
    if (verdict == -1) ++g_follow.ignored_in_a_row;
    if (verdict == 1) g_follow.ignored_in_a_row = 0;
    if (g_follow.warned || g_follow.confirmed) {  // verdict given: only log changes from here on
        // Three ignored windows in a row (about 6 s of gyro), as for the first verdict: one or two can be the pitch
        // limit, a camera blend, a short scripted moment or a hitch. Two gave a false AutoProbe start in PAYDAY 3
        // (an action that held the camera for ~4 s).
        if (g_follow.confirmed && !g_follow.lost && g_follow.ignored_in_a_row >= 3) {
            g_follow.lost = true;
            log("camera: stopped following the gyro (%s, view '%s'; last window: gyro %.0f degrees, camera %.0f) - %s",
                rotation.c_str(), view->label.c_str(), window_applied, window_moved, camera_description(s).c_str());
            request_auto_probe();
        } else if (verdict == 1 && (g_follow.lost || (g_follow.warned && !g_follow.confirmed))) {
            g_follow.lost = false;
            g_follow.confirmed = true;
            log("camera: follows the gyro again (%s, view '%s') - %s", rotation.c_str(), view->label.c_str(),
                camera_description(s).c_str());
            if (g_probe_automatic && g_test.phase == LiveTest::Off) probe_end("AutoProbe: the camera follows the gyro again");
        }
        return;
    }
    if (verdict == -1) ++g_follow.ignored;
    if (verdict == 1) ++g_follow.followed;
    if (judged) {
        g_follow.total_applied += window_applied;
        g_follow.total_moved += window_moved;
    }
    if (!g_follow.warned && g_follow.ignored >= 3 && g_follow.followed == 0) {
        g_follow.warned = true;
        log("camera: the gyro turned %s (view '%s') by %.0f degrees but the camera moved only %.0f: the game ignores it. "
            "To find the camera's real rotation, set Probe=1 in %s, restart and turn the camera with the right stick "
            "(ADVANCED.md: \"Finding the camera of a new game\").",
            rotation.c_str(), view->label.c_str(), g_follow.total_applied, g_follow.total_moved, narrow(g_settings_path).c_str());
        if (g_probe)
            notify::show(L"GyroUE: the gyro doesn't move this camera yet.\nThe probe is on: turn the camera with the "
                         L"right stick or mouse to find the right rotation.", 12);
        else if (request_auto_probe())
            ;  // AutoProbe started: its own notice says what to do
        else if (view->offsets.empty() && view->property.empty())
            notify::show(L"GyroUE: this game's camera seems to ignore the gyro.\nTo find it, set Probe=1 in GyroUE\\GyroUE.ini, "
                         L"restart the game. Ignore this if it's actually working.", 15);
        else
            notify::show(L"GyroUE: the camera doesn't follow the profile's rotation.\nThe profile may be outdated: "
                         L"set Probe=1 in GyroUE\\GyroUE.ini to find the camera again.", 15);
    }
    if (!g_follow.confirmed && g_follow.followed >= 3) {
        g_follow.confirmed = true;
        log("camera: confirmed, the camera follows the gyro (%s, view '%s') - %s", rotation.c_str(), view->label.c_str(),
            camera_description(s).c_str());
    }
}

// The fast path of the Match=Camera choice reads the current view's rotation back: its yaw before and after this
// frame's gyro write, to see on the next frame whether the game put it back.
struct WriteBack {
    uint32_t view = 0;  // 0 = nothing written last frame
    double before = 0, after = 0, written = 0;
} g_write_back;

// One frame of the Match=Camera choice (camera_match.hpp). On a switch, the gyro the dropped view lost is written into
// the new one at once, so the camera catches up instead of losing the turn. Logs each switch with its reason (up to
// 200 per session: a cutscene ignores every view and would repeat them) and every view that follows after a switch.
void camera_match_frame(const Scene& s, const View* view, double yaw, double pitch, bool write_failed, int rewritten) {
    static bool have_last = false, all_ignored = false;
    static double last_yaw = 0;
    static int switches_logged = 0;
    // Yaw only: at the pitch limit the camera ignores pitch whatever view turns it (camera_match.hpp).
    double moved = 0;
    if (s.pov_ok && have_last) moved = std::abs(wrap180(s.pov_yaw - last_yaw));
    if (s.pov_ok) last_yaw = s.pov_yaw;
    have_last = s.pov_ok;
    if (!view || !s.pov_ok || s.paused || s.cursor) return;
    auto label = [](uint32_t id) {
        const View* v = find_view(id);
        return v ? v->label : std::string("?");
    };
    CameraMatch::Frame f;
    f.yaw = yaw, f.pitch = pitch, f.moved = moved, f.look = look_input(), f.write_failed = write_failed;
    f.rewritten = rewritten;
    switch (g_match.frame(f, GetTickCount64())) {
    case CameraMatch::Switched: {
        g_write_back = {};
        View* next = find_view(g_match.current());
        const double cy = g_match.carry_yaw(), cp = g_match.carry_pitch();
        const bool carried = next && (cy != 0 || cp != 0) && apply(*next, s, cp, cy);
        if (carried) g_applied_yaw += cy, g_applied_pitch += cp, g_applied_this_frame = true;
        const char* why = g_match.reason() == CameraMatch::Rewritten ? "is rewritten by the game"
                        : g_match.reason() == CameraMatch::Missing   ? "can't be written (object not found)"
                                                                     : "is ignored by the camera";
        if (++switches_logged <= 200)
            log("camera: '%s' %s, trying '%s' (%.1f degrees carried over)", label(g_match.previous()).c_str(), why,
                label(g_match.current()).c_str(), carried ? std::abs(cy) + std::abs(cp) : 0.0);
        break;
    }
    case CameraMatch::Follows:
        all_ignored = false;
        log("camera: the camera follows view '%s' now (Match=Camera) - %s", label(g_match.current()).c_str(),
            camera_description(s).c_str());
        break;
    case CameraMatch::AllIgnored:
        g_write_back = {};
        if (!all_ignored) log("camera: no view of this camera follows the gyro (cutscene, or a camera to learn) - %s",
                              camera_description(s).c_str());
        all_ignored = true;
        break;
    case CameraMatch::None: break;
    }
}

void status(const Scene& s, const View* view) {
    gl_gyro_state gs{};
    gl_get_gyro_state(g_gyro->get(), &gs);
    gl_diagnostics d{};
    gl_get_diagnostics(g_gyro->get(), &d);
    log("status: view=%s pc=%s pawn=%s target=%s paused=%d cursor=%d | gyro enabled=%u motion=%u calib=%u "
        "samples=%llu still=%u bias=(%.2f %.2f %.2f) | applied yaw=%.1f pitch=%.1f",
        view ? view->label.c_str() : "none", ue::name_of(ue::class_of(s.pc)).c_str(),
        ue::name_of(ue::class_of(s.pawn)).c_str(), ue::name_of(ue::class_of(s.view_target)).c_str(), s.paused,
        s.cursor, gs.enabled, gs.motion_available, d.calibration_state, (unsigned long long)d.accepted_samples,
        d.stationary, d.bias.x, d.bias.y, d.bias.z, g_applied_yaw, g_applied_pitch);
    // Object addresses, for a debugger (e.g. a write breakpoint on ControlRotation); logged when they change.
    static ue::Obj last_pc = 0, last_pawn = 0, last_camera = 0;
    if (s.pc != last_pc || s.pawn != last_pawn || s.camera != last_camera) {
        last_pc = s.pc, last_pawn = s.pawn, last_camera = s.camera;
        const ue::Prop cr = ue::prop_of(s.pc, "ControlRotation");
        log("status: addresses pc=0x%llX (ControlRotation +0x%X) pawn=0x%llX camera=0x%llX", (unsigned long long)s.pc,
            cr ? unsigned(cr.offset) : 0u, (unsigned long long)s.pawn, (unsigned long long)s.camera);
    }
    {
        Delivery& m = g_delivery;
        const uint64_t now_ns = SDL_GetTicksNS();
        const double span = m.since_ns ? double(now_ns - m.since_ns) * 1e-9 : 0;
        if (span > 0)
            log("status: motion %.0f samples/s in %.0f arrivals/s | camera updates %.0f/s, %.0f%% turned, %.0f%% of "
                "those without a new sample (GyroLib burst delivery)",
                m.samples / span, m.arrivals / span, m.updates / span, m.updates ? 100.0 * m.turned / m.updates : 0.0,
                m.turned ? 100.0 * m.spread / m.turned : 0.0);
        m.samples = m.arrivals = m.updates = m.turned = m.spread = 0;
        m.since_ns = now_ns;
    }
    if (view && !view->offsets.empty()) {
        const ue::Obj o = view_object(*view, s);
        std::string text;
        for (int32_t off : view->offsets) {
            double p = 0, y = 0, r = 0;
            if (o && ue::read_rotator(o + off, view->rotator_size, p, y, r))
                text += std::format(" +0x{:X}=(P {:.2f} Y {:.2f})", off, p, y);
        }
        log("status: view object %s%s", o ? ue::name_of(o).c_str() : "(not found)", text.c_str());
    }
}

// The engine's reflection data can't be read (e.g. "ue: core classes not found"): nothing can turn the camera, and the
// probe needs the same data, so say so once instead of staying silent.
void engine_check() {
    static ULONGLONG since = GetTickCount64();
    static bool told = false;
    if (told || ue::ready() || GetTickCount64() - since < 60000) return;
    told = true;
    log("ue: the engine's data still can't be read after 60 s: the gyro can't turn the camera in this game "
        "(the probe needs the same data). Please report the game with this log.");
    notify::show(L"GyroUE: this game isn't supported yet (its engine data can't be read),\n"
                 L"so the gyro can't turn the camera. Please report it with GyroUE\\GyroUE.log.", 20);
}

// GyroLib's panel is driven with the controller while it is open, so the game reads an idle controller then (XInput
// and its own PlayStation support), and after the panel closes until every button is released and the triggers are
// up: the press that closed it doesn't reach the game either.
bool panel_has_pad(gl_context* ctx, SDL_Gamepad* pad) {
    static bool held = false;
    if (gl_panel_open(ctx)) return held = true;
    if (!held || !pad) return held = false;
    bool busy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 ||
                SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !busy; ++b) busy = SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
    return held = busy;
}

// A state logged when it changes and the new value has held for a while, so a hold-to-aim button or a pad being
// picked up doesn't flood the log. The first value is logged at once.
struct StateLog {
    int logged = -1, pending = -1;
    ULONGLONG since = 0;
    bool changed(int value, ULONGLONG now, ULONGLONG hold_ms) {
        if (value == logged) return pending = -1, false;
        if (value != pending) pending = value, since = now;
        if (logged >= 0 && now - since < hold_ms) return false;
        logged = value, pending = -1;
        return true;
    }
};

// The window in front when the game isn't: its process file name and window class.
std::string foreground_window() {
    const HWND w = GetForegroundWindow();
    if (!w) return "none";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[128] = {}, path[MAX_PATH] = {};
    GetClassNameW(w, cls, 128);
    DWORD size = MAX_PATH;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        if (!QueryFullProcessImageNameW(p, 0, path, &size)) path[0] = 0;
        CloseHandle(p);
    }
    const wchar_t* file = std::wcsrchr(path, L'\\');
    return std::format("{} (window class {})", narrow(file ? file + 1 : path), narrow(cls));
}

// What the "gyro on/off" line can't see: focus, GyroLib's own permission (activation mode, focus, pause, menus,
// calibration) and the sensor stream. A gyro that stops while GyroUE still says "gyro on" shows up here.
void log_gyro_changes(bool focused, const gl_gyro_state& gs) {
    static StateLog focus, enabled, motion, still, source;
    static uint64_t last_samples = 0;
    gl_diagnostics d{};
    gl_get_diagnostics(g_gyro->get(), &d);
    const ULONGLONG now = GetTickCount64();
    if (focus.changed(focused, now, 500))
        log("focus: %s", focused ? "the game window is in front" : ("lost to " + foreground_window()).c_str());
    if (enabled.changed(gs.enabled != 0, now, 1000))
        log("gyro: GyroLib %s", gs.enabled ? "enables the gyro"
                                           : "suspends the gyro (activation mode, focus, pause, menus or calibration)");
    if (motion.changed(gs.motion_available != 0, now, 1000))
        log("gyro: motion data %s", gs.motion_available ? "available" : "lost (no controller with a gyro)");
    if (source.changed(int(d.source), now, 0)) log("gyro: motion source %u", d.source);
    if (still.changed(d.stationary != 0, now, 3000))
        log("gyro: the controller %s", d.stationary ? "reads as lying still" : "moves");
    if (d.accepted_samples < last_samples)
        log("gyro: GyroLib's sample count restarted (%llu -> %llu): the controller was reopened",
            (unsigned long long)last_samples, (unsigned long long)d.accepted_samples);
    last_samples = d.accepted_samples;
}

void frame() {
    if (!g_gyro && !start_gyro()) {
        g_failed = true;
        return;
    }
    check_settings_file();
    check_profile_file();

    // Settings panel: attach once the game's DX12 swapchain is known. Without a renderer, never
    // leave the (invisible) panel open, or it would hold the gyro suspended.
    overlay::attach(g_gyro->get());
    overlay::poll_menu_key(g_gyro->get(), game_has_focus());
    overlay::process();
    if (!overlay::available() && gl_panel_open(g_gyro->get())) gl_set_panel_open(g_gyro->get(), 0);

    const Scene s = read_scene();
    engine_check();
    probe_hotkey(s);
    if (g_probe) probe_keys(s);
    auto_probe_watch();
    View* base = base_view(s);
    View* view = active_view(s, base);
    if (view) diagnostics(*view, s);
    const uint32_t view_id = view ? view->id : 0;
    // Logged for the base view; switches between Match=Camera views are logged by camera_match_frame.
    const uint32_t base_id = base ? base->id : 0;
    if (base_id != g_last_view) {
        g_last_view = base_id;
        log("view: %s (target %s)", base ? base->label.c_str() : "none", ue::name_of(ue::class_of(s.view_target)).c_str());
    }

    gl_host_state host{};
    host.focused = game_has_focus();
    host.paused = s.paused;
    // Menus: some games (Cronos) show no cursor in gamepad menus, so pause and "no gameplay view" count too
    // (automatic "menus only" calibration needs it).
    host.menu_open = !s.pc || s.cursor || s.paused || view == nullptr;
    host.camera_allowed = view != nullptr && !s.cursor;
    // Log every change of whether the gyro may turn the camera, with the reason, so a gyro that "suddenly stops"
    // can be matched to a cursor, a pause or a lost view at that exact moment.
    {
        const char* gate = !view ? "off: no gameplay view" : s.cursor ? "off: mouse cursor shown" : s.paused ? "off: game paused"
                                                                                                   : "on";
        static std::string last_gate;
        if (last_gate != gate) {
            last_gate = gate;
            log("gyro %s (target %s)", gate, ue::name_of(ue::class_of(s.view_target)).c_str());
        }
    }
    const uint64_t now_ns = SDL_GetTicksNS();
    update_recenter_support(view, s);
    g_recenter_fraction = 0;
    // The default view (and its Match=Camera views) reports the active sensitivity profile instead (decided from last
    // frame's buttons).
    const bool default_view = base_id == 1;
    const uint32_t profile = default_view ? sensitivity_profile() : 0;
    if (profile != g_last_profile) {
        g_last_profile = profile;
        const char* label = view ? view->label.c_str() : "none";
        for (const Profile& p : g_profiles)
            if (p.id == profile) label = p.label;
        log("profile: %s", label);
    }
    const uint32_t gl_view = profile ? profile : view_id;
    const gl_output out = g_input->update(now_ns, host, gl_view);
    // GyroLib's flick stick needs the game's own right-stick rotation suppressed: possible for the controller shown to
    // the game as XInput, or when the game reads the controller itself through a checked report layout.
    SDL_Gamepad* pad = xinput::selected_gamepad(g_gyro->get(), g_input->get(), out);
    const bool neutral = panel_has_pad(g_gyro->get(), pad);
    update_sensitivity_profiles(pad, default_view && host.camera_allowed && host.focused && !s.paused && !neutral);
    xinput::set_neutral(neutral);
    nativepad::set_neutral(neutral);
    // Long-press blocking: the activation button's press is held back from the game, or given to it as a short tap,
    // wherever the game reads that controller through a path GyroUE can change.
    const longpress::Mask buttons = longpress::frame(g_gyro->get(), pad, now_ns, host.focused && !neutral, g_long_press);
    const bool real_buttons = xinput::set_buttons(pad, buttons.hide, buttons.tap);
    const bool native_buttons = nativepad::set_buttons(buttons.hide, buttons.tap);
    const bool xinput_shown = xinput::frame(g_gyro->get(), g_input->get(), out);
    const bool native = nativepad::frame(pad, out.suppress_native_right_stick != 0 && !xinput_shown);
    learn_dualsense();
    const bool long_press = xinput_shown || real_buttons || native_buttons;
    if ((xinput_shown || native) != g_stick_suppression || long_press != g_long_press) {
        g_stick_suppression = xinput_shown || native;
        if (long_press != g_long_press)
            log("long-press blocking %s", long_press ? (xinput_shown ? "available (the controller shown as XInput)"
                                                       : real_buttons ? "available (the real XInput controller)"
                                                                      : "available (the game's own controller reports)")
                                                     : "unavailable: the game reads the controller in a way GyroUE can't change");
        g_long_press = long_press;
        gl_set_host_capabilities(g_gyro->get(), GL_HOST_MENU_STATE |
                                                    (g_stick_suppression ? GL_HOST_NATIVE_STICK_SUPPRESSION : 0u) |
                                                    (g_long_press ? GL_HOST_LONG_PRESS_BLOCKING : 0u));
    }

    double gyro_deg = 0;  // gyro rotation actually written this frame
    const double pitch = out.pitch_degrees, yaw = out.yaw_degrees;
    {
        gl_gyro_state gs{};
        gl_get_gyro_state(g_gyro->get(), &gs);
        const bool camera = view && host.camera_allowed && gs.enabled;
        log_gyro_changes(host.focused, gs);
        g_delivery.on_update(camera, gs.new_samples, yaw != 0 || pitch != 0);
        trace_frame(now_ns, camera, gs.new_samples, out, pitch, yaw);
    }
    bool write_failed = false;  // the view's rotation couldn't be written (its object is missing)
    // Match=Camera fast path: did the game put back last frame's write into this view? Its rotation now is also this
    // frame's "before".
    int rewritten = -1;
    double now_pitch = 0, now_yaw = 0;
    const bool read_back = view && g_match.active() && read_view_rotation(*view, s, now_pitch, now_yaw);
    if (read_back && g_write_back.view == view->id)
        rewritten = CameraMatch::rewritten(g_write_back.before, g_write_back.after, now_yaw, g_write_back.written);
    g_write_back = {};
    double written_yaw = 0, written_pitch = 0;
    if (view && host.camera_allowed && !probe_capturing() && (yaw != 0 || pitch != 0)) {
        if (apply(*view, s, pitch, yaw)) {
            g_applied_this_frame = true;
            g_applied_yaw += yaw;
            g_applied_pitch += pitch;
            gyro_deg = std::abs(yaw) + std::abs(pitch);
            written_yaw = yaw, written_pitch = pitch;
            double after_pitch = 0, after_yaw = 0;
            if (read_back && read_view_rotation(*view, s, after_pitch, after_yaw))
                g_write_back = {view->id, now_yaw, after_yaw, yaw};
        } else {
            write_failed = true;
            written_yaw = yaw, written_pitch = pitch;  // lost: carried over to the next view
        }
    }
    camera_match_frame(s, view, written_yaw, written_pitch, write_failed, rewritten);
    // Recenter after the gyro delta (GyroLib's order); it isn't gyro motion, so the follow check doesn't count it.
    if (g_recenter_fraction > 0 && view && view->id == g_recenter_view && g_recenter_supported && host.camera_allowed &&
        !probe_capturing()) {
        if (apply_recenter(*view, s, g_recenter_fraction)) g_applied_this_frame = true;
        else log("recenter: view %u rotation not written, request dropped", view->id);
    }
    follow_check(s, view, gyro_deg > 0 ? std::abs(written_yaw) : 0);
    if (g_probe) auto_probe(s);

    const ULONGLONG now = GetTickCount64();
    if (now >= g_next_status) {
        g_next_status = now + 10000;
        status(s, view);
        perf::report();
    }
}

}  // namespace

void host_configure(const std::wstring& mod_dir, const std::wstring& exe_stem) {
    g_mod_dir = mod_dir;
    g_exe_stem = exe_stem;
    g_settings_path = mod_dir + L"\\GyroUE.ini";
    g_profile_path = mod_dir + L"\\profiles\\" + exe_stem + L".ini";
    load_profile();
    nativepad::install(setting(L"NativeFlickStick", 0) != 0, setting(L"DualSenseAsDualShock4", 0) != 0);
    xinput::Settings xs;
    xs.mode = setting(L"XInput", xinput::Auto);
    xs.nintendo_labels = setting(L"XInputNintendoLabels", 0) != 0;
    xs.touchpad_back = setting(L"XInputTouchpadBack", 1) != 0;
    xs.trace = setting(L"XInputTrace", 0) != 0;
    xinput::install(xs);
}

void host_raw_input(void* raw_input_handle) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    // A HID report doesn't fit (or isn't a mouse): that read fails and is ignored.
    if (GetRawInputData(static_cast<HRAWINPUT>(raw_input_handle), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return;
    if (raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    if (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0) g_mouse_moved_at = GetTickCount64();
}

bool host_frame() {
    if (g_failed) return false;
    g_applied_this_frame = false;
    try {
        frame();
    } catch (const std::exception& e) {
        log("fatal: %s (gyro disabled for this session)", e.what());
        g_failed = true;
    } catch (...) {
        log("fatal: unknown exception (gyro disabled for this session)");
        g_failed = true;
    }
    return g_applied_this_frame;
}

}  // namespace gyroue
