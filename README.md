
> [!IMPORTANT]
> This project is being superseeded by [Gyro4All](https://github.com/gfgerard/Gyro4All),
> for broader game support (not only Unreal Engine) and more features. 

---

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
- [Controllers the game doesn't support (universal controller support)](#controllers-the-game-doesnt-support)
- [Supported games](#supported-games)
- [Current scope](#current-scope): what GyroUE does and doesn't do (yet)

More:

- [Problems?](docs/TROUBLESHOOTING.md): common problems and how to report one
- [Advanced](docs/ADVANCED.md): technical details, game profiles, adding [support for a new game](docs/ADVANCED.md#finding-the-camera-of-a-new-game), building from source
- [Credits and licenses](docs/CREDITS.md)

**What you need**

- Windows 10/11 64-bit.
- A controller with a gyro. Tested with a **DualSense over USB/BT, Nintendo Switch Pro controller**.
- Close other controller tools while playing (**DS4Windows, DSX, reWASD**). You can use Steam Input 
  if you want but the mod should take care of input in any game automatically.


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

- **Gyro is on during gameplay** and pauses by itself in menus, the pause screen, the main menu and when you alt-tab.
- **Settings panel: press `F10`** (some games only accept it from the pause menu). Change sensitivity,
  how the gyro activates (always on, hold a button or trigger, toggle), invert axes, smoothing, acceleration
  curves and more. Games with several cameras (e.g. on foot / in car) get one tab each. Settings are saved
  automatically. GyroLib features that need hooks GyroUE doesn't have yet: see [Current scope](#current-scope).
- **Sensitivity profiles (ADS, ALT_ADS, OTHER).** Besides **Exploration** (the default, always on), the panel has
  an **ADS**, an **ALT_ADS** (for games with a second aim, e.g. a scope) and an **OTHER** tab. They are **off until
  you give them a button and a mode** in `GyroUE\GyroUE.ini`: until then their tab reads e.g. "ADS - disabled" and
  its description says which keys to set. An enabled profile starts with the Exploration settings
  (ALT_ADS with the ADS settings), so nothing changes until you edit its tab, and replaces them while its button is
  held, or from one press to the next. A game's profile can override these settings; restart the game after a
  change. They only apply to the main camera, not to other camera views such as a car.

  | Profile setting | Meaning |
  |---|---|
  | `AdsProfileButton=` | Button for the ADS profile: `LT RT LB RB LS RS A B X Y` (or `L2 R2 L1 R1 L3 R3 Cross Circle Square Triangle`), `Back Start Up Down Left Right Touchpad Misc`, paddles `LP1 LP2 RP1 RP2`. Empty = disabled. |
  | `AdsProfileMode=` | `Hold`: active while the button is held. `Toggle`: one press on, the next off; it also turns off on pause, menus and cutscenes, since GyroUE can't see whether the game is still aiming. Empty = disabled. |
  | `AltAdsProfileButton=`, `AltAdsProfileMode=` | Same for the ALT_ADS profile. |
  | `OtherProfileButton=`, `OtherProfileMode=` | Same for the OTHER profile. |

  Example: `AdsProfileButton=LT` and `AdsProfileMode=Hold`.

  When two profiles are active, the one activated last wins. The button still does its normal action in the game.
- **First time: calibrate.** Leave the controller still on a table for a few seconds while in a menu or the pause
  screen. GyroUE learns the sensor drift by itself and keeps it. The panel also has a **"Recalibrate gyro"**
  button.
- **No panel?** (DirectX 11 games such as Life is Strange) Open `GyroUE\gyrolib.ini` with Notepad while the game
  runs and save: changes apply within a second. Every setting is explained in GyroLib's
  [settings reference](https://github.com/lud-berthe/GyroLib).
- No gyro-camera movement after installation? see [add support for a new game](docs/ADVANCED.md#finding-the-camera-of-a-new-game) or [ask for a profile for it](https://github.com/gfgerard/GyroUE/issues)

### Controllers the game doesn't support

> [!NOTE]
> **version >=0.8.0**: Without Steam Input or a remmaper you can play any game ^GyroUE automatically emulates XInput if needed.
> Tested with a DualSense and a Switch Pro controller (Bluetooth).

Many PC games only understand Xbox controllers (XInput). Without Steam Input/xbox emulation (e.g. games from the Epic Games Store)
a DualSense, a Switch Pro controller or a DirectInput pad then does nothing in the game, except for GyroUE's gyro.
GyroUE can **show your controller to the game as an Xbox controller**: buttons, sticks, triggers and rumble.

- It's automatic (`XInput=1` in `GyroUE\GyroUE.ini`). GyroUE only does it when the game doesn't read the controller
  by itself and no Xbox controller is present, so games with native DualSense support, Steam Input, DS4Windows/DSX and
  real Xbox pads are left alone. In games that only support PlayStation controllers natively, other controllers
  (Switch Pro, DirectInput pads) are still shown as Xbox ones.
- A game whose own PlayStation support only knows the DualShock 4 (e.g. Mortal Shell) ignores a DualSense. GyroUE
  notices it, shows the DualSense as an Xbox controller for that session and tells you on screen to restart: from then
  on the game sees it as a DualShock 4 (PlayStation prompts, rumble, light bar). This is saved in the game's profile.
- About 2 seconds after the game starts, a notice says *"Controller shown to the game as an Xbox controller"*.
- The game shows Xbox button icons.
- Settings in `GyroUE\GyroUE.ini` (a game's profile can override them; restart the game after a change):

  | Controller input setting | Meaning |
  |---|---|
  | `XInput=1` | `0` off, `1` auto (default), `2` always: the game gets only the Xbox controller, even if it supports the DualSense itself (GyroUE keeps it from opening PlayStation/Nintendo controllers, so you lose PlayStation icons, adaptive triggers and haptics). Still off while a real Xbox controller, Steam Input or DS4Windows/DSX is active. |
  | `XInputNintendoLabels=0` | Face buttons: `0` by position (the bottom button is Xbox A), `1` swapped A&harr;B and X&harr;Y, so a Nintendo controller's printed A is Xbox A. Applies to any controller. |
  | `XInputTouchpadBack=1` | `1` the DualSense touchpad click also presses View (Back), like the Create button. `0` the game ignores it. |
  | `NativeFlickStick=0` | Experimental, off by default. `1` GyroLib's flick stick also works in games with their own PlayStation controller support (see [Current scope](#current-scope)). Needs `XInput=1`: with `XInput=2` the game doesn't read the controller itself (flick stick then works through the Xbox controller). `0` off. |

## Supported games

> [!TIP]
> **Your game isn't listed?** Most Unreal Engine 4/5 games work without any setup: just try it. If the camera
doesn't follow the gyro, GyroUE tells you on screen, and [Advanced](docs/ADVANCED.md#finding-the-camera-of-a-new-game) explains how to add support.

| Game | Status | What to know |
|---|---|---|
| Pacific Drive | ✅ Works | Gyro on foot **and** in the car. |
| Returnal | ✅ Works | Tip: in the settings panel, set the gyro to work only while L2/R2 is held. |
| Cronos: The New Dawn | ✅ Works | |
| Life is Strange Remastered | ✅ Works | **No `F10` panel** in this game: change settings in `gyrolib.ini` (see [Using it](#using-it)). |
| Luto | ✅ Works | |
| Out of sight | ✅ Works | |
| Terminator: resistance | ✅ Works | **No `F10` panel** in this game: change settings in `gyrolib.ini` (see [Using it](#using-it)). |
| The Outer Worlds: Spacer's Choice Edition | ✅ Works | Native DualSense support: GyroUE leaves the controller to the game. |
| Severed Steel | ✅ Works (tested on the demo) | **No `F10` panel** (DX11). Rename `MSVCP140.dll` and `MSVCP140_2.dll` in its `Binaries\Win64` folder (e.g. to `.bak`): it ships an old VC++ runtime that GyroLib can't use. Until you do, GyroUE leaves the gyro off and tells you so on screen. |
| Gears of War: E-Day (Game Pass) | ⚠️ Campaign only, **with EasyAntiCheat disabled** | Install in `Binaries\WinGDK`. The gyro starts a few seconds after the main menu appears. Never play online with it. |
| The Outer Worlds: Spacer's Choice Edition | ✅ Works |  |
| Hogwarts Legacy | ✅ Works | Needs custom profile |
| Mortal Shell | ✅ Works | **No `F10` panel** (DX11): change settings in `gyrolib.ini`. The game's own PlayStation support only knows the DualShock 4: its profile makes GyroUE show your DualSense (USB or Bluetooth) to it as one, with PlayStation icons, rumble and light bar. |
| Visage | ✅ Works | **No `F10` panel** (DX11): change settings in `gyrolib.ini`. |
| Valor Mortis | ✅ Works |  |
| PAYDAY 3 | ✅ Works |  |


## Current scope

GyroUE does one thing: **turn the game's camera with the gyro**. GyroLib can do more, but some of its
features only work if the mod also hooks the game in specific ways (hide the game's own stick input, filter
its button actions, move its camera on request, draw the panel). GyroLib leaves those hooks to the mod, and
GyroUE may not implement them yet:

| Feature | In GyroUE | Details |
|---|---|---|
| Gyro camera with calibration, smoothing, curves and an activation button | ✅ | The core of the mod. |
| Separate settings per camera (e.g. on foot / in car) | ✅ | Needs a game profile (shipped for the supported games). |
| Settings panel (`F10`) | ✅ DirectX 12 only | The panel needs rendering and window integration, which GyroUE has for DX12 only. DX11 games: edit `gyrolib.ini`. |
| Flick stick | ✅ In Xinput mode (auto / **XInput=2** to force it), and (⚠️ experimental) in games with their own PlayStation controller support | Needs the game's own right-stick rotation suppressed. GyroUE can do that for the controller it translates to XInput ([see above](#controllers-the-game-doesnt-support)), and for a DualSense/DualShock 4 the game reads itself through Sony's pad library (`NativeFlickStick=1`, off by default, with `XInput=1`: the game keeps PlayStation icons, haptics and adaptive triggers; the flick options appear after you move the right stick once in game). Otherwise GyroLib's panel hides the flick options, and the `context.N.flick.*` keys in `gyrolib.ini` are just stored preferences, don't edit them by hand. |
| Different sensitivity when aiming down sights or scoped | ✅ | 4 different profiles configurable with hold/toggle buttons
| Gyro button long press | ✅ | A button used to hold or toggle the gyro if pressed for more than 200ms. Do not use a button that has a long press action in-game. |
| Camera recenter button | ✅ **Off by default**; ⚠️ best effort in games with a custom camera profile | Levels the camera's pitch (up/down) when you press the button you pick in the settings panel's **Recenter** row (`context.N.camera.recenter_button` in `gyrolib.ini`, one per camera); its **+** sets how long it takes (0 = instant). Reliable with the generic camera. With a game profile's custom camera (e.g. a car camera) it may stop a bit above or below level, or not work well: it's optional, just leave it Off there. The button still does its normal action in the game: pick one the game doesn't need. |
