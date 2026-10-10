#pragma once
// Minimal, engine-version-tolerant access to Unreal's reflection data (UE 4.25+ / UE5):
// - GUObjectArray and FNamePool are found by their shape in the exe's writable sections,
//   so no per-game byte signatures are needed;
// - UStruct/FField/FProperty offsets are detected at runtime against known engine classes;
// - objects and properties are then reached by name (GEngine -> world -> player -> ...).
// Every memory read is guarded: a bad pointer returns "not found" instead of crashing the game.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gyroue::ue {

using Obj = uintptr_t;  // UObject* (0 = none)

struct Prop {
    uintptr_t field = 0;      // FProperty*
    int32_t offset = -1;      // Offset_Internal
    int32_t element_size = 0;
    std::string type;         // FFieldClass name, e.g. "StructProperty", "BoolProperty"
    explicit operator bool() const { return field != 0 && offset >= 0; }
};

// One-time discovery. Cheap to call repeatedly; returns true once everything was found.
bool init();
bool ready();

std::string name_of(Obj o);              // object or class short name ("BP_PlayerCarNew_C")
Obj class_of(Obj o);
bool is_a(Obj o, const char* class_name);  // class chain contains class_name (exact match)
bool is_cdo(Obj o);

// Objects
Obj find_instance(const char* class_name);  // first non-CDO object whose class chain has class_name
Obj outer_of(Obj o);
Obj find_subobject(Obj outer, const char* name);  // object named `name` whose Outer is `outer` (full scan)
// First object whose Outer is `outer` and whose class chain has class_name (full scan). For subobjects whose
// instance name changes every session, e.g. "TP_Walk_Manual_C_2147462850".
Obj find_subobject_of_class(Obj outer, const char* class_name);

// Properties (class chain lookup, cached per class)
Prop find_prop(Obj cls_or_struct, const char* name);
Prop prop_of(Obj o, const char* name);       // lookup on o's class
Obj read_obj(Obj o, const char* name);       // object property value (0 if missing)
bool read_bool(Obj o, const char* name, bool fallback);
Obj struct_type(const Prop& p);              // UScriptStruct* of a StructProperty

// Rotators are float (UE4) or double (UE5 large world coordinates); element_size tells which.
bool add_rotator(uintptr_t address, int32_t element_size, double pitch, double yaw);
bool read_rotator(uintptr_t address, int32_t element_size, double& pitch, double& yaw, double& roll);

// Diagnostics / profile helpers for scalar properties (Float, Double, Int, Byte, Bool).
void dump_props(Obj o, const char* stop_at_class);  // log every property down to (not including) stop_at_class
std::string value_string(Obj o, const char* name);  // "" if missing or unsupported type
bool write_number(Obj o, const char* name, double value);

// Probe: snapshot every float/double/rotator (structs nested up to 3 levels) of the roots and all
// objects whose Outer chain reaches a root (components, subobjects); the diff logs what changed.
// Labels are profile paths ("Pawn.View(Class)"): the part before "(" works as Object=.
size_t probe_snapshot(const std::vector<std::pair<std::string, Obj>>& roots);
// Logs what changed since the snapshot (verbose: every changed value, as for F8) and scores the rotations
// that now hold the camera's pitch and yaw. Returns the number of scored diffs so far (0 = not scored).
int probe_diff(bool verbose);
// Suggested profile (INI text) from the scored diffs, "" until there are at least two. summary = one-line pick.
// as_profile: the header for the game's own profile (the game had none), else for <exe>.suggested.ini.
std::string probe_suggestion(const std::string& exe_stem, std::string& summary, bool as_profile);

// The camera's final view rotation (PlayerCameraManager CameraCachePrivate.POV.Rotation).
bool camera_pov(Obj camera, double& pitch, double& yaw);

// Raw guarded reads
bool read_ptr(uintptr_t address, uintptr_t& out);
bool read_i32(uintptr_t address, int32_t& out);

}  // namespace gyroue::ue
