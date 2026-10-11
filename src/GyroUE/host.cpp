#include "host.hpp"

#include "log.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "perf.hpp"
#include "ue.hpp"

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
#include <deque>
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

// Pacing: a jitter buffer for controllers whose motion arrives in bursts. The Switch Pro over Bluetooth measures
// a sample every ~5 ms, but its reports reach the PC 1-4 at a time with gaps of 3-68 ms (motion trace, Luto). GyroLib
// then outputs rotation only on the frames where a burst arrived, so the camera moves in steps; its smoothing can't
// help because it only advances when samples arrive. GyroUE splits each update's rotation over its samples (by
// gyro speed), queues them, and plays them back at the measured sample rate, keeping just enough queued to cover the
// usual gaps (like an audio jitter buffer). When samples arrive about every frame (DualSense) nothing is queued or
// delayed. Pacing=0 in GyroUE.ini disables it; PacingMaxMs caps the delay.
struct Pacer {
    bool enabled = true;
    double max_delay = 0.050;            // PacingMaxMs (s)
    struct Chunk {
        double pitch, yaw;
    };
    std::deque<Chunk> queue;             // one entry per sample, oldest first
    double head_used = 0;                // fraction of queue.front() already applied
    std::vector<double> weights;         // gyro speed of the samples GyroLib processed in this update
    double rate = 0;                     // measured samples per second
    uint64_t rate_start_ns = 0;
    uint32_t rate_count = 0;
    double gap_mean = 0, gap_var = 0;    // time between updates that brought samples (s)
    double frame_mean = 0;               // time between updates (s)
    double target = 0;                   // delay kept in the queue (s)
    bool active = false;                 // pacing this frame (for the status line)
    uint64_t last_frame_ns = 0, last_arrival_ns = 0;

    void on_sample(const gl_sample& sample) {
        const gl_vec3 g = sample.gyro_dps;
        weights.push_back(std::sqrt(double(g.x) * g.x + double(g.y) * g.y + double(g.z) * g.z) + 1e-3);
    }
    void reset() {
        queue.clear();
        head_used = 0;
    }
    double depth() const { return double(queue.size()) - head_used; }  // queued samples

    // Take up to `count` samples from the head of the queue into out.
    void take(double count, double& out_pitch, double& out_yaw) {
        while (count > 1e-9 && !queue.empty()) {
            const double t = std::min(count, 1.0 - head_used);
            out_pitch += queue.front().pitch * t;
            out_yaw += queue.front().yaw * t;
            head_used += t;
            count -= t;
            if (head_used >= 1.0 - 1e-9) {
                queue.pop_front();
                head_used = 0;
            }
        }
    }

    // in: this update's GyroLib output. out: what to apply now.
    void step(uint64_t now_ns, double in_pitch, double in_yaw, double& out_pitch, double& out_yaw) {
        const double dt = last_frame_ns ? double(now_ns - last_frame_ns) * 1e-9 : 0;
        last_frame_ns = now_ns;
        if (dt > 0 && dt < 0.2) frame_mean = frame_mean ? frame_mean + (dt - frame_mean) * 0.05 : dt;
        const size_t n = weights.size();
        if (n) {
            if (last_arrival_ns) {
                const double gap = double(now_ns - last_arrival_ns) * 1e-9;
                if (gap < 0.2) {  // ignore pauses (no samples while the pad sleeps or the game hitches)
                    if (!gap_mean) gap_mean = gap;
                    const double d = gap - gap_mean;
                    gap_mean += d * 0.05;
                    gap_var += (d * d - gap_var) * 0.05;
                }
            }
            last_arrival_ns = now_ns;
            if (!rate_start_ns) rate_start_ns = now_ns;
            rate_count += uint32_t(n);
            const double span = double(now_ns - rate_start_ns) * 1e-9;
            if (span >= 1.0) {
                const double r = rate_count / span;
                rate = rate ? rate + (r - rate) * 0.3 : r;
                rate_start_ns = now_ns;
                rate_count = 0;
            }
        }
        // Delay that covers nearly all gaps between bursts, capped by PacingMaxMs.
        target = std::min(gap_mean + 0.5 * std::sqrt(gap_var), max_delay);
        // Pace only when bursts are clearly further apart than updates; else samples already come every frame.
        active = enabled && rate > 0 && dt > 0 && frame_mean > 0 && gap_mean > frame_mean * 1.5;
        out_pitch = out_yaw = 0;
        if (!active) {
            take(depth() + 1, out_pitch, out_yaw);  // flush anything still queued
            reset();
            out_pitch += in_pitch, out_yaw += in_yaw;
            weights.clear();
            return;
        }
        if (n) {
            double sum = 0;
            for (double w : weights) sum += w;
            for (double w : weights) queue.push_back({in_pitch * w / sum, in_yaw * w / sum});
        } else {
            out_pitch += in_pitch, out_yaw += in_yaw;  // output with no new samples (GyroLib smoothing tail): as is
        }
        weights.clear();
        // Play back at the sample rate, slightly faster or slower to keep about `target` seconds queued.
        const double want = target * rate;
        const double speed = std::clamp(1.0 + 0.5 * (depth() - want) / std::max(want, 1.0), 0.5, 2.0);
        take(rate * dt * speed, out_pitch, out_yaw);
        // After a stall, never keep more than 1.5x the maximum delay queued.
        const double limit = max_delay * rate * 1.5;
        if (depth() > limit) take(depth() - limit, out_pitch, out_yaw);
    }
};
Pacer g_pacer;
uint32_t g_paced_view = 0;

