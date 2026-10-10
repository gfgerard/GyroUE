# <img src="logo.png" alt="GyroUE logo" width="48px" />GyroUE

**Aim with your controller's gyro in Unreal Engine games on PC.** Turn and tilt your DualSense (or any
controller with a gyro) and the in-game camera follows, on top of the normal sticks.
Avoid mixed input conflicts and UI flicker.
No Steam Input (although compatible thanks to Gyrolib), no mouse emulation, no game files changed: drop two files in, delete them to uninstall.

> [!WARNING]
> **Experimental.** Tested in the games below; other Unreal Engine 4/5 games often work too, but bugs and
> crashes are possible. **Offline single-player games only**: never use it in online or anti-cheat protected
> games.

## Contents

- [Supported games](#supported-games)
- [Installation](#installation)
- [Using it](#using-it)
- [Update and uninstall](#update-and-uninstall)
- [Problems?](#problems)
- [Advanced](#advanced): technical details, game profiles, adding support for a new game, building from source
- [Credits and licenses](#credits-and-third-party-licenses)

## Supported games

| Game | Status | What to know |
|---|---|---|
| Pacific Drive | ✅ Works | Gyro on foot **and** in the car. |
| Returnal | ✅ Works | Tip: in the settings panel, set the gyro to work only while L2/R2 is held. |
| Cronos: The New Dawn | ✅ Works | Settings panel also works with HDR mods (RenoDX). |
| Life is Strange Remastered | ✅ Works | **No `F10` panel** in this game: change settings in `gyrolib.ini` (see [Using it](#using-it)). |
| Luto | ✅ Works | |
| Gears of War: E-Day (Game Pass) | ⚠️ Campaign only, **with EasyAntiCheat disabled** | Install in `Binaries\WinGDK`. The gyro starts a few seconds after the main menu appears. Never play online with it. |

**Your game isn't listed?** Most Unreal Engine 4/5 games work without any setup: just try it. If the camera
doesn't follow the gyro, GyroUE tells you on screen, and [Advanced](#advanced) explains how to add support.

**What you need**

- Windows 10/11 64-bit.
- A controller with a gyro. Tested with a **DualSense over USB/BT, Nintendo Switch Pro controller**.
- Close other controller tools while playing (**DS4Windows, DSX, reWASD**).
- **Steam Input:** if it's on for the game, make sure Steam's own "gyro to mouse" is off, or the camera will
  move twice as much.

## Installation

1. **Download** `GyroUE-vX.Y.Z.zip` from the [Releases](../../releases) page.
2. **Find the game's real exe folder.** It's the one with a file ending in **`-Win64-Shipping.exe`**, usually
   `<Game>\<Something>\Binaries\Win64`, **not** the game's main folder (that exe is only a launcher).
   Tip: search the game folder for `Shipping.exe`. Examples:

   | Game | Folder |
   |---|---|
   | Pacific Drive | `Pacific Drive\PenDriverPro\Binaries\Win64` |
   | Cronos: The New Dawn | `...\Cronos\Binaries\Win64` |
   | Life is Strange Remastered | `...\LIS\Binaries\Win64` |
   | Gears of War: E-Day (Game Pass) | `...\Content\FairlightConcept\Binaries\WinGDK` |

3. **Extract the zip into that folder.** You should end up with:

   ```text
   Binaries\Win64\
   ├── <Game>-Win64-Shipping.exe     (the game's exe, already there)
   ├── version.dll                   ← GyroUE
   └── GyroUE\                       ← GyroUE folder (settings, profiles, log)
   ```

4. **Play.** Once you're in control of your character, move the controller: the camera turns. Press **`F10`**
   for the settings panel.

> [!NOTE]
> **Nothing happens?** Another mod may already use a `version.dll`, or the game doesn't load it. Rename
> GyroUE's `version.dll` to **`winmm.dll`** (same file, it works under both names) and try again.

## Using it

- **The gyro is on during gameplay** and pauses by itself in menus, the pause screen, the main menu and when
  you alt-tab.
- **Settings panel: press `F10`** (some games only accept it from the pause menu). Change sensitivity,
  how the gyro activates (always on, hold a button or trigger, toggle), invert axes, smoothing, flick stick and
  more. Games with several cameras (e.g. on foot / in car) get one tab each. Settings are saved automatically.
- **First time: calibrate.** Leave the controller still on a table for a few seconds while in a menu or the pause
  screen. GyroUE learns the sensor drift by itself and keeps it. The panel also has a **"Recalibrate gyro"**
  button.
- **No panel?** (DirectX 11 games such as Life is Strange) Open `GyroUE\gyrolib.ini` with Notepad while the game
  runs and save: changes apply within a second. Every setting is explained in GyroLib's
  [settings reference](https://github.com/lud-berthe/GyroLib).

## Update and uninstall

- **Update:** extract the new zip over the old files and overwrite. Your settings (`gyrolib.ini`,
  `GyroUE.ini`) are kept.
- **Uninstall:** delete `version.dll` (or `winmm.dll` if you renamed it) and the `GyroUE` folder. Nothing else
  was changed.

## Problems?

| Problem | What to do |
|---|---|
| Nothing happens and there is no `GyroUE\GyroUE.log` | GyroUE isn't loading. Check it's next to the `-Win64-Shipping.exe` (step 2), then try renaming `version.dll` to `winmm.dll`. |
| The camera doesn't move, but the log exists | Close DS4Windows/DSX/reWASD, plug the controller in by USB, check Steam Input. If a box at the top of the screen says **the game ignores the gyro**, the game needs a profile: see [Advanced](#finding-the-camera-of-a-new-game). |
| The camera slowly drifts on its own | Put the controller on a table for a few seconds in a menu, or use **"Recalibrate gyro"** in the panel. |
| The camera moves twice as much as it should | Steam Input (or another tool) is also turning the gyro into mouse movement. Turn that off. |
| `F10` does nothing | Try it from the pause menu. DirectX 11 games have no panel: edit `GyroUE\gyrolib.ini` instead. |
| The game crashes | Uninstall GyroUE and report it (below). |

**Reporting a problem:** open an issue with the game name and version, your controller and how it's connected
(USB / Bluetooth, Steam Input on or off), and attach `GyroUE\GyroUE.log` from the session where it went wrong.

---

# Advanced

Everything below is for the curious, for adding support for new games, and for developers. You don't need it to
play.

## Technical features

- **Drop-in install:** one proxy `version.dll` plus a `GyroUE` folder next to the game executable. No loader
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
  trigger, invert axes, smoothing, flick stick and more, saved to `gyrolib.ini`. Edits to `gyrolib.ini`
  are also reloaded live while the game runs.
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
| Gears of War: E-Day | UE 5.6 (Xbox / Game Pass build) | no | Generic path, campaign tested *WITH EAC DISABLED*. Install in `Binaries\WinGDK` as `version.dll`. Protected exe: GyroUE switches to inline hooks and finds the engine objects with a heap scan, so the gyro becomes available a few seconds after the main menu appears. The game ships EasyAntiCheat: don't use GyroUE in online modes. |
| Life is Strange Remastered | UE 4 (pre-4.25 property system), DX11 | yes (shipped) | Found with the camera probe. The camera splits pitch and yaw across two raw rotators, and the walking camera blends back to an automatic follow camera unless it gets look input; the profile writes `OnGyro=` to keep it manual. No settings panel (DX11): configure via `gyrolib.ini`. |
| Luto | UE 5.5.4 | no | Generic path working OOB |

Tested with a DualSense over USB/BT and Nintendo Switch Pro Controller. Requirements and limits:

- Windows 10/11 x64, DirectX 12 for the settings panel. Without a DX12 swapchain the gyro still works; only
  the panel is unavailable.
- **Single-player / offline games without anti-cheat only.** Do not use GyroUE with EasyAntiCheat,
  BattlEye or any online-protected game.
- UE 4 and UE 5. Both property systems are detected at runtime: FField (UE 4.25+, UE 5) and the older UProperty
  objects (tested with Life is Strange Remastered).
- The game must not already use a `version.dll` proxy. If it does (or the game doesn't load `version.dll`),
  rename GyroUE's `version.dll` to `winmm.dll`: the same DLL works under both names. The installer refuses to
  overwrite a foreign proxy.
- Close other pad remappers (DS4Windows, DSX, reWASD) while playing. If Steam Input is enabled for the game,
  check that Steam isn't also converting the gyro to mouse, or the motion will be doubled.

## Log reference

All diagnostics go to `GyroUE\GyroUE.log` (probe results, if enabled, to `GyroUE\GyroUE-probe.log`).

| Symptom | Check |
|---|---|
| No log file | `version.dll` is not next to the game exe (it must be in `Binaries\Win64` or `Binaries\WinGDK`), another mod's `version.dll` replaced it, or the game doesn't load `version.dll` (e.g. Xbox/GDK builds): rename it to `winmm.dll`. |
| "Entry point `timeGetDevCaps` not found" | A GyroUE older than 0.4.7 renamed to `winmm.dll`. Update: 0.4.7+ exports winmm too. |
| `motion=0` in the status line | The controller isn't visible to SDL: close remappers, try USB, check Steam Input. |
| `view=none` in gameplay | The pawn is missing or a placeholder. The game may need a profile. |
| `applied yaw` changes but the camera doesn't move | Custom camera: see [Finding the camera](#finding-the-camera-of-a-new-game). |
| Camera drifts slowly | Rest the pad in a menu for a few seconds (`bias=` in the status line should become non-zero) or use "Recalibrate gyro". |
| Panel doesn't open | Look for `overlay:` lines. Try from the game's pause menu. As a fallback, edit `gyrolib.ini` directly. |
| Double motion | Steam Input or another tool is also converting the gyro to mouse. |

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

- `version.dll` is a proxy for the system DLL of the same name. It also exports all of `winmm.dll`, so it can be
  renamed to `winmm.dll`; either way every export is forwarded to the System32 DLL with its own file name, so the
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

Most UE games turn the camera from `PlayerController.ControlRotation`, and GyroUE works with no profile.
Some games keep their own look rotation somewhere else: in a vehicle actor, in a camera component, or in
C++ memory that Unreal's reflection doesn't describe. The **probe** finds that rotation for you. It records
the camera-related state, you turn the camera with the right stick, and it logs what changed. The result
goes into the game's profile.

The steps below use the real Cronos: The New Dawn session as the example.

### 1. Check whether the game needs a profile

Install GyroUE, play for a few seconds moving the controller, quit, and open `GyroUE\GyroUE.log`. GyroUE compares
how much it turned the view with how much the camera actually moved, and says it in one line:

```text
camera: confirmed, the camera follows the gyro (ControlRotation, view 'Camera')
camera: the gyro turned ControlRotation (view 'Camera') by 412 degrees but the camera moved only 0: the game ignores it.
        To find the camera's real rotation, set Probe=1 in ...GyroUE\GyroUE.ini, restart and turn the camera
        with the right stick (README: "Finding the camera of a new game").
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
| `motion=0` | Controller problem, not a camera problem. See [Troubleshooting](#troubleshooting). |

### 2. Enable the probe

Open `GyroUE\GyroUE.ini` (GyroUE creates it with the defaults on the first run; releases don't ship it, so updates
keep your values) and set:

```ini
[GyroUE]
Probe=1
```

`GyroUE.ini` holds the settings for every game in that folder. A game's profile
(`GyroUE\profiles\<game exe>.ini`) can override any of them in its own `[GyroUE]` section, e.g. `Probe=1` for just
that game.

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
- The GyroLib **1.3.0** Windows x64 SDK, from the
  [GyroLib releases](https://github.com/lud-berthe/GyroLib/releases). Verify it against `SHA256SUMS` and
  extract it to `third_party/GyroLib-1.3.0/sdk/GyroLib-1.3.0-sdk-windows-x64/`. The pinned version is in
  `tools/gyrolib-sdk.sh`.

```bash
bash tools/build-gyroue.sh      # -> build/GyroUE/Release/version.dll
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

## Credits and third-party licenses

GyroUE is released under the [MIT License](LICENSE.txt).

### GyroLib

GyroUE depends on and redistributes **[GyroLib](https://github.com/lud-berthe/GyroLib)** (version 1.3.0,
commit `b241b7d`), which provides all controller input, gyro processing and the settings panel.

```text
MIT License

Copyright (c) 2026 GyroLib contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Components inside `gyrolib.dll`

`gyrolib.dll` embeds or compiles in the following components. Their full notices are taken unchanged from the
GyroLib SDK (`share/doc/GyroLib/licenses/`) and ship in every GyroUE release under `GyroUE/licenses/`:

| Component | License | Notice file |
|---|---|---|
| [SDL 3.4.16](https://github.com/libsdl-org/SDL) (altered build by GyroLib; the alteration notice ships as `SDL-CHANGES.md`, patches are in the GyroLib SDK under `share/doc/GyroLib/sdl-changes`) | zlib | `SDL.txt`, `SDL-CHANGES.md` |
| HIDAPI (vendored in SDL) | BSD-style | `HIDAPI-LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | MIT | `ImGui.txt` |
| [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers) by Julian "Jibb" Smart | MIT | `GamepadMotion.LICENSE` |
| ProggyClean / ProggyForever fonts | MIT | `FONTS-LICENSE.txt` |
| stb by Sean Barrett | MIT / public domain | `STB-LICENSE.txt` |
| GyroLib | MIT | `LICENSE` |

GyroUE's own license is included in the same folder as `GyroUE-LICENSE.txt`. `version.dll` statically links
[MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2-Clause, includes Hacker Disassembler Engine by
Vyacheslav Patkov); its notice ships as `MinHook-LICENSE.txt`.

GyroLib's own provenance record is `THIRD_PARTY.md` in its documentation (included in the SDK under
`share/doc/GyroLib/`). At runtime GyroLib
extracts its embedded `SDL3.dll` to `%LOCALAPPDATA%\GyroLib\runtime\<hash>\`, verifies it, and loads it from
there. It does not replace or touch a game's own SDL.

### Disclaimer

GyroUE is an unofficial fan project. It is not affiliated with or endorsed by Epic Games, Sony Interactive
Entertainment, Valve, or the developers and publishers of any game mentioned here. Unreal Engine, DualSense,
PlayStation and Steam are trademarks of their respective owners; they are named only to describe compatibility.
No game files, assets or decompiled code are included in this repository or its releases. Use at your own
risk, in offline single-player games only.

Development is AI assisted (Claude Code + Open Code with Qwen3.8).
