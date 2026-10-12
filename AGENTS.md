# AGENTS.md

This file provides guidance to AI code agents when working with code in this repository.

## Project

A gyro-aiming mod for Unreal Engine (UE4/UE5) games on PC: DualSense (and other SDL-supported pads) motion
sensors turn the in-game camera, with no per-game code in the common case. How it works today:
- gyro math and controller input come from **GyroLib** (https://github.com/lud-berthe/GyroLib, MIT;
  reviewed at v1.2.2, now pinned to **1.3.1** in `tools/gyrolib-sdk.sh`); we write the engine side;
- GyroUE is a **standalone system-DLL proxy** (`winmm.dll`, which also exports all of `version.dll` and can
  be renamed to it) with its own Unreal reflection reader. It needs no UE4SS: UE4SS was only the loader of
  the first prototype (MODLOG 2026-10-08) and is not used or shipped;
- most UE games turn the camera through the reflected `PlayerController.ControlRotation`, so the generic
  view needs no profile;
- per-game differences (custom cameras, vehicles, auto-recentering) go in small per-game INI profiles
  (`profiles/<exe stem>.ini`), not code. The camera probe (`Probe=1`) finds and writes them.
- Only `ue.cpp` and the scene/view/probe parts of `host.cpp` know Unreal. The loader, hooks, GyroLib
  wiring, XInput translation, native pad handling, overlay and notices are engine-neutral.

Read these first:
- `MODLOG.md`: the running journal and the source of truth for paths, versions, signatures, findings and
  next steps. Record every new finding there (exact class, function, property and offset names), and
  record dead ends too.