// Motion trace (MotionTrace=1 in GyroUE.ini): 30 s of per-sample and per-frame timing in GyroUE\motion-trace.csv,
// to see where motion gets bunched (controller, transport, frame) and how pacing spreads it.
//   S,sensor_ns,arrival_ns,gyro_x,gyro_y,gyro_z         every accepted sample (GyroLib sample observer)
//   F,now_ns,new_samples,raw_yaw,raw_pitch,yaw,pitch     every frame while the gyro may turn the camera
struct Trace {
    bool wanted = false, done = false;
    FILE* file = nullptr;
    uint64_t start_ns = 0;
} g_trace;

void GL_CALL on_sample(void*, uint64_t, const gl_sample* sample) {
    if (!sample) return;
    g_pacer.on_sample(*sample);
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
    "; 1 = spread motion that a controller sends in bursts (e.g. Switch Pro, ~66 bursts/s) evenly over the frames, so\r\n"
    ";     the camera turns smoothly at high frame rates. Adds at most one burst interval of delay, and none for\r\n"
    ";     controllers that report every frame (DualSense). 0 = apply each burst as it arrives.\r\n"
    "Pacing=1\r\n"
    "; Bursts further apart than this (ms) are not paced.\r\n"
    "PacingMaxMs=50\r\n";

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

void load_profile() {
    View def;
    def.label = ini(L"View.Default", L"Label", L"Camera");
    def.property = ini(L"View.Default", L"Property");
    load_view_extras(def, L"View.Default");
    g_views = {def};
    ensure_settings_file();
    g_profile_existed = GetFileAttributesW(g_profile_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    g_probe = setting(L"Probe", 0) != 0;
    g_pacer.enabled = setting(L"Pacing", 1) != 0;
    g_trace.wanted = setting(L"MotionTrace", 0) != 0;
    if (g_trace.wanted) log("settings: MotionTrace=1 (30 s from the first gameplay frame)");
    g_pacer.max_delay = std::clamp(setting(L"PacingMaxMs", 50), 5, 100) / 1000.0;
    log("settings: Pacing=%d PacingMaxMs=%d", g_pacer.enabled ? 1 : 0, int(g_pacer.max_delay * 1000 + 0.5));
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
    gl_set_sample_observer(ctx, &on_sample, nullptr);  // pacing (and the motion trace) need every sample
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

// Writes the probe's result once there are two scored captures: the game's profile itself if the game had none
// when it started (and the same file again on later captures), otherwise <exe>.suggested.ini next to it. A result
// with no usable view never becomes the profile. Returns where it went (empty = nothing written yet).
enum class Written { Nothing, Profile, Suggested };

Written write_suggestion(bool& usable) {
    usable = false;
    std::string summary;
    const std::string probe = ue::probe_suggestion(narrow(g_exe_stem), summary, false);
    if (probe.empty()) return Written::Nothing;
    usable = probe.find("[View.Default]") != std::string::npos;
    const bool as_profile = usable && (!g_profile_existed || g_profile_created);
    const std::string text = as_profile ? ue::probe_suggestion(narrow(g_exe_stem), summary, true) : probe;
    const std::wstring path = g_mod_dir + L"\\profiles\\" + g_exe_stem + (as_profile ? L".ini" : L".suggested.ini");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0) file = nullptr;
    const bool written = file && std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (file) std::fclose(file);
    if (written && as_profile) g_profile_created = true;
    const char* what = as_profile ? "game profile" : "suggested profile";
    probe_log("probe: %s (%s)%s:\n%s", what, summary.c_str(),
              written ? (" written to " + narrow(path)).c_str() : " (could not write the file)", text.c_str());
    log("probe: %s: %s -> %s", what, summary.c_str(), written ? narrow(path).c_str() : "write failed, see GyroUE-probe.log");
    if (!written) return Written::Nothing;
    return as_profile ? Written::Profile : Written::Suggested;
}

// On-screen progress after a scored capture.
void notify_capture(int capture) {
    bool usable = false;
    const Written written = write_suggestion(usable);
    std::wstring text = capture > 0 ? L"GyroUE probe: capture " + std::to_wstring(capture) + L" done.\n"
                                    : std::wstring(L"GyroUE probe: diff done.\n");
    if (written == Written::Nothing)
        text += L"Turn the camera again with the right stick (more than 30\u00B0), then stop.";
    else if (written == Written::Profile)
        text += L"Game profile created: GyroUE\\profiles\\" + g_exe_stem + L".ini\n"
                L"Restart the game to use it, then set Probe=0 in GyroUE\\GyroUE.ini. More turns make it more reliable.";
    else if (usable)
        text += L"This game already has a profile, so the result went to GyroUE\\profiles\\" + g_exe_stem +
                L".suggested.ini\nTo use it, rename it to " + g_exe_stem + L".ini (replacing the old one) and restart.";
    else
        text += L"No camera rotation found yet. Keep turning with the right stick (see GyroUE-probe.log).";
    notify::show(text, written == Written::Nothing ? 10 : 20);
}

// Automatic capture (Probe=1): a snapshot once the camera rests, a scored diff when the player has turned it
// more than 30 degrees with the stick and stopped again. Off as soon as F7/F8 are used.
struct AutoProbe {
    bool enabled = true, armed = false, announced = false, have_last = false;
    bool stick_turn = false;      // the right stick was deflected since the snapshot
    bool stick_data = false;      // GyroLib has reported the right stick at least once (else no stick filter)
    double snap_pitch = 0, snap_yaw = 0, last_pitch = 0, last_yaw = 0;
    ue::Obj pawn = 0;
    ULONGLONG still_since = 0, snapshot_at = 0, next_snapshot = 0;
    int captures = 0;
} g_auto;
constexpr int kMaxAutoCaptures = 8;

// While the automatic capture runs, the gyro output is paused: a turn must come from the right stick alone, and the
// gyro written into a rotation the camera ignores (or a resting controller's drift) would only spoil it.
bool probe_capturing() { return g_probe && g_auto.enabled && g_auto.captures < kMaxAutoCaptures; }

// The right stick is deflected (GyroLib's view of the selected controller, SDL or a bound companion).
bool right_stick_moving() {
    gl_flick_input fi{};
    if (gl_get_flick_input(g_gyro->get(), &fi) != GL_OK || !(fi.available & GL_FLICK_INPUT_STICK)) return false;
    g_auto.stick_data = true;
    return std::hypot(fi.stick_x, fi.stick_y) > 0.3f;
}

void auto_probe(const Scene& s) {
    const ULONGLONG now = GetTickCount64();
    if (!g_auto.enabled || g_auto.captures >= kMaxAutoCaptures || !s.pov_ok || !s.pawn || s.paused || s.cursor ||
        !game_has_focus()) {
        g_auto.armed = g_auto.have_last = false;
        return;
    }
    const double motion = g_auto.have_last ? std::abs(wrap180(s.pov_yaw - g_auto.last_yaw)) +
                                                 std::abs(s.pov_pitch - g_auto.last_pitch)
                                           : 1;
    if (motion > 0.05 || !g_auto.have_last) g_auto.still_since = now;
    g_auto.last_pitch = s.pov_pitch, g_auto.last_yaw = s.pov_yaw, g_auto.have_last = true;
    const ULONGLONG still_for = now - g_auto.still_since;

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
        if (ue::probe_diff(false) > 0)
            notify_capture(g_auto.captures);
        else
            notify::show(L"GyroUE probe: capture " + std::to_wstring(g_auto.captures) +
                         L" could not be compared (see GyroUE-probe.log).\nKeep turning with the right stick.");
        if (g_auto.captures >= kMaxAutoCaptures) log("probe: %d automatic captures done, automatic capture off", g_auto.captures);
        return;
    }
    if (still_for >= 500 && now >= g_auto.next_snapshot) {
        take_snapshot(s);
        g_auto.armed = true;
        g_auto.snap_pitch = s.pov_pitch, g_auto.snap_yaw = s.pov_yaw;
        g_auto.stick_turn = false;
        g_auto.pawn = s.pawn;
        g_auto.snapshot_at = now;
        if (!g_auto.announced) {
            g_auto.announced = true;
            log("probe: automatic capture ready: turn the camera with the RIGHT STICK (more than 30 degrees), stop, "
                "and repeat 3-4 times; the gyro is paused until the captures are done");
            notify::show(L"GyroUE probe ready to find cameras. In game, turn the camera with "
                         L"the RIGHT STICK (more than 30°), then stop. Repeat 3-4 times.", 30);
        }
    }
}

void probe_keys(const Scene& s) {
    const bool focused = game_has_focus();
    const bool snap = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool diff = focused && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if ((snap || diff) && g_auto.enabled) {
        g_auto.enabled = g_auto.armed = false;
        log("probe: F7/F8 used, automatic capture off for this session");
    }
    if (snap && !g_probe_snap_down) take_snapshot(s);
    if (diff && !g_probe_diff_down && ue::probe_diff(true) > 0) notify_capture(0);
    g_probe_snap_down = snap;
    g_probe_diff_down = diff;
}

// Step 1 of finding a camera: tells the user when the gyro turns the view's rotation but the camera doesn't
// follow (the game ignores ControlRotation, or the profile's rotation), or confirms that it does. Only on the
// player's own camera: scripted or fixed cameras (another view target) ignore every rotation by design.
struct FollowCheck {
    double applied = 0, moved = 0, last_pitch = 0, last_yaw = 0, total_applied = 0, total_moved = 0;
    bool have_last = false, warned = false, confirmed = false;
    int ignored = 0, followed = 0;  // 2 s windows with enough gyro motion where the camera ignored / followed it
    bool stick = false;             // the right stick moved in this window: the camera motion can't be judged
    ULONGLONG window_end = 0;
} g_follow;

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
    if (!view || gyro_deg <= 0 || s.paused || s.cursor || !s.pawn) return;
    if (view->view_target_class.empty() && s.view_target != s.pawn) return;
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
    log("status: motion %.0f samples/s, bursts every %.1f ms, updates every %.1f ms -> pacing %s", g_pacer.rate,
        g_pacer.gap_mean * 1000, g_pacer.frame_mean * 1000,
        !g_pacer.enabled ? "off (Pacing=0)"
        : g_pacer.active ? std::format("on, {:.0f} ms buffer", g_pacer.target * 1000).c_str()
                         : "not needed");
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

    double gyro_deg = 0;  // gyro rotation actually written this frame
    double pitch = 0, yaw = 0;
    g_pacer.step(now_ns, out.pitch_degrees, out.yaw_degrees, pitch, yaw);
    if (g_trace.wanted) {
        gl_gyro_state gs{};
        gl_get_gyro_state(g_gyro->get(), &gs);
        trace_frame(now_ns, view && host.camera_allowed && gs.enabled, gs.new_samples, out, pitch, yaw);
    }
    if (!view || !host.camera_allowed || probe_capturing() || view_id != g_paced_view) g_pacer.reset();  // never carry over
    g_paced_view = view_id;
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
