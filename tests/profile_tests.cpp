// Unit tests for the engine-neutral parts of GyroUE: the game profile (profile.cpp) and the Match=Camera choice
// (camera_match.hpp). Built with GyroUE (target gyroue_tests); run with tools/test-gyroue.sh.
#include "camera_match.hpp"
#include "profile.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace gyroue;

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                          \
    } while (0)

std::wstring temp_profile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"gyroue_tests_" + name + L".ini";
    DeleteFileW(path.c_str());
    return path;
}

void write_file(const std::wstring& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string ini_value(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[512];
    GetPrivateProfileStringW(section, key, L"", buffer, 512, path.c_str());
    char out[512];
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out, 512, nullptr, nullptr);
    return out;
}

const View* by_id(const std::vector<View>& views, uint32_t id) {
    for (const View& v : views)
        if (v.id == id) return &v;
    return nullptr;
}

// ---------------------------------------------------------------- CameraMatch

CameraMatch::Event step(CameraMatch& m, uint64_t now, double yaw, double moved, bool look = false,
                        bool write_failed = false, int rewritten = -1) {
    CameraMatch::Frame f;
    f.yaw = yaw, f.moved = moved, f.look = look, f.write_failed = write_failed, f.rewritten = rewritten;
    return m.frame(f, now);
}

// A simulated game, frame by frame (10 ms), driven the way host.cpp drives CameraMatch: the gyro writes `gyro` degrees
// of yaw into the current member's rotation; the camera follows only `driver` (0 = a cutscene). A member in
// `rewritten_by_game` that isn't the driver gets its old value put back by the game a frame later (an output, like
// PAYDAY 3's ControlRotation in a clamped action); any other non-driver keeps the write but the camera ignores it.
struct Sim {
    CameraMatch m;
    uint64_t now = 1000;
    uint32_t driver = 1;
    std::vector<uint32_t> rewritten_by_game;
    double value[16] = {};  // each member's rotation (yaw)
    uint32_t last_view = 0;
    double before = 0, after = 0, written = 0;
    double carried = 0;       // gyro handed over on switches
    double camera_moved = 0;  // total camera rotation
    CameraMatch::Event last = CameraMatch::None;

    explicit Sim(std::vector<uint32_t> members) { m.select(members); }

    bool rewrites(uint32_t id) const {
        return id != driver && std::find(rewritten_by_game.begin(), rewritten_by_game.end(), id) != rewritten_by_game.end();
    }
    // Runs `ms` milliseconds; returns the last non-None event.
    CameraMatch::Event run(uint64_t ms, double gyro = 0.3, double pitch = 0) {
        last = CameraMatch::None;
        for (uint64_t end = now + ms; now < end; now += 10) {
            const uint32_t id = m.current();
            // The game's frame: an output member gets its old value back.
            if (last_view && rewrites(last_view)) value[last_view] = before;
            int verdict = -1;
            if (last_view == id) verdict = CameraMatch::rewritten(before, after, value[id], written);
            before = value[id];
            value[id] += gyro;
            after = value[id];
            written = gyro;
            last_view = id;
            const double moved = id == driver ? gyro : 0;
            camera_moved += moved;
            CameraMatch::Frame f;
            f.yaw = gyro, f.pitch = pitch, f.moved = moved, f.rewritten = verdict;
            const CameraMatch::Event e = m.frame(f, now);
            if (e == CameraMatch::Switched) {
                carried += std::abs(m.carry_yaw());
                if (m.current() == driver) camera_moved += std::abs(m.carry_yaw());
                last_view = 0;
            }
            if (e != CameraMatch::None) last = e;
        }
        return last;
    }
};

void test_rewritten_helper() {
    std::puts("match: rewritten() tells a put-back write from a kept one");
    CHECK(CameraMatch::rewritten(10, 10.5, 10.5, 0.5) == 0);          // kept
    CHECK(CameraMatch::rewritten(10, 10.5, 10.0, 0.5) == 1);          // put back
    CHECK(CameraMatch::rewritten(10, 10.5, 10.02, 0.5) == 1);         // put back, almost exactly
    CHECK(CameraMatch::rewritten(10, 10.01, 10.0, 0.01) == -1);       // too small to tell
    CHECK(CameraMatch::rewritten(10, 10.5, 13.0, 0.5) == -1);         // moved for other reasons
    CHECK(CameraMatch::rewritten(179.8, -179.7, 179.8, 0.5) == 1);    // across +-180
    CHECK(CameraMatch::rewritten(179.8, -179.7, -179.7, 0.5) == 0);
    CHECK(CameraMatch::rewritten(10, 9.5, 10.0, -0.5) == 1);          // negative writes
}