- `docs/ADVANCED.md`: how it works, the profile format, the camera probe ("Finding the camera of a new
  game") and the log reference.

## Ground rules

- Never commit or ship game files, extracted assets, decompiled output or GyroLib's build artifacts that
  we didn't build. A release ships only our own code (with MinHook linked in), GyroLib and their license
  notices (`GyroUE\licenses\`).
- Ask before installing GyroUE or any loader (e.g. UE4SS) into a game folder, editing game or registry
  settings, changing Steam Input settings, or deleting anything. Back up saves first with `um backup create`.
- Single-player / offline games without anti-cheat only (no EAC, BattlEye or kernel anti-cheat).
- Don't present untested behaviour as working. "Builds" is not "works in game"; verify in game and say
  which pad, connection (USB/BT) and Steam Input state it was tested with.
- Do not write local system paths, device hardware addresses, users or any sensitive information 
  into code comments nor *.md files
- Do not increase release version automatically. Ask for version to use if needed.

## Architecture

```
DualSense --HID--> SDL3 (inside gyrolib.dll)
          --> GyroLib: calibration, gravity fusion, gyro space, smoothing, curves, activation,
                       views (contexts), flick stick, settings panel
          --> gl_update(now, host_state, view) -> out.yaw_degrees / out.pitch_degrees
          --> GyroUE (winmm.dll proxy), on the game thread once per frame (end of the PeekMessageW pump):
                engine  <- GUObjectArray / FNamePool found by shape, reflection layout detected at runtime
                scene   <- GEngine -> GameViewport -> World -> OwningGameInstance -> LocalPlayers[0]
                           -> PlayerController, Pawn, PlayerCameraManager (view target, POV)
                host    <- focus, WorldSettings pauser, bShowMouseCursor, no gameplay view (DefaultPawn)
                view    <- the profile view whose ViewTargetClass matches the view target, else the default
                apply   -> view rotation += delta, written in memory (default: PlayerController.ControlRotation;
                           profiles: another reflected rotator, raw offsets, or WorldRotation=1)
```

Key rules:
- All GyroLib calls on **one owner thread**: the game thread, from the `PeekMessageW` hook (`dllmain.cpp`).
  The `gl_context` is created lazily on the first frame (`host.cpp`, `start_gyro`), never in `DllMain`;
  `DllMain` only registers hooks and reads (or creates) `GyroUE.ini` and the profile. Exception: the DX12
  panel's render calls run on GyroUE's own render worker thread (`overlay.cpp`), because UE moves
  `Present` between threads.
- GyroLib output is already integrated degrees: don't multiply by dt.
- Rotations are written straight into memory as degrees: float rotators in UE4, double in UE5 (the
  property's element size says which). Not through `AddYawInput`/`AddPitchInput`: UE4 multiplies them by
  `InputYawScale` (2.5; pitch -2.5) and UE5 deprecated those scales.
- Every engine memory access is guarded (`ue.cpp`): a bad pointer returns "not found" instead of crashing
  the game.
- One GyroUE per install folder (session mutex): other exes in that folder that load the proxy stay plain
  system-DLL proxies.
- Steam Input may hide the physical pad or add its own gyro-to-mouse: test with it off and on, and watch
  for doubled motion.

## Layout

- `src/GyroUE/`: the mod, MSVC x64, CMake.
  - `dllmain.cpp`: entry point and the per-frame `PeekMessageW` hook. `proxy.cpp`/`proxy.def`: forwarding
    to the System32 DLL with the proxy's own name (`winmm.dll` or `version.dll`).
  - `hooks.cpp`: import-table hooks (regular and delay-load) on the game modules (exe plus
    `<Project>-*.dll` of modular builds); inline MinHook hooks for protected exes.
  - `host.cpp`: the per-frame logic: GyroLib, scene, views and profiles, camera probe and live test,
    follow check.
  - `ue.cpp`: Unreal memory layout and reflection, and the probe's snapshot/diff.
  - `xinput.cpp`: the SDL controller shown to the game as XInput. `nativepad.cpp`: the game's own
    PlayStation pad access (Sony's pad library), for `XInput=2` and native flick stick.
  - `overlay.cpp`: GyroLib's DX12 settings panel. `notify.cpp`: on-screen notices (GDI). `deps.cpp`:
    Visual C++ runtime check before loading GyroLib. `log.cpp`, `perf.cpp`.
- `profiles/`: per-game INI profiles, `<exe stem>.ini` (format in `docs/ADVANCED.md`, "Game profiles").
- `docs/`: user documentation, shipped in the release zip.
- `games/<Game>/`: per-game notes and scripts (git-ignored).
- `third_party/`: the GyroLib SDK and MinHook (git-ignored; versions in MODLOG).
- `tools/`: build, package, deploy, benchmark and test tools (see Tooling).
- `bench/`: benchmark method and results.
- `build/release/`: installable release zips, `GyroUE-v<version>.zip` (git-ignored, like all of `build/`).
- In a game's `Binaries\Win64`: `winmm.dll` (or `version.dll`) and `GyroUE\` with `gyrolib.dll`,
  `profiles\`, `GyroUE.ini` (mod settings) and `gyrolib.ini` (GyroLib settings), both created on first run,
  and the logs (`GyroUE.log`, `GyroUE-probe.log`).

## Tooling

- `um` (universal-modder CLI) is on PATH: `um scan "<game>"`, `um backup ...`, `um win ...` (launch,
  screenshot, drive the game), `um publish check`.
- Visual Studio 2026 Community (v18, MSVC 14.51, cl 19.51). VS's bundled `cmake` and `ninja` are on the
  user PATH (added 2026-10-08; needs an app/terminal restart to show up). `cl`/`link` are not, because
  they also need `INCLUDE`/`LIB`: run them through `tools\vsdev.cmd`, which loads `vcvars64.bat` and then
  runs its arguments, e.g. `cmd //c 'tools\vsdev.cmd cl /nologo /EHsc foo.cpp'` from Git Bash.
  CMake with the default Visual Studio generator finds MSVC by itself; `-G Ninja` needs `vsdev.cmd`.
- Python 3.13 and uv are available for helper scripts. git is available; gh is not.
- Build GyroUE: `bash tools/build-gyroue.sh`. Build + release zip: `bash tools/package-gyroue.sh` (see
  "Releases" below). Install into any UE game:
  `bash tools/deploy-gyroue.sh "<game>\<Project>\Binaries\Win64" [remove]` (installs `winmm.dll`; prefix
  `PROXY=version.dll` to install under that name when `winmm.dll` doesn't load or another mod owns it; Gears of War:
  E-Day needs `version.dll`).
  Sensor test tool: `tools/gyroprobe` (`gyroprobe.exe [seconds]`, `--settings`).
- Performance: `bench/BENCHMARKS.md` (method, results, optimisation analysis); tools `tools/bench.sh`
  (modes vanilla / mod / mod-nopanel, PresentMon capture with Ctrl+Shift+F9 marks, report).
- MinHook 1.3.4 must be in `third_party/minhook-1.3.4` to build (see MODLOG 2026-10-09 for the clone command); it
  is only used for protected exes (inline hooks).
- To update GyroLib: download the release SDK into `third_party/GyroLib-<ver>/sdk`, verify
  SHA256SUMS, bump `tools/gyrolib-sdk.sh`, rebuild, and redeploy (users keep their `gyrolib.ini`).

## Releases

Every time you build a new version of GyroUE, also produce its release zip:

1. Bump the version in `project(GyroUE VERSION x.y.z ...)` in `src/GyroUE/CMakeLists.txt`. That is the
   only place it lives: the DLL's log line (`GyroUE x.y.z in ...`) and the zip name both come from it.
   Never hard-code the version anywhere else.
2. Run `bash tools/package-gyroue.sh`. It builds, checks that `winmm.dll` carries that exact version,
   and writes `build/release/GyroUE-v<version>.zip`. Extract the zip into a game's
   `<Project>\Binaries\Win64` folder: `winmm.dll` plus `GyroUE\` (`gyrolib.dll`, all `profiles/`,
   `licenses/`, and `readme/` with `README.md` and `docs/` (`TROUBLESHOOTING.md`, `ADVANCED.md`, `CREDITS.md`), as in the repository). It ships no `gyrolib.ini` or `GyroUE.ini` (both created on first run), so users keep
   their settings when they update.
3. Record the new version and what changed in `MODLOG.md`.

The zip is the asset to attach to a GitHub release tagged `v<version>`. Never put game files, extracted
assets or `gyrolib.ini`/`GyroUE.ini`/logs into it; if the script's file list shows anything else, fix the script.
