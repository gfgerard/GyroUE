# <img src="logo.png" alt="GyroUE logo" width="48px" />GyroUE

## Introduction
The purpose of this project is to provide a mod that, combined with
 [Gyrolib](https://github.com/lud-berthe/GyroLib), natively enables gyro in games developed with 
Unreal Engine.

**Aim with your controller's gyro in Unreal Engine games on PC.** Turn and tilt your DualSense (or any
controller with a gyro) and the in-game camera follows, on top of the normal sticks.
Avoid mixed input conflicts and UI flicker.
No Steam Input (although compatible if the game needs it), no mouse emulation, no game files changed: drop two files in, delete them to uninstall.

> [!WARNING]
> **Experimental.** Tested in the games below; other Unreal Engine 4/5 games often work too, but bugs and
> crashes are possible. **Offline single-player games only**: never use it in online or anti-cheat protected
> games.

## Contents

- [Installation / uninstall / update](#installation--uninstall--update)
- [Using it](#using-it)
- [Supported games](#supported-games)
- [Current scope](#current-scope): what GyroUE does and doesn't do (yet)

More:

- [Problems?](TROUBLESHOOTING.md): common problems and how to report one
- [Advanced](ADVANCED.md): technical details, game profiles, adding [support for a new game](ADVANCED.md#finding-the-camera-of-a-new-game), building from source
- [Credits and licenses](CREDITS.md)

**What you need**

- Windows 10/11 64-bit.
- A controller with a gyro. Tested with a **DualSense over USB/BT, Nintendo Switch Pro controller**.
- Close other controller tools while playing (**DS4Windows, DSX, reWASD**).
- **Steam Input:** if it's on for the game, you can set in the config panel how you prefer Steam input mouse used / not used.

## Installation / uninstall / update

#### Install

1. **Download** `GyroUE-vX.Y.Z.zip` from the [Releases](https://github.com/gfgerard/GyroUE/releases) page.
2. **Find the game's real exe folder.**  Usually it's the one with a file ending in **`-Win64-Shipping.exe`**, at 
   `<Game>\<Something>\Binaries\Win64`, **not** the game's main folder (that exe is only a launcher).
   Tip: search the game folder for `Shipping.exe`. It's where other mods like Optiscaler or Reshade go.
   
   Examples:

   | Game | Folder |
   |---|---|
   | Pacific Drive | `Pacific Drive\PenDriverPro\Binaries\Win64` |
   | Cronos: The New Dawn | `...\Cronos\Binaries\Win64` |
   | Life is Strange Remastered | `...\LIS\Binaries\Win64` |
   | Gears of War: E-Day (Game Pass) | `...\Content\FairlightConcept\Binaries\WinGDK` (rename `winmm.dll` to `version.dll`, see the note below) |

3. **Extract the zip into that folder.** You should end up with:

   ```text
   Binaries\Win64\
   ├── <Game>-Win64-Shipping.exe     (the game's exe, already there)
   ├── winmm.dll                     ← GyroUE
   └── GyroUE\                       ← GyroUE folder (settings, profiles, log)
   ```

4. **Play.** Once you're in control of your character, move the controller: the camera turns. Press **`F10`**
   for the settings panel.

> [!NOTE]
> **Nothing happens?** Another mod may already use a `winmm.dll`, or another program (e.g. RivaTuner) makes the
> game load the Windows one first (.log and .ini files should appear inside the **GyroUE** folder).
> Rename GyroUE's `winmm.dll` to **`version.dll`** (it works under both names) and try again.

#### Update

Extract the new zip over the old files and overwrite. Your settings (`gyrolib.ini`, `GyroUE.ini`)
are kept. Since 0.7.0 the documents are in `GyroUE\readme\`: the old `.md` files directly in `GyroUE\` can be
deleted.

#### Uninstall

Delete `winmm.dll` (or `version.dll` if you renamed it) and the `GyroUE` folder. Nothing else was
changed.

## Using it

- **The gyro is on during gameplay** and pauses by itself in menus, the pause screen, the main menu and when
  you alt-tab.
- **Settings panel: press `F10`** (some games only accept it from the pause menu). Change sensitivity,
  how the gyro activates (always on, hold a button or trigger, toggle), invert axes, smoothing, acceleration
  curves and more. Games with several cameras (e.g. on foot / in car) get one tab each. Settings are saved
  automatically. GyroLib features that need hooks GyroUE doesn't have yet (such as flick stick) are hidden in the panel or have
  no effect: see [Current scope](#current-scope).
- **First time: calibrate.** Leave the controller still on a table for a few seconds while in a menu or the pause
  screen. GyroUE learns the sensor drift by itself and keeps it. The panel also has a **"Recalibrate gyro"**
  button.
- **No panel?** (DirectX 11 games such as Life is Strange) Open `GyroUE\gyrolib.ini` with Notepad while the game
  runs and save: changes apply within a second. Every setting is explained in GyroLib's
  [settings reference](https://github.com/lud-berthe/GyroLib).

## Supported games

> [!TIP]
> **Your game isn't listed?** Most Unreal Engine 4/5 games work without any setup: just try it. If the camera
doesn't follow the gyro, GyroUE tells you on screen, and [Advanced](ADVANCED.md#finding-the-camera-of-a-new-game) explains how to add support.

| Game | Status | What to know |
|---|---|---|
| Pacific Drive | ✅ Works | Gyro on foot **and** in the car. |
| Returnal | ✅ Works | Tip: in the settings panel, set the gyro to work only while L2/R2 is held. |
| Cronos: The New Dawn | ✅ Works | Settings panel also works with HDR mods (RenoDX). |
| Life is Strange Remastered | ✅ Works | **No `F10` panel** in this game: change settings in `gyrolib.ini` (see [Using it](#using-it)). |
| Luto | ✅ Works | |
| Out of sight | ✅ Works | |
| Terminator: resistance | ✅ Works | |
| Severed Steel | ✅ Works (tested on the demo) | **No `F10` panel** (DX11). Rename `MSVCP140.dll` and `MSVCP140_2.dll` in its `Binaries\Win64` folder (e.g. to `.bak`): it ships an old VC++ runtime that GyroLib can't use. Until you do, GyroUE leaves the gyro off and tells you so on screen. |
| Gears of War: E-Day (Game Pass) | ⚠️ Campaign only, **with EasyAntiCheat disabled** | Install in `Binaries\WinGDK`. The gyro starts a few seconds after the main menu appears. Never play online with it. |



## Current scope

GyroUE does one thing: **turn the game's camera with the gyro**. GyroLib can do more, but some of its
features only work if the mod also hooks the game in specific ways (hide the game's own stick input, filter
its button actions, move its camera on request, draw the panel). GyroLib leaves those hooks to the mod, and
GyroUE doesn't implement them yet:

| Feature | In GyroUE | Why |
|---|---|---|
| Gyro camera with calibration, smoothing, curves and an activation button | ✅ | The core of the mod. |
| Separate settings per camera (e.g. on foot / in car) | ✅ | Needs a game profile (shipped for the supported games). |
| Settings panel (`F10`) | ✅ DirectX 12 only | The panel needs rendering and window integration, which GyroUE has for DX12 only. DX11 games: edit `gyrolib.ini`. |
| Flick stick | ❌ Not available | Needs the game's own right-stick rotation suppressed. GyroUE doesn't declare that capability, so GyroLib's panel hides the flick options; the `context.N.flick.*` keys in `gyrolib.ini` are just stored preferences, don't edit them by hand. |
| Gyro button that doesn't also act in the game | ❌ | GyroUE doesn't filter the game's input (no long-press blocking). A button used to hold or toggle the gyro still does its normal action in the game: pick one the game doesn't need. |
| Camera recenter button | ❌ | Needs the mod to recenter the game's camera on request (a host callback). |
| Different sensitivity when aiming down sights or scoped | ❌ | GyroUE doesn't detect aiming. Camera settings only change between profile views (e.g. a vehicle). |
