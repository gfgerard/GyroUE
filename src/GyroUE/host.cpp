#include "host.hpp"

#include "deps.hpp"
#include "log.hpp"
#include "nativepad.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "perf.hpp"
#include "ue.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.hpp>
#include <gyrolib/version.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <vector>

namespace gyroue {
namespace {

// A gameplay view from the game profile. The default view (id 1) turns the PlayerController's
// ControlRotation; other views add the deltas to a rotator property of the camera view target.
// Any view can name another object (Object=Root.Sub..., Root = PC, Pawn, Camera or ViewTarget; each Sub is an
// object property or a subobject name) and, for non-reflected C++ state, raw rotator offsets in it.
struct View {
    uint32_t id = 1;
    std::string key = "Default", label = "Camera";
    std::string view_target_class;  // active when the view target's class chain has this class
    std::string property;           // rotator property on the view object ("" = ControlRotation)
    std::vector<std::string> object;  // path to the view object (empty = PC for the default view, else ViewTarget)
    std::vector<int32_t> offsets;     // raw rotator offsets in the view object (instead of Property)
    int32_t rotator_size = 24;        // 24 = double (UE5), 12 = float (UE4)
    // WorldRotation=1: Property is a scene component's RelativeRotation with absolute rotation; its world rotation
    // (ComponentToWorld) is written too, because the game turns the component from it (Severed Steel's camera).
    bool world_rotation = false;
    const char* world_why = nullptr;  // last reason it couldn't be applied (logged when it changes)
    uint32_t parent = 0;            // settings inheritance
    // Optional: scalar properties written whenever the gyro moves this view (e.g. to cancel a camera
    // auto-recentering that the game only cancels for stick input). "Name:value" is on the view object;
    // "Root.Sub.Name:value" on another object, with the same path syntax as Object= (Life is Strange:
    // OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1).
    struct OnGyro {
        std::vector<std::string> object;  // empty = the view object
        std::string property;
        double value = 0;
        bool warned = false;
    };
    std::vector<OnGyro> on_gyro;
    // Diagnostics: log these properties whenever they change; Dump=1 logs all properties once.
    std::vector<std::string> watch;
    bool dump = false, dumped = false;
    std::vector<std::string> watched_values;
};

std::wstring g_mod_dir, g_profile_path, g_exe_stem;
std::wstring g_settings_path;  // GyroUE\GyroUE.ini (all games)
// The game had a profile when it started; the probe writes the profile itself only if it didn't (and then keeps
// updating the one it created this session), else <exe>.suggested.ini.
bool g_profile_existed = false, g_profile_created = false;
std::vector<View> g_views;
// Diagnostics: [GyroUE] Probe=1 captures the camera state automatically on stick turns (see auto_probe) and
// enables F7 (snapshot) / F8 (log what changed since the snapshot) for manual captures.
bool g_probe = false, g_probe_snap_down = false, g_probe_diff_down = false;

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
// The camera's POV is read every frame only while something needs it (the follow check until it has a verdict,
// or the probe).
bool g_need_pov = true;

std::string ini(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(section, key, fallback, buffer, 2048, g_profile_path.c_str());
    return narrow(buffer);
}

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(sep, start);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}

void load_view_extras(View& v, const wchar_t* section) {
    v.object = split(ini(section, L"Object"), '.');
    for (const std::string& o : split(ini(section, L"Offsets"), ','))
        v.offsets.push_back(int32_t(std::strtol(o.c_str(), nullptr, 0)));
    v.rotator_size = GetPrivateProfileIntW(section, L"RotatorSize", 24, g_profile_path.c_str());
    v.world_rotation = GetPrivateProfileIntW(section, L"WorldRotation", 0, g_profile_path.c_str()) != 0;
    v.watch = split(ini(section, L"Watch"), ',');
    v.watched_values.assign(v.watch.size(), std::string{});
    v.dump = GetPrivateProfileIntW(section, L"Dump", 0, g_profile_path.c_str()) != 0;
    for (const std::string& item : split(ini(section, L"OnGyro"), ',')) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::vector<std::string> path = split(item.substr(0, colon), '.');
        if (path.empty()) continue;
        View::OnGyro w;
        w.property = path.back();
        path.pop_back();
        w.object = std::move(path);
        w.value = std::atof(item.c_str() + colon + 1);
        v.on_gyro.push_back(std::move(w));
    }
}

