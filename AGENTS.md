# AGENTS.md

This file provides guidance to AI code agnets when working with code in this repository.

## Project

A universal gyro-aiming mod for PC games: DualSense (and other SDL-supported pads) motion sensors turn
the in-game camera, with no per-game code in the common case. The plan:
- gyro math and controller input come from **GyroLib** (https://github.com/lud-berthe/GyroLib, MIT;
  reviewed at v1.2.2, now pinned to **1.3.0** in `tools/gyrolib-sdk.sh`); we write the engine side;
- the first engine is **Unreal (UE4/UE5)** via a **UE4SS C++ mod**, because every UE game shares the
  reflected `APlayerController` camera path;
- per-game differences (menus, ADS, vehicles, custom cameras) go in small per-game profiles, not code.

Read these first:
- `gyrolib-ue-review.md`: the feasibility review. Why GyroLib, what the UE side must supply, the main
  obstacles and the first-prototype plan. Treat it as the design brief.
- `MODLOG.md`: the running journal and the source of truth for paths, versions, signatures, findings and
  next steps. Record every new finding there (exact class, function, property and offset names), and
  record dead ends too.

## Ground rules

- Never commit or ship game files, extracted assets, decompiled output or GyroLib's build artifacts that
  we didn't build. A release ships only our own code, GyroLib (with its MIT license) and UE4SS (with its
  license).
- Ask before installing loaders (UE4SS) into a game folder, editing game or registry settings, changing
  Steam Input settings, or deleting anything. Back up saves first with `um backup create`.
- Single-player / offline games without anti-cheat only (no EAC, BattlEye or kernel anti-cheat).
- Don't present untested behaviour as working. "Builds" is not "works in game"; verify in game and say
  which pad, connection (USB/BT) and Steam Input state it was tested with.
- Do not write local system paths, device hardware addresses, users or any sensitive information 
  into code comments nor *.md files
- Do not increase release version automatically. Ask for version to use if needed.

## Architecture (target)

```
DualSense --HID--> SDL3 (inside gyrolib.dll, or its isolated reader process)
          --> GyroLib: calibration, gravity fusion, gyro space, smoothing, curves,
                       activation, views (Camera / ADS / Scope), FOV scaling
          --> gl_update(now, host_state, &out) -> out.yaw_degrees / out.pitch_degrees
          --> our UE4SS C++ mod, on the game thread (engine tick):
                host_state  <- focus, IsGamePaused, bShowMouseCursor / input mode, pawn possessed
                active view <- L2 past half travel (default) or per-game profile signal
                FOV         <- PlayerCameraManager->GetFOVAngle()
                apply       -> SetControlRotation(GetControlRotation() + delta)
                               (or AddYawInput/AddPitchInput, scaled per UE version)
```

Key rules from the review:
- All GyroLib calls on **one owner thread** (the game thread via the UE4SS tick callback). Create the
  `gl_context` in `on_unreal_init`, never in `DllMain`.
- GyroLib output is already integrated degrees: don't multiply by dt.
- UE4 `AddYawInput` multiplies by `InputYawScale` (2.5; pitch -2.5) and UE5 deprecated those scales.
  Prefer `SetControlRotation`, which is true degrees on both.
- Steam Input may hide the physical pad or add its own gyro-to-mouse: test with it off and on, and watch
  for doubled motion.

## Layout (planned)

- `src/`: the UE4SS C++ mod (`GyroUE`), MSVC x64.
- `profiles/`: per-game JSON (ADS/menu signals, apply method, sensitivity defaults).
- `games/<Game>/`: per-game notes, deploy and package scripts, UE4SS config.
- `third_party/`: GyroLib and UE4SS sources or pinned releases (not committed if large; note versions in
  MODLOG).
- `build/release/`: installable release zips, `GyroUE-v<version>.zip` (git-ignored, like all of `build/`).

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
   `licenses/`, `README.md`, `TROUBLESHOOTING.md`, `ADVANCED.md`, `CREDITS.md`). It ships no `gyrolib.ini` or `GyroUE.ini` (both created on first run), so users keep
   their settings when they update.
3. Record the new version and what changed in `MODLOG.md`.

The zip is the asset to attach to a GitHub release tagged `v<version>`. Never put game files, extracted
assets or `gyrolib.ini`/`GyroUE.ini`/logs into it; if the script's file list shows anything else, fix the script.