void test_match_single_member() {
    std::puts("match: one member never switches");
    Sim s({1});
    s.driver = 0;
    CHECK(s.run(5000) == CameraMatch::None);
    CHECK(s.m.current() == 1);
    CHECK(!s.m.active());
}

void test_match_base_follows() {
    std::puts("match: the base view stays while the camera follows it");
    Sim s({1, 5});
    CHECK(s.m.active());
    CHECK(s.run(4000) == CameraMatch::None);
    CHECK(s.m.current() == 1);
    CHECK(s.carried == 0);
}

void test_match_fast_path() {
    std::puts("match: a base view the game rewrites is dropped within a few frames, its gyro carried over");
    Sim s({1, 5});
    s.rewritten_by_game = {1, 5};
    s.run(1000);
    s.driver = 5;  // the clamped action starts: ControlRotation becomes an output
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 1000 && e != CameraMatch::Switched) e = s.run(10);
    CHECK(e == CameraMatch::Switched);
    CHECK(s.m.reason() == CameraMatch::Rewritten);
    CHECK(s.now - start <= 60);
    CHECK(s.m.current() == 5);
    CHECK(s.carried > 0.8 && s.carried < 1.6);  // the 3-4 writes that were put back
    CHECK(s.run(500) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
}

void test_match_slow_path() {
    std::puts("match: a rotation the camera ignores without rewriting is dropped by the camera check, carried over");
    Sim s({1, 5});
    s.run(1000);
    s.driver = 5;  // ControlRotation keeps the writes, but the camera now ignores it
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 1000 && e != CameraMatch::Switched) e = s.run(10);
    CHECK(e == CameraMatch::Switched);
    CHECK(s.m.reason() == CameraMatch::Ignored);
    CHECK(s.now - start >= CameraMatch::kJudgeMinMs && s.now - start <= 400);
    CHECK(s.carried >= CameraMatch::kJudgeDegrees);
    CHECK(s.m.current() == 5);
}

void test_match_scene_round_trip() {
    std::puts("match: normal play -> clamped action -> normal play, no gyro lost");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.run(2000);
    CHECK(s.m.current() == 1);
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
    s.driver = 1;  // the action ends; the modifier's rotation is just ignored now
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 1);
    CHECK(s.run(2000) == CameraMatch::None);
    // Everything written reached the camera, directly or carried over, and no more (to a frame or two).
    const double total = 0.3 * 600;
    CHECK(std::abs(s.camera_moved - total) < 0.7);
    CHECK(s.carried > 1);
}

