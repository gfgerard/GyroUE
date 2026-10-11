# GyroUE: advanced

This page is for the curious, for adding support for new games, and for developers. You don't need it to
play.

## Technical features

- **Drop-in install:** one proxy `winmm.dll` (or `version.dll`) plus a `GyroUE` folder next to the game executable. No loader
  (UE4SS etc.) required.
- **Signature-free engine access:** GUObjectArray, FNamePool and the UStruct/FProperty layout are found at
  runtime by shape and verified against known engine classes, so no per-game byte patterns are needed.
  Works on monolithic and modular (one DLL per engine module) builds.
- **Generic camera path:** adds the gyro delta to `PlayerController.ControlRotation` in real degrees (float
  rotators on UE4, double rotators on UE5).
- **Per-game profiles** for games with custom cameras: vehicle views, camera components, or non-reflected
  C++ camera state reached by raw offset.
- **Gameplay gating:** the gyro stops in menus, pause, main menu, while the mouse cursor is shown, and when
  the game window loses focus.
- **Automatic calibration** in menus (GyroLib's "menus only" mode), including games whose gamepad menus
  show no cursor.
- **In-game settings panel** (GyroLib's DX12 overlay, `F10` by default): sensitivity, activation button or
  trigger, invert axes, smoothing and more, saved to `gyrolib.ini`. Edits to `gyrolib.ini` are also reloaded
  live while the game runs. GyroUE declares only `GL_HOST_MENU_STATE` to GyroLib: no native stick or touchpad
  suppression, no long-press blocking and no recenter callback (see [Current scope](README.md#current-scope)).
- **Coexists with ReShade, RenoDX (HDR) and the Steam overlay.** Panel rendering runs on GyroUE's own
  thread, so it keeps working when the engine moves `Present`/`ResizeBuffers` between threads.
- **Diagnostics for new games:** a status line every 10 s in `GyroUE.log`, plus an optional probe (`F7`
  snapshot / `F8` diff) to find which rotation a game's camera actually uses.

## Compatibility details

| Game | Engine | Profile needed| Notes |
|---|---|---|---|
| Pacific Drive | UE 4.27 | yes | On foot uses the generic path. In the car the gyro drives the car camera and overrides the auto-recentering while driving. |
| Returnal | UE 4.25 (modular build) | no | Generic path working OOB. A good setup is gyro on while L2/R2 is held |
| Cronos: The New Dawn | UE 5.4 | yes | The camera ignores `ControlRotation`; the profile writes the look rotation inside the pawn's `View` component. Panel verified with HDR + RenoDX. |
| Gears of War: E-Day | UE 5.6 (Xbox / Game Pass build) | no | Generic path, campaign tested *WITH EAC DISABLED*. Install in `Binaries\WinGDK` renamed to `version.dll` (with RivaTuner running, the game loads the system `winmm.dll` first). Protected exe: GyroUE switches to inline hooks and finds the engine objects with a heap scan, so the gyro becomes available a few seconds after the main menu appears. The game ships EasyAntiCheat: don't use GyroUE in online modes. |
| Life is Strange Remastered | UE 4 (pre-4.25 property system), DX11 | yes (shipped) | Found with the camera probe. The camera splits pitch and yaw across two raw rotators, and the walking camera blends back to an automatic follow camera unless it gets look input; the profile writes `OnGyro=` to keep it manual. No settings panel (DX11): configure via `gyrolib.ini`. |
| Luto | UE 5.5.4 | no | Generic path working OOB |

Tested with a DualSense over USB/BT and Nintendo Switch Pro Controller. Requirements and limits:

- Windows 10/11 x64, DirectX 12 for the settings panel. Without a DX12 swapchain the gyro still works; only
  the panel is unavailable.
- **Single-player / offline games without anti-cheat only.** Do not use GyroUE with EasyAntiCheat,
  BattlEye or any online-protected game.
- UE 4 and UE 5. Both property systems are detected at runtime: FField (UE 4.25+, UE 5) and the older UProperty
  objects (tested with Life is Strange Remastered).
- GyroUE ships as `winmm.dll`, which more games import than `version.dll`. If the game already has a `winmm.dll`
  proxy from another mod, or GyroUE doesn't load (no log), rename it to `version.dll`: the same DLL works under both
  names. The installer refuses to overwrite a foreign proxy.
- Close other pad remappers (DS4Windows, DSX, reWASD) while playing. If Steam Input is enabled for the game,
  check that Steam isn't also converting the gyro to mouse, or the motion will be doubled.

## Log reference

All diagnostics go to `GyroUE\GyroUE.log` (probe results, if enabled, to `GyroUE\GyroUE-probe.log`).

| Symptom | Check |
|---|---|
| No log file | `winmm.dll` is not next to the game exe (it must be in `Binaries\Win64` or `Binaries\WinGDK`), another mod's `winmm.dll` replaced it, or the system `winmm.dll` was loaded first (e.g. by RivaTuner's hooks, seen in Gears of War: E-Day): rename it to `version.dll`. |
| "Entry point `timeGetDevCaps` not found" | A GyroUE older than 0.4.7 named `winmm.dll`. Update: 0.4.7+ exports winmm too. |
| `motion=0` in the status line | The controller isn't visible to SDL: close remappers, try USB, check Steam Input. |
| `view=none` in gameplay | The pawn is missing or a placeholder. The game may need a profile. |
| `applied yaw` changes but the camera doesn't move | Custom camera: see [Finding the camera](#finding-the-camera-of-a-new-game). |
| Camera drifts slowly | Rest the pad in a menu for a few seconds (`bias=` in the status line should become non-zero) or use "Recalibrate gyro". |
| Panel doesn't open | Look for `overlay:` lines. Try from the game's pause menu. As a fallback, edit `gyrolib.ini` directly. |
| Double motion | Steam Input or another tool is also converting the gyro to mouse. |
| `deps:` lines | Before loading GyroLib, GyroUE checks which copy of each DLL GyroLib shares with the game (Visual C++ runtime, SDL3): one already loaded by the game, or one next to its exe, is the one GyroLib gets. `older than 14.40, GyroLib ... would crash the game` turns the gyro off for the session (the game still starts) and shows the fix: rename the game's copies so Windows' newer ones are used. `older than GyroLib's build tools` and `the game's own SDL3` are warnings only. |

## How it works

```text
DualSense --HID--> SDL3 (inside gyrolib.dll)
          --> GyroLib: calibration, fusion, gyro space, smoothing, curves, activation, views
          --> gl_update() -> yaw/pitch delta in degrees
          --> GyroUE, on the game thread once per frame:
                scene   <- GEngine -> GameViewport -> World -> LocalPlayer -> PlayerController,
                           Pawn, PlayerCameraManager view target, WorldSettings pauser
                gating  <- focus, pause, cursor, "no gameplay view" (main menu DefaultPawn)
                apply   -> profile view rotation += delta  (default: ControlRotation)
```

- `winmm.dll` is a proxy for the system DLL of the same name. It also exports all of `version.dll`, so it can be
  renamed to `version.dll`; either way every export is forwarded to the System32 DLL with its own file name, so the
  game behaves normally. 
- The per-frame hook is an import-table patch of `PeekMessageW`. Unreal pumps window messages on the game
  thread once per frame, which is a safe place to read and write game objects. The game's own code is never
  patched. Exception: protected exes whose import table is only a stub filled in later by the protector
  (e.g. Gears of War: E-Day). There GyroUE hooks `user32!PeekMessageW` inline with
  [MinHook](https://github.com/TsudaKageyu/minhook), and finds the swapchain through the vtable of the real
  System32 DXGI factory (so it doesn't fight Streamline, ReShade or RivaTuner over the `dxgi.dll` exports). The log says
  which mode is used (`hooks: ... import-table hooks` / `inline hooks on exports`); `GYROUE_INLINE=1` in the
  environment forces inline mode.
- Every memory read through reflection is guarded, so a missing object or property disables the feature
  instead of crashing the game.
- The settings panel hooks DXGI factory and swapchain vtables (`Present`, `ResizeBuffers`) and subclasses
  the game window to forward input. These hooks are only installed if the panel is enabled in
  `gyrolib.ini`. Set `ui.menu_key=` (empty) and `ui.gamepad_menu_shortcut=0` to run with no graphics hooks
  at all.

## Game profiles

The generic path needs no profile. When a game's camera ignores `ControlRotation`, add
`GyroUE\profiles\<exe name without .exe>.ini`. Profiles shipped in this repo live in [`profiles/`](profiles).

```ini
[GyroUE]
; Extra views, in priority order (the default view always exists).
Views=InCar
; Diagnostics: F7 = snapshot, F8 = log what changed (see below).
Probe=0

[View.Default]
Label=Camera
; Optional: another object and rotation for the default view.
;   Object=Root.Sub...   Root = PC | Pawn | Camera | ViewTarget; each Sub is an object property name,
;                        the name of a subobject (e.g. a component) of the previous object, or @Class:
;                        its first subobject of that class (for names that change every session)
;   Property=Name        a reflected FRotator property on that object
;   Offsets=0x2A0,0x420  raw FRotator offsets instead (non-reflected C++ state); all get the delta
;   RotatorSize=24       24 = double (UE5), 12 = float (UE4)
;   WorldRotation=1      with Property=RelativeRotation of a scene component that uses absolute rotation: also
;                        writes the component's world rotation (see "Components with absolute rotation")

[View.InCar]
Id=2
Label=In car
; Active while the camera's view target is (or derives from) this class.
ViewTargetClass=BP_PlayerCarNew_C
; Rotator property on the view target (or use Object/Offsets as above).
Property=CameraControllerRotation_Target
; Settings inherit from view 1 until changed for this view in the panel.
Parent=1
; Scalar writes on the view object whenever the gyro moves it (e.g. to cancel auto-recentering).
; Name:value is on the view object; Root.Sub.Name:value on another object (same path syntax as Object=),
; e.g. Life is Strange: OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1
OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0
; Diagnostics: log properties on change / dump all properties once.
;Watch=IsAutoControlling
;Dump=1
```

## Finding the camera of a new game

> [!IMPORTANT] 
> GyroUE guides you with on-screen messages but the game can't be using exclusive fullscreen or you won't see them.


Most UE games turn the camera from `PlayerController.ControlRotation`, and GyroUE works with no profile.
Some games keep their own look rotation somewhere else: in a vehicle actor, in a camera component, or in
C++ memory that Unreal's reflection doesn't describe. The **probe** finds that rotation for you. It records
the camera-related state, you turn the camera with the right stick, and it logs what changed. The result
goes into the game's profile.

The steps below use the real Cronos: The New Dawn session as the example.

### 1. Check whether the game needs a profile

Install GyroUE, play for a few seconds moving the controller to check if it moves the camera, quit, and open `GyroUE\GyroUE.log`. GyroUE compares
how much it turned the view with how much the camera actually moved, and says it in one line:

```text
camera: confirmed, the camera follows the gyro (ControlRotation, view 'Camera')
camera: the gyro turned ControlRotation (view 'Camera') by 412 degrees but the camera moved only 0: the game ignores it.
        To find the camera's real rotation, set Probe=1 in ...GyroUE\GyroUE.ini, restart and turn the camera
        with the right stick.
```

The check only runs on the player's own camera (view target = pawn), so scripted or fixed cameras don't trigger it,
and it stops once it has a verdict. The warning also appears **on screen**, in a small box at the top of the game
window, with what to do next; so do the probe's progress messages below (capture ready, capture done, turn
discarded, suggested profile written). The box is drawn by Windows, not by the game, so it works with DX11 and DX12;
it doesn't show in exclusive fullscreen. `Notices=0` in `GyroUE\GyroUE.ini` keeps the messages in the log only.
The `status:` lines written every 10 seconds give more detail:

```text
status: view=Camera pc=SHPlayerControllerPlay_BP_C pawn=CronosCharacterPlay_BP_C target=CronosCharacterPlay_BP_C
        paused=0 cursor=0 | gyro enabled=1 motion=1 ... | applied yaw=-220.1 pitch=14.5
```

| What you see | Meaning |
|---|---|
| `view=Camera`, `applied yaw/pitch` changing, **camera moves** | The generic path works. No profile needed. |
| `view=Camera`, `applied yaw/pitch` changing, **camera doesn't move** | GyroUE writes `ControlRotation` but the game ignores it. Use the probe. |
| `view=none` during gameplay | No pawn is possessed, or it is a placeholder `DefaultPawn` (normal in main menus). |
| `motion=0` | Controller problem, not a camera problem. See [Problems?](TROUBLESHOOTING.md). |

### 2. Enable the probe

Open `GyroUE\GyroUE.ini` (GyroUE creates it with the defaults on the first run; releases don't ship it, so updates
keep your values) and set:

```ini
[GyroUE]
Probe=1
```

`GyroUE.ini` holds the settings for every game in that folder. A game's profile
(`GyroUE\profiles\<game exe>.ini`) can override any of them in its own `[GyroUE]` section.

`GyroUE.log` now says `profile: probe enabled (automatic capture on stick turns, F7/F8 manual), results in ...GyroUE-probe.log`. The
probe writes its results to that separate file, `GyroUE\GyroUE-probe.log`, which starts with a short reminder of
these steps; `GyroUE.log` only gets one line per snapshot and diff. `F7` and `F8` are polled only while
the game window has focus. If the game uses those keys for something, press them where that doesn't matter
(e.g. standing still).

### 3. Capture camera turns

**Automatic (recommended).** With `Probe=1` GyroUE captures by itself:

1. Load into gameplay and stand still for a second. GyroUE takes a snapshot
   (`GyroUE.log`: `probe: automatic capture ready ...`). The gyro output is **paused** while the probe captures,
   so the controller can stay in your hands.
2. Turn the camera **with the right stick**, more than 30° sideways (and a bit up or down), then stop. GyroUE
   detects the turn and compares. Camera turns made without the right stick (cutscenes, scripted cameras) are
   ignored.
3. Repeat 3 or 4 times (up to 8 are captured per session; then the gyro output resumes).

From the second capture on, GyroUE ranks every rotation that held the camera's pitch and yaw in all the captures,
drops the camera's outputs (camera manager, component transforms) and writes the result (also copied at the end of
`GyroUE-probe.log`):

- **The game has no profile:** GyroUE writes the game's profile itself, `GyroUE\profiles\<game exe>.ini`, and keeps
  updating it with every capture of that session. Restart the game to use it.
- **The game already has a profile:** it is left untouched; the result goes to `<game exe>.suggested.ini` next to it.
  To try it, rename it to `<game exe>.ini` (replacing the old one) and restart.
- A result with no usable rotation never becomes the profile (it only goes to `.suggested.ini` and the probe log).

For Cronos the profile should read (the answer the manual probe found):

```ini
; GyroUE 0.6.4 profile for Cronos-Win64-Shipping, created by the camera probe (3 scored captures, ...).
; NOT VERIFIED. Restart the game to use it, then set Probe=0 in GyroUE\GyroUE.ini.
[View.Default]
Label=Camera
Object=Pawn.View
Offsets=0x2A0,0x420
RotatorSize=24
```

Once the camera follows the gyro, set `Probe=0` again in `GyroUE\GyroUE.ini`. If it still ignores the gyro, the file
lists alternatives to try one at a time; the steps below explain how to read the full probe output.

**Manual.** `F7` takes a snapshot, turn the camera with the right stick only (well over 30° to one side **and**
some degrees up or down, without walking), `F8` logs the diff to `GyroUE-probe.log`. Diffs with a large enough turn
also feed the suggested profile. Pressing `F7`/`F8` turns the automatic capture off for the session.

### 4. Read the probe output

Open `GyroUE\GyroUE-probe.log` (it is recreated every game session). The diff has three parts. Every line starts with the object's **profile path**: the root (`PC`, `Pawn`,
`Camera` = PlayerCameraManager, or `ViewTarget`), followed by subobject names, then the object's class in
parentheses. Abridged output from Cronos:

```text
probe: roots pc=SHPlayerControllerPlay_BP_C_2147478565 pawn=CronosCharacterPlay_BP_C_2147478555 camera=SHPlayerCameraManagerPlay_2147478563 ...
probe: snapshot of 159 objects, 3179 values

  rot Camera(SHPlayerCameraManagerPlay).CameraCachePrivate.POV.Rotation (P 0.00 Y 150.00 R -0.00) -> (P 55.15 Y -62.26 R -0.00)
  rot Camera.TransformComponent0(SceneComponent).RelativeRotation (P 0.00 Y 150.00 R 0.00) -> (P 55.15 Y -62.26 R -0.00)
  rot Pawn.Animation(SHCharacterPlayAnimComponent).LastAimRotation (P 0.00 Y 150.00 R -0.00) -> (P 55.15 Y -62.26 R -0.00)
probe: diff done (6 rotators, 18 numbers changed)

probe: raw diff, POV pitch 0.00 -> 55.15 (d 55.15), yaw 150.00 -> -62.26 (d 147.74)
  raw Camera(SHPlayerCameraManagerPlay)+0x388 double 0.0000 -> 55.1492  = POV pitch
  raw Camera(SHPlayerCameraManagerPlay)+0x390 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.View(SHCharacterPlayViewComponent)+0x2A0 double 0.0000 -> 55.1492  = POV pitch
  raw Pawn.View(SHCharacterPlayViewComponent)+0x2A8 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.View(SHCharacterPlayViewComponent)+0x420 double 0.0000 -> 55.1492  = POV pitch
  raw Pawn.View(SHCharacterPlayViewComponent)+0x428 double 150.0000 -> -62.2608  = POV yaw
  raw Pawn.SpringArm(SHCharacterPlaySpringArmComponent)+0x2D8 double 0.0000 -> 55.1492  = POV pitch
probe: raw diff done (39 matches)
```

| Line | What it is |
|---|---|
| `rot <path>.<Property> (before) -> (after)` | A **reflected** `FRotator` that changed (nested structs are followed up to 3 levels, e.g. `CameraCachePrivate.POV.Rotation`). |
| `num <path>.<Property> before -> after` | A reflected float/double that changed. Useful for side effects (arm length, aim offsets, auto-recentering flags) and for `OnGyro=` / `Watch=`. |
| `raw <path>+0xOFFSET double/float before -> after = ...` | **Non-reflected** memory inside the object that now holds the camera's yaw or pitch (in degrees or radians), or changed by the same amount as the camera. |

The camera's final view (`PlayerCameraManager...POV.Rotation`) always changes, but it is an **output**: the
engine recomputes it every frame, so writing to it does nothing. You are looking for the **input**, the value
the stick writes and the camera reads. How to choose:

- **Ignore the camera manager and transforms**: `Camera(...)`, `Camera.TransformComponent0`, and component
  world transforms (`+0x100`/`+0x200` in scene components, the spring arm's world rotation). These are copies of
  the result.
- **Ignore animation consumers**: anim instances, aim offsets, `LastAimRotation`, head rotation. They read the
  camera; they don't drive it.
- **Prefer a reflected property** (`rot` line) on the PlayerController, the pawn, a vehicle, or a
  camera/"control"/"view" component, with a name like `...Rotation_Target`, `ControlRotation`, `LookRotation`.
- **Otherwise look for a raw rotator**: three consecutive values at `+N` (pitch), `+N+8` (yaw), `+N+16` (roll)
  for doubles, or `+N`, `+N+4`, `+N+8` for floats, in a component whose name suggests control or view. Here
  that is `Pawn.View(SHCharacterPlayViewComponent)`, a rotator at **+0x2A0** and another at **+0x420**,
  matching in both diffs.
- If there are two candidates that always hold the same value (current/target), list both.

### 5. Write the view in the profile

Turn the chosen line into profile keys:

| Probe line | Profile key |
|---|---|
| Path before `(` | `Object=` (e.g. `Pawn.View`). Omit it for `PC` + `ControlRotation`. |
| `rot ... .PropertyName` | `Property=PropertyName` |
| `raw ...+0x2A0` | `Offsets=0x2A0` (the offset of the **pitch** value, i.e. the start of the rotator) |
| `double` / `float` | `RotatorSize=24` (UE5) / `RotatorSize=12` (UE4) |
| `WorldRotation=1` in the suggestion | keep it (a component with absolute rotation, Example 3) |

**Example 1: non-reflected rotator in a pawn component (Cronos, UE5).** The camera is always the
character's, so the default view is redirected:

```ini
; profiles/Cronos-Win64-Shipping.ini
[View.Default]
Label=Camera
Object=Pawn.View
Offsets=0x2A0,0x420
RotatorSize=24
```

**Example 2: reflected property on a vehicle (Pacific Drive, UE4).** On foot the generic path works. In the car
the camera's view target becomes the car actor, and the right stick writes its reflected rotator
`CameraControllerRotation_Target` (found with an earlier, script-based version of the same snapshot/diff method). An
extra view is added that is active only while the view target is the car:

```ini
; profiles/PenDriverPro-Win64-Shipping.ini
[GyroUE]
Views=InCar

[View.Default]
Label=On foot

[View.InCar]
Id=2
Label=In car
ViewTargetClass=BP_PlayerCarNew_C
Property=CameraControllerRotation_Target
Parent=1
; From the num lines: the car re-centres its camera unless these are cleared while the player looks.
OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0,Camera_AutoControlAmount_current:0
```

**Example 3: a camera component with absolute rotation (Severed Steel, UE4).** The character turns its
`FirstPersonCamera` component directly from the component's current world rotation (look input, then
`SetWorldRotation`), and copies the result to `ControlRotation` every frame, so `ControlRotation` is only an output.
A component with absolute rotation keeps its rotation twice: in `RelativeRotation` (reflected) and in its world
transform `ComponentToWorld` (not reflected), which is what the game reads back. Writing `RelativeRotation` alone
is undone the same frame. `WorldRotation=1` writes both:

```ini
[View.Default]
Label=Camera
Object=Pawn.FPCamera
Property=RelativeRotation
WorldRotation=1
```

GyroUE finds `ComponentToWorld` by value the first time (its quaternion equals the `RelativeRotation` one; log:
`ue: CameraComponent ComponentToWorld rotation at +0x1C0`) and only writes while the two still agree. If they
don't (the component isn't absolute, or the game is between its writes), it skips that frame and logs
`profile: view N WorldRotation not applied to ...` with the reason. The probe suggests this form by itself for
attached components whose world rotation matches their `RelativeRotation`, a camera component first.

Use the **default view** with `Object=` when the game always uses that camera. Use an **extra view** with
`ViewTargetClass=` when a different camera takes over in some situations (vehicles, turrets). Extra views use
the view target as their object unless `Object=` is given. Each view gets its own tab in the settings panel.

### 6. Verify and clean up

1. Start the game and move the controller. With raw offsets, the log adds a line per status:
   `status: view object View +0x2A0=(P -13.34 Y 167.93) +0x420=(P -13.34 Y 167.93)`. If it says `(not found)`,
   the `Object=` path is wrong.
2. Check that the camera turns smoothly, that up is up, that aiming follows the camera, and that the right
   stick still works. If the camera jumps back or fights the gyro, you picked an output: try another
   candidate.
3. Set `Probe=0` (or remove the line) once the profile works.

Raw offsets depend on the exact game build and may break when the game is updated. Prefer a reflected property
when one exists, and note the game version in the profile.

### What the probe covers

- **Objects:** the PlayerController, the pawn, the PlayerCameraManager, the camera's view target, and every
  object whose Outer chain leads to one of them (components and other subobjects, up to 8 levels).
- **Reflected values:** every `float`, `double` and `FRotator` property of those objects, following nested
  structs up to 3 levels (vectors, quaternions, transforms and colours are skipped). Rotators are logged when
  any component changes by more than 0.5°, numbers when they change by more than 0.001 (at most 150 rotator and 250 number
  lines per diff).
- **Raw memory:** each object's bytes up to its class size, compared as aligned floats and doubles. A value is
  reported if it now equals the camera's yaw or pitch (±0.6°, degrees or radians) or changed by the camera's
  delta. Turns smaller than 2° are ignored (at most 300 lines).
- **Not covered:** objects that aren't subobjects of those roots (e.g. a separate camera actor that isn't the view
  target, world subsystems) and plain C++ heap data that isn't stored inside a UObject. For those, a debugger or
  memory scanner is needed. The probe only reads memory and never writes.