std::string probe_header() {
    return "GyroUE " GYROUE_VERSION " camera probe (recreated every game session)\n"
           "\n"
           "How to use it (automatic):\n"
           "  1. In gameplay, stand still for a second. The gyro is paused while the probe captures.\n"
           "  2. Turn the camera with the RIGHT STICK: more than 30 degrees sideways (and a bit up or down),\n"
           "     then stop. GyroUE captures the turn by itself (turns without the stick, e.g. cutscenes, are ignored).\n"
           "  3. Repeat 3 or 4 times. From the second capture on, the game's profile is written to\n"
           "     GyroUE\\profiles\\<game>.ini, or to <game>.suggested.ini if the game already had a profile\n"
           "     (and copied at the end of this file). Restart the game to use it; set Probe=0 in GyroUE\\GyroUE.ini\n"
           "     once the camera works.\n"
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
    ";     RIGHT STICK (more than 30 degrees) and stop, 3-4 times (the gyro is paused meanwhile). GyroUE then writes the game's\r\n"
    ";     profile (GyroUE\\profiles\\<game exe>.ini, or <game exe>.suggested.ini if the game already has one).\r\n"
    ";     Set it back to 0 once the camera works. Results: GyroUE\\GyroUE-probe.log.\r\n"
    "Probe=0\r\n"
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
    "NativeFlickStick=0\r\n";

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

// The game's profile describes camera views (a profile can also hold only [GyroUE] settings, e.g. the DualSense
// learning below): the probe then writes its result as a suggestion instead.
bool profile_has_views() {
    wchar_t buffer[64];
    return GetPrivateProfileSectionW(L"View.Default", buffer, 64, g_profile_path.c_str()) > 0 ||
           GetPrivateProfileStringW(L"GyroUE", L"Views", L"", buffer, 64, g_profile_path.c_str()) > 0;
}

void load_profile() {
    View def;
    def.label = ini(L"View.Default", L"Label", L"Camera");
    def.property = ini(L"View.Default", L"Property");
    load_view_extras(def, L"View.Default");
    g_views = {def};
    ensure_settings_file();
    g_profile_existed = profile_has_views();
    g_probe = setting(L"Probe", 0) != 0;
    g_trace.wanted = setting(L"MotionTrace", 0) != 0;
    if (g_trace.wanted) log("settings: MotionTrace=1 (30 s from the first gameplay frame)");
    // On-screen notices (camera check, probe progress); Notices=0 keeps them in the log only.
    const bool notices = setting(L"Notices", 1) != 0;
    log("settings: Probe=%d Notices=%d (GyroUE.ini, the game's profile overrides)", g_probe ? 1 : 0, notices ? 1 : 0);
    notify::set_enabled(notices);
    if (g_probe) {
        const std::wstring probe_path = g_mod_dir + L"\\GyroUE-probe.log";
        const bool opened = probe_log_open(probe_path, probe_header());
        log("profile: probe enabled (automatic capture on stick turns, F7/F8 manual), results in %s", opened ? narrow(probe_path).c_str()
                                                                                  : "GyroUE.log (probe file failed)");
    }
    if (GetFileAttributesW(g_profile_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log("profile: %s not found, using the generic ControlRotation view only", narrow(g_profile_path).c_str());
        return;
    }
    wchar_t list[512];
    GetPrivateProfileStringW(L"GyroUE", L"Views", L"", list, 512, g_profile_path.c_str());
    std::wstring all = list;
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(L',', start);
        if (end == std::wstring::npos) end = all.size();
        std::wstring key = all.substr(start, end - start);
        start = end + 1;
        while (!key.empty() && key.front() == L' ') key.erase(key.begin());
        while (!key.empty() && key.back() == L' ') key.pop_back();
        if (key.empty()) continue;
        const std::wstring section = L"View." + key;
        View v;
        v.key = narrow(key);
        v.id = GetPrivateProfileIntW(section.c_str(), L"Id", 0, g_profile_path.c_str());
        v.label = ini(section.c_str(), L"Label", key.c_str());
        v.view_target_class = ini(section.c_str(), L"ViewTargetClass");
        v.property = ini(section.c_str(), L"Property");
        v.parent = GetPrivateProfileIntW(section.c_str(), L"Parent", 1, g_profile_path.c_str());
        load_view_extras(v, section.c_str());
        if (v.id <= 1 || v.view_target_class.empty() || (v.property.empty() && v.offsets.empty())) {
            log("profile: view %s ignored (needs Id > 1, ViewTargetClass and Property or Offsets)", v.key.c_str());
            continue;
        }
        g_views.push_back(v);
    }
    log("profile: %s, %zu view(s)", narrow(g_profile_path).c_str(), g_views.size());
    for (const View& v : g_views) {
        std::string object, where;
        for (const std::string& part : v.object) object += (object.empty() ? "" : ".") + part;
        for (int32_t off : v.offsets) where += (where.empty() ? "raw +0x" : ",0x") + std::format("{:X}", off);
        if (where.empty()) where = v.property.empty() ? "ControlRotation" : v.property;
        if (v.world_rotation) where += v.offsets.empty() ? " (WorldRotation)" : " (WorldRotation ignored: needs Property=)";
        log("  view %u '%s' target=%s object=%s rotation=%s parent=%u", v.id, v.label.c_str(),
            v.view_target_class.empty() ? "(pawn)" : v.view_target_class.c_str(),
            object.empty() ? "(default)" : object.c_str(), where.c_str(), v.parent);
    }
    for (const View& v : g_views)
        if (!v.on_gyro.empty() || !v.watch.empty() || v.dump)
            log("  view %u extras: %zu OnGyro write(s), %zu watch(es), dump=%d", v.id, v.on_gyro.size(), v.watch.size(), v.dump);
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
    for (const View& v : g_views) g_gyro->register_view(v.id, v.label.c_str());
    for (const View& v : g_views)
        if (v.parent) gl_set_context_parent(ctx, v.id, v.parent);
    gl_set_host_capabilities(ctx, GL_HOST_MENU_STATE);

    // Mod defaults (an existing gyrolib.ini overrides them): always on, calibrate in menus, no flick stick.
    gl_setting_set(ctx, "gyro.activation", 0);
    gl_setting_set(ctx, "calibration.automatic", 1);
    gl_setting_set(ctx, "flick.mode", 0);
    const std::string dir = narrow(g_mod_dir);
    g_gyro->initialize_settings(dir.c_str());
    log("gyro: settings %s (menu key F%u, gamepad shortcut %u)", gl_get_settings_path(ctx), gl_get_menu_key(ctx),
        gl_get_gamepad_menu_shortcut(ctx));

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
    s.pov_ok = (g_need_pov || g_probe) && ue::camera_pov(camera, s.pov_pitch, s.pov_yaw);
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

View* active_view(const Scene& s) {
    if (s.view_target) {
        for (View& v : g_views)
            if (!v.view_target_class.empty() && ue::is_a(s.view_target, v.view_target_class.c_str())) return &v;
    }
    // Main menus usually possess a placeholder DefaultPawn (SpectatorPawn derives from it): no gameplay camera.
    if (!s.pawn || ue::is_a(s.pawn, "DefaultPawn")) return nullptr;
    return &g_views.front();
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

Written write_suggestion(bool& usable, bool& verified) {
    usable = verified = false;
    std::string summary;
    const std::string probe = ue::probe_suggestion(narrow(g_exe_stem), summary, false, verified);
    if (probe.empty()) return Written::Nothing;
    usable = probe.find("[View.Default]") != std::string::npos;
    const bool as_profile = usable && verified && (!g_profile_existed || g_profile_created);
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

// While the automatic probe runs, the gyro output is paused: a turn must come from the right stick alone, and the
// gyro written into a rotation the camera ignores (or a resting controller's drift) would only spoil it.
bool probe_capturing() { return g_probe && g_auto.enabled && !g_auto.done; }

// The right stick is deflected (GyroLib's view of the selected controller, SDL or a bound companion).
bool right_stick_moving() {
    gl_flick_input fi{};
    if (gl_get_flick_input(g_gyro->get(), &fi) != GL_OK || !(fi.available & GL_FLICK_INPUT_STICK)) return false;
    g_auto.stick_data = true;
    return std::hypot(fi.stick_x, fi.stick_y) > 0.3f;
}

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

void finish_tests() {
    g_test.phase = LiveTest::Off;
    bool usable = false, verified = false;
    const Written written = write_suggestion(usable, verified);
    const std::wstring file = L"GyroUE\\profiles\\" + g_exe_stem + (written == Written::Profile ? L".ini" : L".suggested.ini");
    if (verified) {
        g_auto.done = true;
        log("probe: done, a rotation moved the camera in both rounds of the live test");
        notify::show(written == Written::Profile
                         ? L"GyroUE probe: camera found and VERIFIED live.\nProfile created: " + file +
                               L"\nRestart the game to use it, then set Probe=0 in GyroUE\\GyroUE.ini."
                         : L"GyroUE probe: camera found and VERIFIED live.\nThis game already has a profile, so it went to " +
                               file + L"\nRename it to " + g_exe_stem + L".ini (replacing the old one) and restart.",
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
        return;
    }
    notify::show(L"GyroUE probe: none of these rotations moved the camera.\nTurn the camera again with the RIGHT "
                 L"STICK (more than 30°), then stop, to find more.",
                 15);
}

// One frame of the live test. valid: in game, unpaused, no cursor, focused (the same rules as the captures).
void run_live_test(const Scene& s, bool valid, ULONGLONG still_for) {
    const ULONGLONG now = GetTickCount64();
    const bool writing = g_test.phase != LiveTest::Settle && g_test.phase != LiveTest::Null;
    if (!valid || !s.pov_ok || ue::class_of(s.view_target) != g_auto.target_class || right_stick_moving()) {
        if (writing || g_test.phase == LiveTest::Null)
            abort_test(s, !valid ? "left gameplay" : !s.pov_ok ? "no camera" : ue::class_of(s.view_target) != g_auto.target_class
                                                                                  ? "another camera" : "stick moved");
        return;
    }
    const double dy = wrap180(s.pov_yaw - g_test.base_y), dp = s.pov_pitch - g_test.base_p;
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
        if (g.property.find('.') != std::string::npos) return test_result(ue::kUntestable, "nested in a struct");
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
                                                     std::abs(s.pov_pitch - g_auto.last_pitch)
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
        if (right_stick_moving()) g_auto.stick_turn = true;
        if (s.pawn != g_auto.pawn || now - g_auto.snapshot_at > 120000) {  // other pawn, or stale
            g_auto.armed = false;
            return;
        }
        const double dy = std::abs(wrap180(s.pov_yaw - g_auto.snap_yaw)), dp = std::abs(s.pov_pitch - g_auto.snap_pitch);
        if (still_for < 300 || dy < 30) return;  // still turning, or not far enough yet
        g_auto.armed = false;
        g_auto.next_snapshot = now + 1500;
        // The camera turned without the right stick: a cutscene or a scripted camera, not the player's look input.
        if (g_auto.stick_data && !g_auto.stick_turn) {
            probe_log("probe: turn of %.0f degrees ignored: the right stick wasn't used (cutscene or scripted camera)", dy);
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
                         ? std::format(L"GyroUE probe: capture {} of {} done.\nTurn the camera again with the RIGHT STICK "
                                       L"(more than 30°, also up or down), then stop.",
                                       g_auto.captures, kCapturesForTest)
                         : std::format(L"GyroUE probe: capture {} done, no new rotation to test yet.\nTurn the camera again "
                                       L"with the RIGHT STICK (more than 30°), then stop.",
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
            log("probe: automatic capture ready: turn the camera with the RIGHT STICK (more than 30 degrees), stop, "
                "and repeat; after %d captures the rotations found are tested live; the gyro is paused meanwhile",
                kCapturesForTest);
            notify::show(L"GyroUE probe ready to find the camera. In game, turn the camera with the RIGHT STICK "
                         L"(more than 30°), then stop. Repeat 3 times.\nThen don't touch the controller while "
                         L"the probe tests what it found.",
                         30);
        }
    }
}

void probe_keys(const Scene& s) {
    const bool focused = game_has_focus();
    const bool snap = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool diff = focused && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
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

// Step 1 of finding a camera: tells the user when the gyro turns the view's rotation but the camera doesn't
// follow (the game ignores ControlRotation, or the profile's rotation), or confirms that it does. Only on the
// player's own camera (see player_camera): scripted or fixed cameras ignore every rotation by design.
struct FollowCheck {
    double applied = 0, moved = 0, last_pitch = 0, last_yaw = 0, total_applied = 0, total_moved = 0;
    bool have_last = false, warned = false, confirmed = false;
    int ignored = 0, followed = 0;  // 2 s windows with enough gyro motion where the camera ignored / followed it
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
    if (right_stick_moving()) g_follow.stick_moved += moved;
    if (g_follow.stick_moved < 30) return false;
    g_follow.player_cameras.push_back(cls);
    log("camera: view target %s turns with the right stick: treated as the player's camera", ue::name_of(cls).c_str());
    return true;
}

void follow_check(const Scene& s, const View* view, double gyro_deg) {
    if (g_follow.warned || g_follow.confirmed) return;  // verdict given: off for the session
    if (!s.pov_ok) {
        g_follow.have_last = false;
        return;
    }
    const double moved = g_follow.have_last ? std::abs(wrap180(s.pov_yaw - g_follow.last_yaw)) +
                                                  std::abs(s.pov_pitch - g_follow.last_pitch)
                                            : 0;
    g_follow.last_pitch = s.pov_pitch, g_follow.last_yaw = s.pov_yaw, g_follow.have_last = true;
    if (!view || s.paused || s.cursor || !s.pawn) return;
    if (view->view_target_class.empty() && !player_camera(s, moved)) return;
    if (gyro_deg <= 0) return;
    g_follow.applied += gyro_deg;
    g_follow.moved += moved;
    if (right_stick_moving()) g_follow.stick = true;
    const ULONGLONG now = GetTickCount64();
    if (!g_follow.window_end) g_follow.window_end = now + 2000;
    if (now < g_follow.window_end) return;
    if (g_follow.applied >= 10 && !g_follow.stick) {  // enough gyro motion in this window to judge, no stick
        const double ratio = g_follow.moved / g_follow.applied;
        if (ratio < 0.2) ++g_follow.ignored;
        else if (ratio > 0.5) ++g_follow.followed;
        g_follow.total_applied += g_follow.applied;
        g_follow.total_moved += g_follow.moved;
    }
    g_follow.applied = g_follow.moved = 0;
    g_follow.window_end = 0;
    g_follow.stick = false;
    std::string rotation = view->property.empty() ? "ControlRotation" : view->property;
    if (!view->offsets.empty()) rotation = "the profile's raw offsets";
    if (!g_follow.warned && g_follow.ignored >= 3 && g_follow.followed == 0) {
        g_follow.warned = true;
        log("camera: the gyro turned %s (view '%s') by %.0f degrees but the camera moved only %.0f: the game ignores it. "
            "To find the camera's real rotation, set Probe=1 in %s, restart and turn the camera with the right stick "
            "(ADVANCED.md: \"Finding the camera of a new game\").",
            rotation.c_str(), view->label.c_str(), g_follow.total_applied, g_follow.total_moved, narrow(g_settings_path).c_str());
        if (g_probe)
            notify::show(L"GyroUE: the gyro doesn't move this camera yet.\nThe probe is on: turn the camera with the "
                         L"right stick to find the right rotation.", 12);
        else if (view->offsets.empty() && view->property.empty())
            notify::show(L"GyroUE: this game's camera seems to ignore the gyro.\nTo find it, set Probe=1 in GyroUE\\GyroUE.ini, "
                         L"restart the game. Ignore this if it's actually working.", 15);
        else
            notify::show(L"GyroUE: the camera doesn't follow the profile's rotation.\nThe profile may be outdated: "
                         L"set Probe=1 in GyroUE\\GyroUE.ini to find the camera again.", 15);
        g_need_pov = false;
    }
    if (!g_follow.confirmed && g_follow.followed >= 3) {
        g_follow.confirmed = true;
        log("camera: confirmed, the camera follows the gyro (%s, view '%s')", rotation.c_str(), view->label.c_str());
        g_need_pov = false;
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

void frame() {
    if (!g_gyro && !start_gyro()) {
        g_failed = true;
        return;
    }
    check_settings_file();

    // Settings panel: attach once the game's DX12 swapchain is known. Without a renderer, never
    // leave the (invisible) panel open, or it would hold the gyro suspended.
    overlay::attach(g_gyro->get());
    overlay::poll_menu_key(g_gyro->get(), game_has_focus());
    overlay::process();
    if (!overlay::available() && gl_panel_open(g_gyro->get())) gl_set_panel_open(g_gyro->get(), 0);

    const Scene s = read_scene();
    engine_check();
    if (g_probe) probe_keys(s);
    View* view = active_view(s);
    if (view) diagnostics(*view, s);
    const uint32_t view_id = view ? view->id : 0;
    if (view_id != g_last_view) {
        g_last_view = view_id;
        log("view: %s (target %s)", view ? view->label.c_str() : "none", ue::name_of(ue::class_of(s.view_target)).c_str());
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
    const gl_output out = g_input->update(now_ns, host, view_id);
    // GyroLib's flick stick needs the game's own right-stick rotation suppressed: possible for the controller shown to
    // the game as XInput, or when the game reads the controller itself through a checked report layout.
    SDL_Gamepad* pad = xinput::selected_gamepad(g_gyro->get(), g_input->get(), out);
    const bool xinput_shown = xinput::frame(g_gyro->get(), g_input->get(), out);
    const bool native = nativepad::frame(pad, out.suppress_native_right_stick != 0 && !xinput_shown);
    learn_dualsense();
    if ((xinput_shown || native) != g_stick_suppression) {
        g_stick_suppression = xinput_shown || native;
        gl_set_host_capabilities(g_gyro->get(), GL_HOST_MENU_STATE | (g_stick_suppression ? GL_HOST_NATIVE_STICK_SUPPRESSION : 0u));
    }

    double gyro_deg = 0;  // gyro rotation actually written this frame
    const double pitch = out.pitch_degrees, yaw = out.yaw_degrees;
    {
        gl_gyro_state gs{};
        gl_get_gyro_state(g_gyro->get(), &gs);
        const bool camera = view && host.camera_allowed && gs.enabled;
        g_delivery.on_update(camera, gs.new_samples, yaw != 0 || pitch != 0);
        trace_frame(now_ns, camera, gs.new_samples, out, pitch, yaw);
    }
    if (view && host.camera_allowed && !probe_capturing() && (yaw != 0 || pitch != 0)) {
        if (apply(*view, s, pitch, yaw)) {
            g_applied_this_frame = true;
            g_applied_yaw += yaw;
            g_applied_pitch += pitch;
            gyro_deg = std::abs(yaw) + std::abs(pitch);
        }
    }
    follow_check(s, view, gyro_deg);
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