void test_match_single_rewrite_doesnt_switch() {
    std::puts("match: one write put back (recoil, a clamp) doesn't drop the view");
    CameraMatch m;
    m.select({1, 5});
    uint64_t now = 1000;
    CHECK(step(m, now, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 0) == CameraMatch::None);  // kept: streak over
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_lagging_camera_follows() {
    std::puts("match: a camera that follows with lag isn't taken as ignoring the gyro");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    CHECK(step(m, now, 3, 0) == CameraMatch::None);
    for (int i = 1; i <= 15; ++i) CHECK(step(m, now + i * 10, 0, 0.2) == CameraMatch::None);
    CHECK(step(m, now + 210, 0, 0) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_needs_enough_gyro_and_time() {
    std::puts("match: no camera-check verdict below 2 degrees or before 200 ms");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    for (int i = 0; i < 15; ++i) CHECK(step(m, now + i * 10, 0.1, 0) == CameraMatch::None);  // 1.5 degrees
    CHECK(m.current() == 1);
    CameraMatch f;
    f.select({1, 5});
    CHECK(step(f, now, 5, 0) == CameraMatch::None);
    CHECK(step(f, now + 100, 0, 0) == CameraMatch::None);
    CHECK(step(f, now + 210, 0, 0) == CameraMatch::Switched);
    CHECK(f.carry_yaw() == 5);
}

void test_match_look_input_resets() {
    std::puts("match: stick or mouse input prevents a verdict");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    for (int i = 0; i < 200; ++i) CHECK(step(m, now + i * 10, 0.3, 0, true, false, 1) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_cutscene() {
    std::puts("match: a cutscene ignores every view -> base view, rest, nothing carried; then found again");
    Sim s({1, 5, 6});
    s.driver = 0;
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 3000 && e != CameraMatch::AllIgnored) e = s.run(10);
    CHECK(e == CameraMatch::AllIgnored);
    CHECK(s.m.current() == 1);
    CHECK(s.m.carry_yaw() == 0);
    const double carried = s.carried;
    CHECK(s.run(CameraMatch::kRestMs - 400) == CameraMatch::None);  // resting
    CHECK(s.carried == carried);
    s.driver = 5;
    CHECK(s.run(2000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
}

void test_match_write_failed() {
    std::puts("match: a view whose object is missing is dropped at once, with that frame's gyro carried over");
    CameraMatch m;
    m.select({1, 5});
    uint64_t now = 1000;
    for (int i = 0; i < 30; ++i) step(m, now += 10, 0.3, 0);
    CHECK(m.current() == 5);
    for (int i = 0; i < 30; ++i) step(m, now += 10, 0.3, 0.3);
    CHECK(step(m, now += 10, 0.4, 0, false, true) == CameraMatch::Switched);
    CHECK(m.reason() == CameraMatch::Missing);
    CHECK(m.current() == 1);
    CHECK(m.carry_yaw() == 0.4);
    CameraMatch n;
    n.select({1, 5});
    CHECK(step(n, now, 0.3, 0, false, true) == CameraMatch::Switched);
    CHECK(step(n, now, 0.3, 0, false, true) == CameraMatch::AllIgnored);
    CHECK(n.current() == 1);
    CHECK(n.carry_yaw() == 0);
}

void test_match_carry_limit() {
    std::puts("match: at most 45 degrees per axis are carried over");
    CameraMatch m;
    m.select({1, 5});
    CHECK(step(m, 1000, 80, 0) == CameraMatch::None);
    CHECK(step(m, 1300, 0, 0) == CameraMatch::Switched);
    CHECK(m.carry_yaw() == CameraMatch::kMaxCarryYaw);
}

void test_match_pitch_limit() {
    std::puts("match: pushing pitch against the limit (camera still) is no camera change");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
    // Looking straight up and still pushing: only pitch is written, the camera doesn't move.
    CHECK(s.run(3000, 0, 0.5) == CameraMatch::None);
    CHECK(s.m.current() == 5);
    // Pushing up while turning sideways: the yaw still follows.
    CHECK(s.run(3000, 0.3, 1.0) == CameraMatch::None);
    CHECK(s.m.current() == 5);
}

void test_match_all_ignored_keeps_followed() {
    std::puts("match: when every view is ignored, the one followed last stays (a yaw limit in the clamped action)");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    s.driver = 0;  // held at its yaw limit: the clamp view is ignored, the base view is still rewritten
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 3000 && e != CameraMatch::AllIgnored) e = s.run(10);
    CHECK(e == CameraMatch::AllIgnored);
    CHECK(s.m.current() == 5);
    s.driver = 5;  // back inside the limits: no switch needed
    CHECK(s.run(3000) == CameraMatch::Follows);  // confirmed again, no switch
    CHECK(s.m.current() == 5);
}

void test_match_pitch_carry_limit() {
    std::puts("match: at most 10 degrees of pitch are carried over");
    CameraMatch m;
    m.select({1, 5});
    CameraMatch::Frame f;
    f.yaw = 3, f.pitch = 30;
    CHECK(m.frame(f, 1000) == CameraMatch::None);
    CHECK(step(m, 1300, 0, 0) == CameraMatch::Switched);
    CHECK(m.carry_yaw() == 3);
    CHECK(m.carry_pitch() == CameraMatch::kMaxCarryPitch);
}

void test_match_members_change() {
    std::puts("match: new members (view target or profile changed) start over at the base view");
    Sim s({1, 5});
    s.driver = 5;
    s.run(500);
    CHECK(s.m.current() == 5);
    CHECK(s.m.select({1, 5, 7}) == 1);
    CHECK(s.m.select({2}) == 2);
}

// ---------------------------------------------------------------- profile

void test_load_missing() {
    std::puts("profile: a missing file gives the default view alone");
    const std::wstring path = temp_profile(L"missing");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 1);
    CHECK(views[0].id == 1);
    CHECK(views[0].label == "Exploration");
    CHECK(!has_views(path));
}

void test_load_views() {
    std::puts("profile: ViewTargetClass and Match=Camera views load, invalid ones are skipped");
    const std::wstring path = temp_profile(L"load");
    write_file(path,
               "[GyroUE]\r\nViews=InCar, Clamp, Bad, Dup\r\n\r\n"
               "[View.Default]\r\nLabel=On foot\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\n\r\n"
               "[View.Clamp]\r\nId=3\r\nMatch=camera\r\nObject=Camera.@SBZClampCameraRotationModifier\r\nOffsets=0x68\r\n"
               "RotatorSize=24\r\n\r\n"
               "[View.Bad]\r\nId=4\r\nMatch=Camera\r\n\r\n"
               "[View.Dup]\r\nId=3\r\nMatch=Camera\r\nProperty=X\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(views[0].label == "On foot");
    const View* car = by_id(views, 2);
    CHECK(car && car->view_target_class == "BP_Car_C" && !car->match_camera && car->property == "CamRot");
    const View* clamp = by_id(views, 3);
    CHECK(clamp && clamp->match_camera && clamp->view_target_class.empty());
    CHECK(clamp && clamp->object.size() == 2 && clamp->object[1] == "@SBZClampCameraRotationModifier");
    CHECK(clamp && clamp->offsets.size() == 1 && clamp->offsets[0] == 0x68 && clamp->rotator_size == 24);
    CHECK(clamp && clamp->label == "Clamp" && clamp->parent == 1);
    CHECK(has_views(path));
}

LearnedView clamp_view() {
    LearnedView l;
    l.object = "Camera.@SBZClampCameraRotationModifier";
    l.offsets = {0x68};
    l.rotator_size = 24;
    l.label = "SBZClampCameraRotationModifier";
    l.note = "Learned by a test.\nSecond line.";
    return l;
}

void test_add_view_new_file() {
    std::puts("profile: add_view creates the profile and the view reads back");
    const std::wstring path = temp_profile(L"new");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "PAYDAY3-Win64-Shipping", clamp_view(), {1001, 1002, 1003}, id, key) == AddResult::Added);
    CHECK(id == 2);
    CHECK(key == "Auto2");
    CHECK(ini_value(path, L"GyroUE", L"Views") == "Auto2");
    const std::string text = read_file(path);
    CHECK(text.find("; GyroUE profile for PAYDAY3-Win64-Shipping") == 0);
    CHECK(text.find("; Learned by a test.\r\n; Second line.\r\n[View.Auto2]") != std::string::npos);
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 2);
    const View* v = by_id(views, 2);
    CHECK(v && v->match_camera && v->parent == 1 && v->label == "SBZClampCameraRotationModifier");
    CHECK(v && v->offsets.size() == 1 && v->offsets[0] == 0x68 && v->rotator_size == 24);
    CHECK(v && v->object.size() == 2 && v->object[0] == "Camera");
    // The default view stays ControlRotation.
    CHECK(views[0].object.empty() && views[0].property.empty() && views[0].offsets.empty());
}

void test_add_view_keeps_profile() {
    std::puts("profile: add_view keeps the existing views and settings, takes a free id");
    const std::wstring path = temp_profile(L"existing");
    write_file(path,
               "; hand-written profile\r\n[GyroUE]\r\nViews=InCar\r\nDualSenseAsDualShock4=1\r\n\r\n"
               "[View.Default]\r\nLabel=On foot\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\n");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {1001, 1002, 1003}, id, key) == AddResult::Added);
    CHECK(id == 3);
    CHECK(ini_value(path, L"GyroUE", L"Views") == "InCar,Auto3");
    CHECK(ini_value(path, L"GyroUE", L"DualSenseAsDualShock4") == "1");
    CHECK(read_file(path).find("; hand-written profile") == 0);
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(views[0].label == "On foot");
    CHECK(by_id(views, 2) && by_id(views, 2)->property == "CamRot");
    CHECK(by_id(views, 3) && by_id(views, 3)->match_camera);
}