## Building from source

Requirements:

- Visual Studio 2022 or newer with the C++ desktop workload (MSVC x64, CMake 3.24+). C++20.
- Git Bash (for the helper scripts).
- The GyroLib **1.3.1** Windows x64 SDK, from the
  [GyroLib releases](https://github.com/lud-berthe/GyroLib/releases). Verify it against `SHA256SUMS` and
  extract it to `third_party/GyroLib-1.3.1/sdk/GyroLib-1.3.1-sdk-windows-x64/`. The pinned version is in
  `tools/gyrolib-sdk.sh`.

```bash
bash tools/build-gyroue.sh      # -> build/GyroUE/Release/winmm.dll
bash tools/package-gyroue.sh    # build + build/release/GyroUE-v<version>.zip (installable release)
```

GyroUE links the static MSVC runtime. `gyrolib.dll` is delay-loaded by full path from the `GyroUE` subfolder.

Repository layout:

| Path | Contents |
|---|---|
| `src/GyroUE/` | The mod: proxy, hooks, UE reflection access (`ue.cpp`), host logic (`host.cpp`), panel (`overlay.cpp`) |
| `profiles/` | Per-game profiles |
| `games/<Game>/` | Per-game notes and scripts |
| `tools/` | Build, deploy and benchmark scripts; `gyroprobe` sensor test tool |
| `bench/` | Performance measurements |
| `MODLOG.md` | Development journal: findings, offsets, dead ends |

From a source checkout, `tools/deploy-gyroue.sh` installs into a game (`remove` as a second argument
uninstalls; an existing `gyrolib.ini` is never overwritten):

```bash
bash tools/deploy-gyroue.sh "D:/Games/MyGame/MyGame/Binaries/Win64"
```

## Performance

GyroUE's per-frame work is roughly 50–100 µs on the game thread, plus 15–40 µs in the `Present` hook while the
panel is closed. Benchmarks in Returnal showed no measurable FPS or frametime change. See
[`bench/BENCHMARKS.md`](bench/BENCHMARKS.md).