void test_add_view_duplicate() {
    std::puts("profile: the same rotation isn't added twice");
    const std::wstring path = temp_profile(L"dup");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {}, id, key) == AddResult::Added);
    const std::string before = read_file(path);
    CHECK(add_view(path, "Game", clamp_view(), {}, id, key) == AddResult::Duplicate);
    CHECK(id == 2 && key == "Auto2");
    CHECK(read_file(path) == before);
    // ControlRotation is the default view already.
    LearnedView control;
    control.object = "PC";
    control.property = "ControlRotation";
    CHECK(add_view(path, "Game", control, {}, id, key) == AddResult::Duplicate);
    CHECK(id == 1);
    // A different raw offset of the same object is another rotation.
    LearnedView other = clamp_view();
    other.offsets = {0x80};
    CHECK(add_view(path, "Game", other, {}, id, key) == AddResult::Added);
    CHECK(id == 3);
}

void test_add_view_reserved_ids() {
    std::puts("profile: reserved ids (sensitivity profiles) and ids in use are skipped");
    const std::wstring path = temp_profile(L"reserved");
    write_file(path, "[GyroUE]\r\nViews=A\r\n[View.A]\r\nId=3\r\nViewTargetClass=X\r\nProperty=Y\r\n");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {2, 4}, id, key) == AddResult::Added);
    CHECK(id == 5);
}

void test_load_trailing_tab() {
    std::puts("profile: a hand-edited Views= with a trailing space and tab still finds its view");
    const std::wstring path = temp_profile(L"tab");
    write_file(path, "[View.Default]\r\nLabel=Exploration\r\n[View.Clamp]\r\nId=2\r\nLabel=Clamp Camera\r\nMatch=Camera\r\n"
                     "Object=Camera.@SBZClampCameraRotationModifier\r\nOffsets=0x68\r\n[GyroUE]\r\nViews=Clamp \t\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 2);
    CHECK(by_id(views, 2) && by_id(views, 2)->label == "Clamp Camera");
}

void test_add_view_property() {
    std::puts("profile: a reflected rotation is written as Property (with WorldRotation)");
    const std::wstring path = temp_profile(L"property");
    LearnedView l;
    l.object = "Pawn.CameraComponent";
    l.property = "RelativeRotation";
    l.world_rotation = true;
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", l, {}, id, key) == AddResult::Added);
    const std::vector<View> views = load_views(path);
    const View* v = by_id(views, id);
    CHECK(v && v->property == "RelativeRotation" && v->world_rotation && v->offsets.empty());
    CHECK(v && v->label == key);  // no label given: the section key
}

void test_add_view_rejects_empty() {
    std::puts("profile: a learned view without a rotation is refused");
    const std::wstring path = temp_profile(L"empty");
    LearnedView l;
    l.object = "Camera";
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", l, {}, id, key) == AddResult::Failed);
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

void test_split() {
    std::puts("profile: split trims and drops empty items");
    const std::vector<std::string> parts = split(" a, b ,,c ", ',');
    CHECK(parts.size() == 3 && parts[0] == "a" && parts[1] == "b" && parts[2] == "c");
    CHECK(split("", ',').empty());
    const std::vector<std::string> tabs = split("Clamp \t", ',');
    CHECK(tabs.size() == 1 && tabs[0] == "Clamp");
}

}  // namespace

int main() {
    test_rewritten_helper();
    test_match_single_member();
    test_match_base_follows();
    test_match_fast_path();
    test_match_slow_path();
    test_match_scene_round_trip();
    test_match_single_rewrite_doesnt_switch();
    test_match_lagging_camera_follows();
    test_match_needs_enough_gyro_and_time();
    test_match_look_input_resets();
    test_match_cutscene();
    test_match_write_failed();
    test_match_carry_limit();
    test_match_members_change();
    test_match_pitch_limit();
    test_match_all_ignored_keeps_followed();
    test_match_pitch_carry_limit();
    test_load_missing();
    test_load_views();
    test_add_view_new_file();
    test_add_view_keeps_profile();
    test_add_view_duplicate();
    test_add_view_reserved_ids();
    test_load_trailing_tab();
    test_add_view_property();
    test_add_view_rejects_empty();
    test_split();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
