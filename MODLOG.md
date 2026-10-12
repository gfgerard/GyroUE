# MODLOG: universal gyro aiming for UE4/UE5 games

Goal: DualSense gyro turns the real game camera in standard UE4/UE5 single-player games, without mouse emulation 
or gyro to stick.

## Findings

(Record exact class/function/property names, signatures, offsets, UE versions and what worked or failed.)

- 2026-10-08 **GyroLib accepts external samples**: `gl_register_endpoint(ctx, &gl_endpoint)` +
  `gl_submit_sample(ctx, endpoint, &gl_sample{sensor_ns, arrival_ns, gyro_dps, accel_g})`. Rules
  (docs/INPUT.md): endpoint ids nonzero and unique (max 32); motion qualifies after 2+ increasing sensor
  timestamps with arrivals ≤ 100 ms apart; stale after 150 ms. SDL reader: `gl_sdl_create(ctx, 0)`; the
  host must pump SDL events on the owner thread. Steam: `gl_steam_runtime_create` borrows the loaded
  `steam_api64.dll`.
- 2026-10-08 **Games that ship Sony's `libScePad.dll`** (Pacific Drive) already drive the DualSense
  natively. Alternative sensor source: hook `scePadReadState` + `scePadSetMotionSensorState` and feed
  `gl_submit_sample`, which avoids a second HID reader. Details in `games/PacificDrive/MODDING_PLAN.md`.

- 2026-10-08 **gyroprobe** (`tools/gyroprobe`, built with VS CMake against the GyroLib 1.2.2 SDK in
  `third_party/`, which is git-ignored): headless `SdlInput` sees "DualSense Wireless Controller" over USB,
  source SDL, ~490 accepted samples/s. Before calibration a stationary pad drifts ~0.6 deg/s
  (`calibration_state=0`), so the mod must calibrate (`gl_begin_calibration` / auto-calibration).
- 2026-10-08 **Pacific Drive + UE4SS v3.0.1-1028** (release zip, `dwmapi.dll` +
  `ue4ss\` in `PenDriverPro\Binaries\Win64`, settings: UE 4.27, console on, hot reload on):
  - AOB scan OK, except `ConsoleManagerSingleton` (2 candidates; only matters for console commands).
    `GameEngineTick` found. Lua mods start; GyroCamProbe loads.
  - **GUI console crashes the game at startup**: a stack overflow (AV write just below the stack limit)
    with UE4SS, `gameoverlayrenderer64` (Steam overlay), `d3d12` (SCSKiller proxy), ReShade `dxgi` and
    `nvoglv64` (the UE4SS OpenGL GUI) recursing on the stack. Fix: `GuiConsoleEnabled = 0`. The text
    console is fine (diagnosed from a Windows crash dump).
  - BPModLoaderMod created an empty `LogicMods` folder in the game's Paks.
- 2026-10-08 **Pacific Drive camera path (GyroCamProbe v1, first save, USB, Steam Input off)**:
  - PC = `BP_PlayerController_MainCharacter_C`, pawn = `PDP_MainCharacter_C` **both on foot and in the
    car** (no vehicle possession; at the main menu the pawn is `DefaultPawn`). PCM class = `PlayerCameraManager`, FOV 90.
  - **On foot:** camera rotation == `ControlRotation`. `SetControlRotation` moves the view in exact degrees
    (yaw +10 → +10, pitch +5 → +5). `AddYawInput(10)` → +25° (`InputYawScale=2.5`,
    `InputPitchScale=-2.5`). So on foot the plan's `SetControlRotation(GetControlRotation()+delta)` works.
  - **In the car:** `ControlRotation` still changes with both methods, but the camera doesn't move
    (the PCM rotation stays at the car heading ± small head motion). The car view uses its own rotation
    (camera component / free-look variable), so it needs a per-game "InCar" view → probe v2 (F5/F11
    snapshot diff while looking around with the right stick).
  - `bShowMouseCursor=false`, `IsLookInputIgnored=false`, `Paused=false` in gameplay (pause menu not tested yet).
  - F6 works but only writes to the log (shipping build: nothing on screen).
  - Probe keys to avoid: **F11** (windowed/fullscreen toggle). F10 was Steam's screenshot key; the user
    moved it in Steam on 2026-10-08, so F10 is free now.
- 2026-10-08 **Pacific Drive car camera (probe v2 snapshot diff, right-stick look in the car):**
  - In the car: view target = attach parent = **`BP_PlayerCarNew_C`**. Active camera:
    `BP_PlayerCarNew_C.FirstPersonCamera` (CameraComponent).
  - Stick look writes **`CameraControllerRotation_Target`** (FRotator on the car). It is smoothed into
    **`CameraControllerRotation_Current`**, which is copied into `FirstPersonCamera.RelativeRotation` (and
    `LastUsedRotationCameraRotation`, `3rdPersonSpring.RelativeRotation`).
  - The yaw seems clamped at **±140°** (stick held at the limit gave exactly 140.000).
  - Auto-recentering: `IsAutoControlling` 1→0, `Camera_AutoControlAmount_current/_target` 1→0 while the
    player looks; `TimeNotSteering` (float) keeps counting. These probably bring the view back to the
    front when the player stops looking.
  - Only yaw was tested; pitch in the car is unknown.
  - Probe v3 (F7/F9 in the car add to `CameraControllerRotation_Target`): **direct writes work**.
    Yaw +10 and pitch +5 land exactly, `_Current` follows within ~50 ms, `FirstPersonCamera` turns, and
    the value holds for 3+ s (no recentering while parked; `IsAutoControlling` stays false). The BP clamps
    yaw at **140** (writes past it stay at 140.00). Stick look-up also writes the Target pitch. Pitch
    limits are not measured yet (seen -19.8 to +11.5).
  - Recentering while *driving* (`TimeNotSteering`, `Camera_AutoControlAmount`) is not tested yet.
    It may pull a gyro-moved view back to the front. Check it in the C++ mod.
- 2026-10-08 **Coexistence confirmed by user**: with gyroprobe's SDL reader running, the game's native
  DualSense haptics and adaptive triggers kept working (USB, Steam Input off). So SDL is OK as the sensor
  source for Pacific Drive, and the libScePad hook fallback isn't needed for now.
- 2026-10-08 **GyroLib settings** (`gyroprobe --settings` lists every id, range and choice). Why
  calibration never ran: `calibration.automatic` defaults to **0 = Off** (1 = menus only, 2 = any time).
  The panel key `ui.menu_key` defaults to **F10** (free since the user moved Steam's screenshot key).
  The pad shortcut Back+Start = DualSense Create+Options; check it doesn't also trigger game actions.
  Settings that need host support:
  - `camera.recenter_button` → recenter callback;
  - `flick.*` → suppress the game's right stick;
  - `gyro.zoom_compensation` → FOV (`GetFOVAngle`).
- **Pacific Drive apply rule** (draft profile `profiles/pacific-drive.json`):
  - pawn attached to a `BP_PlayerCarNew_C` → add the deltas to
    `car.CameraControllerRotation_Target`;
  - otherwise → `SetControlRotation(GetControlRotation() + delta)`.
  - Degrees are real in both paths, so no `InputYawScale` conversion is needed.
- 2026-10-08 **SDL + libScePad coexistence (gyroprobe, 11 min alongside gameplay):** GyroLib's SDL reader got
  continuous DualSense motion (~460 samples/s, 6 rejected, 0 dropped, no errors) while the game ran with
  its native DualSense. `calibration_state` stayed 0 all the time, so auto-calibration doesn't kick in on
  its own; the mod must call `gl_begin_calibration` (or check the settings). Effect on haptics/triggers:
  pending user report.

- 2026-10-08 **GyroUE v0.1 works in Pacific Drive** (user confirmed: on foot and in the car; stops in menus/pause).
  - Standalone `version.dll` proxy (`src/GyroUE`); UE4SS disabled (`dwmapi.dll.off`). Gyro frame runs from an
    IAT hook of `PeekMessageW` on the game thread.
  - Shape-scan found GUObjectArray (exe+0x54722B0, item stride 0x18) and FNamePool.Blocks (exe+0x5435D10).
  - Detected layout matches UE4SS: SuperStruct 0x40, ChildProperties 0x50, FField.Next 0x20, Name 0x28,
    ElementSize 0x3C, Offset_Internal 0x4C, sizeof(FProperty) 0x78. Engine class `DrivingGameEngine`.
  - The first build failed to detect `sizeof(FProperty)` because the size loop started unaligned and skipped 0x78.
  - Main menu: PC `BP_MainMenu_PlayerController_C`, pawn `DefaultPawn`, cursor=1 → camera off.
    Pause: `WorldSettings.PauserPlayerState` set (paused=1).
  - `gl_diagnostics.calibration_state` stays 0. That field seems to track *manual* calibration only. The status line
    now logs `bias`/`stationary` to check whether automatic menu calibration learns a bias.
  - Settings panel: v0.1 disables F10/Create+Options on purpose, because there's no frontend yet.
    `gyrolib.ini` edits reload live (1 s poll). An in-game panel needs GyroLib's DX12 overlay:
    - hook `IDXGISwapChain::Present` and capture the DIRECT `ID3D12CommandQueue`;
    - forward window messages;
    - gate game input while the panel is open.
    That's risky next to ReShade + SCSKiller + Steam overlay (the UE4SS GUI already crashed there).

- 2026-10-08 **GyroUE v0.2: in-game settings panel works** (GyroLib DX12 overlay, F10 / Create+Options).
  - It's gated by `gyrolib.ini`: if `ui.menu_key=` is empty and `ui.gamepad_menu_shortcut=0`, no graphics hooks
    are installed (read in DllMain before the renderer starts).
  - Hooks (all inside our own module, no inline patching):
    - IAT of `CreateDXGIFactory`/`CreateDXGIFactory1`;
    - factory vtable: CreateSwapChain (10) and CreateSwapChainForHwnd (15), which capture the swapchain and
      its DIRECT queue;
    - swapchain vtable: Present 8, ResizeBuffers 13, Present1 22, ResizeBuffers1 39;
    - WndProc subclass that forwards to `gl_overlay_win32_message`.
  - Pacific Drive with ReShade + RenoDX + SCSKiller + Steam overlay: swapchain captured about 0.7 s after
    start, panel ready, no crash. Without a renderer (non-DX12), GyroUE force-closes the panel so it can't
    hold the gyro invisibly.
  - Automatic calibration ("Menus only") works: bias learned at the main menu, (0.10 -0.08 -0.25) deg/s.
  - Known limitation: while the panel is open, gamepad input still reaches the game (only mouse/keyboard are gated).

- 2026-10-08 **GyroUE v0.2 user test in Pacific Drive: everything works, no crashes.**
  - The panel saves to `gyrolib.ini` and GyroUE reloads the file each time (9 reloads, all `result 0`).
    User settings now:
    - `ui.language=es`;
    - on foot: `gyro.activation=4` (toggle), `gyro.smoothing_ms=10`;
    - in car: sensitivity 2.0 X/Y, which is a local exception; the rest is inherited (`context.2.inherit=1`).
  - Toggle mode works (`enabled` flips 1/0) in both views. Car/foot switching is correct over repeated
    entries and exits. The automatic menu calibration learned bias (0.24 -0.19 -0.65) deg/s, which
    matches gyroprobe's drift.
  - Correction: the panel's "Inherit settings from: None" was the On-foot tab (the root view); the car does inherit.
- 2026-10-08 **GyroUE v0.2.1**: no gameplay view while the pawn is a `DefaultPawn` (main menus; SpectatorPawn
  derives from it). v0.2 applied a tiny −0.1/−0.9° to the main-menu camera when the cursor was hidden.
  Verified: main menu → `view=none`, applied 0.

- 2026-10-08 **Bug (user): in the car, while driving (R2), the gyro stops moving the view after a while; a right-stick
  look brings it back.** It doesn't happen parked. Likely cause: the car BP's camera auto-recentering
  (`IsAutoControlling`, `Camera_AutoControlAmount_*`) engages while moving without stick look input and drives
  `CameraControllerRotation_Target` back to the front every tick. The stick path cancels it; our property write doesn't.
  - GyroUE v0.2.2 adds generic profile keys per view:
    - `OnGyro=Prop:value,...`: scalar writes on the view object whenever the gyro moves the view;
    - `Watch=Prop,...`: log on change;
    - `Dump=1`: log all properties once.
    These are supported for Float, Double, Int, Byte and Bool properties.
  - Pacific Drive InCar: `OnGyro=IsAutoControlling:0,Camera_AutoControlAmount_target:0,Camera_AutoControlAmount_current:0`
    (the values the game sets on stick look). **Fixed, user-verified while driving.** The watch log showed
    the BP re-arming auto-control 6 times while driving (`IsAutoControlling=true`,
    `Camera_AutoControlAmount_target=1`), and each time OnGyro cancelled it about 3 ms later. Releasing
    the gyro still lets the game recenter normally.
  - Car camera properties from the Dump (BP_PlayerCarNew_C, 4.27, float rotators):
    - `CameraControllerRotation_Current/_Target` @0xAF4/0xB00;
    - `CameraControllerRotationSpeed` 55 (7 while snapping);
    - `CameraYawMinMaxAngles` / `CameraPitchMinMaxAngles` (Vector2D);
    - `IsAutoControlling` @0xB68;
    - `AutoRecenteringSpeedSlow/Fast` 0.3/1.3;
    - `AutoControlRotCurrent/Target`;
    - `TimeSteering` / `TimeNotSteering`;
    - `Camera_AutoControlAmount_current/_target` @0xC28/0xC2C.
  - Diagnostics are disabled again in the profile; only OnGyro stays.

- 2026-10-08 **Second game: Returnal (Steam 1649240), UE 4.25.1 licensee, MODULAR build** (264 `Returnal-*-Win64-Shipping.dll`).
  - No anti-cheat found (EOS SDK present for co-op). It ships `libScePad.dll` (native DualSense) and has ReShade
    (`dxgi.dll`, RenoDX UE addon), SCSKiller (`d3d12.dll`) and Streamline (DLSS) modules.
  - Saves backed up before testing.
  - **GyroUE v0.3** makes the DLL work for modular builds:
    - every import hook applies to the exe + all `<Project>-*.dll` modules, and modules loaded later are
      patched from hooked `LoadLibrary*`;
    - memory scans cover all game modules.
    Returnal: `version.dll` is imported by Core, `PeekMessageW` by ApplicationCore/Core, and DXGI by
    DXBridge/StreamlineD3D12RHIPreInit (loaded later).
  - **Result without any profile or game-specific code:**
    - GUObjectArray found in `Returnal-CoreUObject…dll+0x2F6D28`, FNamePool in `Returnal-Core…dll+0x5C2950`;
    - same 4.25+ layout as Pacific Drive; engine `GameEngine`;
    - PC `BP_PlayerController_C`, pawn `BP_PlayerCharacter_C`; the generic ControlRotation view is active;
    - menu calibration learns a bias.
  - **Panel bug found and fixed (v0.3.2):** Returnal calls `ResizeBuffers` on a different thread than
    `Present` (RHI thread 9528 vs 13112). GyroLib only accepts render calls on its one render thread, so
    `before_resize` returned -1. The panel kept its backbuffer references, ResizeBuffers failed, and the
    image froze on the black loading screen (audio, vibration and gyro kept running).
    Fix: while the panel is closed, release GyroLib's backbuffer references at the end of every frame on the
    render thread (nothing is drawn, so it costs nothing). Resizes from other threads are safe now. Still
    open: a resize from another thread *while the panel is open* would still fail (it's logged).
    v0.3.2 verified: loads into gameplay; F10 panel opens (single "Camera" tab) and closes.
  - Generic installer: `tools/deploy-gyroue.sh "<Win64 dir>" [remove]`. It never overwrites `gyrolib.ini` and
    refuses to replace a foreign `version.dll`.
  - **User gameplay test (v0.3.2): works, no crashes.**
    - About 7 min in combat with the generic ControlRotation view;
    - pause/menus gate the gyro (`paused=1 cursor=1` → `enabled=0`);
    - menu calibration bias (0.17 -0.15 -0.62) deg/s.
    The user configured the panel: `gyro.activation=3` (hold to enable) with `activation.trigger=3`
    (L2 or R2, threshold 0.2), so the gyro is active only while aiming/shooting. Triggers read through SDL
    work while the game reads the pad through libScePad. Haptics and adaptive triggers: OK (user).
  - **Panel shortcuts in Returnal:**
    - A physical F10 didn't open the panel: F10 is a system key, and the game doesn't deliver it as a normal
      WM_KEYDOWN. My SendInput test had worked.
    - Create+Options opened the panel and closed it again at once: Options also opens the game's pause menu.
      GyroLib closes the panel when owner snapshots are older than 250 ms, probably because of the hitch
      when the pause menu opens.
    - **v0.3.3:** GyroUE polls the configured F-key (`GetAsyncKeyState`) on the game thread, toggles the
      panel with `gl_overlay_set_open`, and no longer forwards that key's messages to GyroLib (so it can't
      toggle twice). Returnal's `gyrolib.ini` now has `ui.gamepad_menu_shortcut=0`.
    - Verified F10 opens/closes the panel in Returnal (single Camera tab, the user's L2/R2 hold activation shown).
      Physical F10 confirmed by the user, **but only from the game's menus/pause; during gameplay F10 doesn't
      open the panel**. The user accepts this. The cause isn't diagnosed: the log has no panel-state lines.
      If needed, log `gl_panel_open` / `gl_overlay_capture` on each toggle request to see whether GyroLib
      refuses to open or closes the panel immediately in gameplay.
    - Panel edits in the pause menu saved and reloaded correctly (several `gyrolib.ini changed, reloaded (0)`).

- 2026-10-08 **GyroLib updated to 1.3.0** (commit b241b7d; SHA256SUMS verified for the SDK, the demo and
  `gyrolib.dll` 04f32087…). It's a drop-in: ABI 1 and INI schema 0.2.0 are unchanged. Relevant fixes:
  - "Recalibrate gyro" works (the panel used to revert it);
  - manual calibration is more tolerant and is saved per SDL sensor across restarts;
  - Steam mouse Block/Convert fixes.
  New host rule: report menu state independently of camera permission. GyroUE already does
  (`menu_open` = cursor; `camera_allowed` is separate).
  - SDK version pinned in `tools/gyrolib-sdk.sh`, which is used by `tools/build-gyroue.sh` (reconfigures when
    the pin changes) and both deploy scripts. gyroprobe was rebuilt too.
  - GyroUE v0.3.4 logs the SDK it was built with. Deployed to Pacific Drive and Returnal; Returnal smoke test
    OK (settings kept, panel ready, Camera view).
  - `third_party/GyroLib-1.2.2` deleted (unused).

- 2026-10-08 **Performance:** benchmarks and per-frame cost analysis live in `bench/BENCHMARKS.md`.
  Summary: no measurable FPS/frametime impact in Returnal (GPU-bound).


- 2026-10-08 **Third game: Cronos: The New Dawn (Bloober Team), UE 5.4.4, monolithic** (`Cronos\Binaries\Win64`).
  - GyroUE 0.3.6 with no profile: GUObjectArray at exe+0xAD91670, FNamePool at exe+0xACADED0, same layout as 4.25+
    but FField.Next 0x18 / Name 0x20 / ElementSize 0x34 / Offset_Internal 0x44 / sizeof(FProperty) 0x70 (UE 5.4).
    Engine `GameEngine`. ReShade + RenoDX present; DX12 panel works (user).
  - Main menu: PC `PlayerController`, pawn `DefaultPawn`, view target `CameraActor` → `view=none` (correct).
  - Gameplay: PC `SHPlayerControllerPlay_BP_C`, pawn/view target `CronosCharacterPlay_BP_C`. The generic view writes
    `ControlRotation` (applied yaw reached -220°) **but the camera doesn't move**. The game's camera uses another
    rotation (the PC class prefix `SH` = Bloober's Silent Hill 2 Remake framework).
  - GyroUE 0.3.7 adds a generic probe (`[GyroUE] Probe=1`): F7 snapshots every float/double/rotator (nested structs
    up to 3 levels) of PC, pawn, PlayerCameraManager, view target and every object whose Outer chain reaches them
    (components); F8 logs what changed.
  - **Probe result (right-stick look, 2 runs):** the stick does **not** change `ControlRotation` (only the gyro's
    ~0.6°). Changed rotators, all outputs: `SHPlayerCameraManagerPlay.ViewTarget/CameraCachePrivate/
    LastFrameCameraCachePrivate.POV.Rotation`, the PCM's `TransformComponent0.RelativeRotation`,
    `Animation(SHCharacterPlayAnimComponent).LastAimRotation`. Also `SpringArm(SHCharacterPlaySpringArmComponent)
    .TargetArmLength` (180 → 102 when looking up), anim `AimOffset.Y` = pitch, `CameraYawDifference`.
    So the look state is non-reflected C++ (probably inside `SHPlayerCameraManagerPlay`).
  - GyroUE 0.3.8: the probe also diffs the raw memory of every probed object (up to `PropertiesSize`, read at
    ChildProperties+8) and logs aligned floats/doubles that now equal the POV yaw/pitch (deg or rad) or changed by
    the POV delta.
  - **Raw probe result (2 runs, yaw +148/+179, pitch +55/+61):** most matches are output copies (PCM +0x380/0x1450/
    0x1C90 = POV, component transforms, anim). The look state candidate is the pawn's
    **`View` (`SHCharacterPlayViewComponent`)**: two non-reflected **double rotators at +0x2A0 and +0x420** (P, Y, R)
    that equal the POV in both runs. `SpringArm`+0x2D8 and `Animation`+0x1C0 also match (world rotation / LastAim).
  - GyroUE 0.4.0: views can target any object and raw offsets. Profile keys `Object=Root.Sub...` (Root = PC, Pawn,
    Camera, ViewTarget; each Sub is an object property or a subobject name, cached), `Offsets=0x..,0x..`,
    `RotatorSize=24|12`. The status line logs the raw rotators. Cronos profile: `Object=Pawn.View`,
    `Offsets=0x2A0,0x420` (both get the delta), `Dump=1`.
  - **User test (0.4.0): works, "perfectly acceptable".** About 3 min of gameplay. Gyro turns the camera and
    aim; the right stick still works; pause gates the gyro. Both offsets always hold the same value. While paused
    they read (P 0, Y -115) (not used). The Dump of `View` printed no reflected float/rotator properties.
  - Calibration gap: the Cronos main menu and pause menu show no cursor with a gamepad (`cursor=0`), so
    `menu_open` was false and "menus only" auto-calibration rarely ran (bias stayed 0 most of the session; the
    panel's ini reloads also reset it to 0). **GyroUE 0.4.1:** `menu_open = !pc || cursor || paused || no view`
    (generic). Profile cleaned up (Probe/Dump off). Pacific Drive/Returnal still run 0.3.x.
  - **0.4.1 test:** calibration fixed. Bias learned in the main menu with `cursor=0` within 10 s
    ((0.15 -0.14 -0.57) → (0.17 -0.16 -0.64)), thanks to "no gameplay view". The pause menu shows `cursor=1` only
    after a keyboard key is touched.
  - **New bug: the F10 panel didn't appear in that session** (it worked with 0.4.0). The GyroUE log has nothing on it,
    because F10 requests, panel state and render results weren't logged. ReShade logged a
    `ResizeBuffers(2, 3840x2160, fmt 24)` at the main menu on the **game thread 34404** (render thread 31108). The
    panel was closed and buffers released, so it should be safe, but the next render must reacquire buffers.
    GyroUE 0.4.2 logs F-key requests, panel open/closed, panel drawn/hidden, render results (and re-inits the renderer
    once on a render error), ResizeBuffers HRESULTs and Presents on uncaptured swapchains.
  - User hint: the panel worked while the game's HDR was **off**; with HDR on, the **RenoDX** add-on (ReShade) takes
    over. Hypothesis: RenoDX upgrades the swapchain format behind the game's back (the game passes RGB10A2 = 24 to
    ResizeBuffers, the real buffers become RGBA16F), so GyroUE saw "no format change", skipped the required GyroLib
    shutdown/init, and the panel stopped drawing silently. **GyroUE 0.4.3:** after every ResizeBuffers(1) it reads
    the real `GetDesc` count/format and re-inits the panel renderer if they differ (logged as "real buffers ...").
    Tested with HDR on.
  - **0.4.3 log disproves the format hypothesis**: every ResizeBuffers keeps 2 buffers, format 24 (real = requested;
    ReShade also sets `SetColorSpace1(12)` = HDR10). **Real cause: UE moved Present to another thread.** The panel
    was initialized on thread 10864. After the 3rd ResizeBuffers (game thread 33312, 28 s after start), Present came
    from thread 33348 and `gl_overlay_dx12_render` returned -1 "render requires initialized render thread". 0.4.2's
    retry made it worse: shutdown from the new thread failed too (-1), then init failed with "Overlay already
    initialized/detached", so the panel was disabled for the session (F10 requests then returned -2). Thread 10864 was
    still alive (it called SetFullscreenState at exit). This is probably also why F10 didn't work in Returnal gameplay.
  - **GyroUE 0.4.4: render worker.** Every GyroLib render call (init/render/before_resize/shutdown) runs on GyroUE's
    own thread. The presenting or resizing thread hands the job over and waits, which keeps the panel drawn before
    the game's Present, whatever thread UE uses. While the panel is closed and holds no buffers, Present doesn't call
    GyroLib at all (zero per-frame cost). The cross-thread resize problem (Returnal) is handled the same way. A worker
    stuck for 5 s disables the panel. The 0.4.2 render-error retry was removed.
  - **0.4.4 test: F10 still didn't open.** The worker initialized fine, no render errors, and every F10 request
    returned 0, but `gl_panel_open` never became 1. Cause: the "skip GyroLib while closed" optimisation. GyroLib
    refuses/closes the panel when the render side is stale (> 250 ms, see overlay.h). **Rule: call
    `gl_overlay_dx12_render` every frame, even while closed.** 0.4.5 renders every frame through the worker again.
  - **0.4.5 user test (HDR on, RenoDX, ReShade): panel works in menus and in game; no stutter with it closed.**
    Log: 3 ResizeBuffers (all format 24), then 5 open/close cycles in the main menu and in gameplay
    (`panel state open` → `panel drawn (capture 7)` → hidden/closed), one ini save reloaded (0).
    Present hook cost with the panel closed (worker handoff + GyroLib render + buffer release): avg 16–20 µs,
    p99 29–42 µs, about the same as 0.3.x in Returnal. With the panel open, Present averages 4–7 ms (p99 ~15 ms),
    the same as before the worker (0.3.6 showed 6–11 ms open). That is GyroLib's own panel draw/fence wait, not a regression.
  - Fallback if a game still breaks the panel (user's idea): detect the failure, not the mods. Disable the panel for
    the session with a clear log line ("configure via gyrolib.ini, reloads live"); optional profile `Panel=0`;
    README section. Not needed for Cronos.

- 2026-10-08 **README.md** written for GitHub: contents, install, profiles, a probe walkthrough, GyroLib MIT notice and a
  third-party table. `tools/deploy-gyroue.sh` now ships `GyroUE/licenses/` (the GyroLib SDK licenses,
  `SDL-CHANGES.md`, `GyroUE-LICENSE.txt`), as GyroLib's THIRD_PARTY.md requires. `LICENSE.txt` still has the template
  placeholders (`[year] [fullname]`).
- 2026-10-08 **GyroUE 0.4.6:** probe lines are labelled with the profile path of each object
  (`Pawn.View(SHCharacterPlayViewComponent)`, `Camera.TransformComponent0(SceneComponent)`). The roots are tried in
  the order PC, Pawn, Camera, ViewTarget, so the part before "(" can be copied into `Object=`. Builds; not run in game.
- 2026-10-09 **Gears of War: E-Day** (Xbox/Game Pass GDK build, UE 5.6 non-modular per UE4SS log).
  Exe `<Xbox games folder>\Gears of War- E-Day\Content\FairlightConcept\Binaries\WinGDK\GoWEDay.exe` (encrypted/ACL'd:
  pefile/dumpbin can't read it, so its import table is unknown). Already present: UE4SS as `dwmapi.dll`, ReShade as
  `dxgi.dll` (+ RenoDX `renodx-mfgunlock.addon64`), `EACbegone.bat`.
  - User report: default install (`version.dll`) wrote no log; renamed to `winmm.dll` it failed with "entry point
    `timeGetDevCaps` not found".
  - Cause 1 (no log): the DllMain gate only accepted `*-Shipping` / `*-Win64-*` exe names and returned before
    `log_open`, so `GoWEDay.exe` was silently skipped. Whether the exe imports `version.dll` is still unverified.
  - Cause 2 (winmm): the DLL only exported the 17 version.dll functions; the exe statically imports winmm.
  - **GyroUE 0.4.7:** gate also accepts any exe whose folder's parent is `Binaries` (`Binaries\WinGDK\GoWEDay.exe`).
    The proxy exports version.dll + all 180 named winmm.dll exports (`proxy.def`, was `version.def`) and forwards to
    `System32\<own file name>`, so the same DLL works as `version.dll` or `winmm.dll`. Out-of-game test: as winmm,
    `timeGetDevCaps`/`timeGetTime` work; as version, `GetFileVersionInfoSizeW` works and a winmm export returns
    `ERROR_PROC_NOT_FOUND`. `deploy-gyroue.sh` takes `PROXY=winmm.dll`, finds renamed exes (largest exe in the dir)
    and removes our proxy under the other name. Deployed to Gears as `winmm.dll`; **not yet run in game**.
- 2026-10-09 **GyroUE 0.4.8:** the exe-name/location gate is gone (it only ever filtered exes in the DLL's own
  folder, and it silently skipped renamed exes). Instead a mutex `Local\GyroUE-<hash of the lowercased install
  folder>` makes the first process that loads the proxy the only one running GyroUE; later processes are plain
  forwarders (no pad, hooks or log). Out-of-game test with two processes: the second sees the mutex and doesn't
  attach, forwarding still works, the mutex is gone once both exit. Deployed to Gears as `winmm.dll`; not run in game.
- 2026-10-09 Gears 0.4.8 log: GyroUE loads as `winmm.dll` (log written, game thread = DllMain thread), but
  `hook: PeekMessageW not imported by any loaded game module yet` and nothing after it: no gyro frame ever ran, so
  GyroLib never initialized (no `gyrolib.ini` created). UE4SS says the build is non-modular (MainExe only), so the
  exe itself doesn't import `USER32.dll!PeekMessageW` by that name. Hypotheses: API-set import names
  (`ext-ms-win-ntuser-message-*`, GDK/OneCore linking), a stripped name table, or SecureEngine (`SecureEngineSDK64.dll`,
  Themida/WinLicense) resolving imports itself.
  - **GyroUE 0.4.9:** at startup the log dumps the exe's import table (each DLL: slots/by name/by ordinal/no name
    table) and every message-pump import (Peek/GetMessage, MsgWait..., DispatchMessage) with where its slot points,
    also matched by user32's real address. Hooks now accept API-set DLL names (`api-ms-*`/`ext-ms-*`) and, without a
    name table, match slots by the resolved address. Tested on a small exe (PeekMessageW found and hooked).
    Deployed to Gears; waiting for a run.
  - **0.4.9 Gears run: the exe's PE import directory is a protector stub.** Import directory at RVA 0xfbe4795
    (unaligned, at the end of the 0x12ee5000 image), 52 DLLs with **1 slot each and no name table** (USER32, dxgi,
    WINMM, VERSION, ...). That only forces the DLLs to load; the real imports are resolved later by the protector
    (SecureEngine/Themida-style, `SecureEngineSDK64.dll` ships with the game) into its own table, after our DllMain.
    So no IAT hook can work in this exe: not PeekMessageW, not the LoadLibrary hooks, not the DXGI factory hooks
    (the F10 panel won't attach either). Note: VERSION.dll **is** in the stub import list, so `version.dll` would
    also have loaded (the original no-log symptom was only the name gate).
    Options: (1) inline hook of user32!PeekMessageW (and dxgi exports) as a fallback when the IAT hook finds nothing
    (MinHook, BSD-2); UE4SS and ReShade already patch code in this process. (2) scan the protector's table after
    unpacking and patch pointers there (may not exist if calls go through stubs; writes into protected memory).
    (3) a thread timer on the game thread (no patching, but WM_TIMER is limited to ~10-16 ms, so not every frame).
  - **GyroUE 0.5.0 (user chose option 1):** MinHook **1.3.4** (commit c3fcafdc, BSD-2) cloned into
    `third_party/minhook-1.3.4` (git-ignored like all of third_party; re-fetch with
    `git clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook third_party/minhook-1.3.4`), built as
    a static lib in `src/GyroUE/CMakeLists.txt`. `hooks::init` detects the stub (imports present, no name tables) and
    switches to inline mode: every `hooks::add` with a named DLL hooks that DLL's export with MinHook (`*original` =
    trampoline); LoadLibrary hooks (dll null) are skipped (only needed by modular builds). `GYROUE_INLINE=1` forces
    the mode. Log: `hooks: ... import-table hooks|inline hooks on exports`, `hooks: inline X!F at module+off: MH_OK`.
    Out-of-game test (small exe, forced): inline PeekMessageW MH_OK and the gyro frame ran. Releases ship
    `licenses/MinHook-LICENSE.txt`. Deployed to Gears as winmm.dll; **not yet run in game**. Open risks: the protector
    may detect/undo patches in user32/dxgi (ReShade's dxgi.dll here), and the game thread is assumed to be the DllMain
    thread.
  - **0.5.0 Gears run (02:08, ~3.5 min, per UE4SS/ReShade logs): no GyroUE.log at all** (the file stayed the 0.4.9 one
    from 01:56), mod and panel inactive. `winmm.dll` was 0.5.0. GyroUE opens its log right after the instance mutex,
    so either the DLL never ran DllMain or the mutex already existed. Main hypothesis: the 01:56 instance hadn't fully
    exited and still held `Local\GyroUE-<hash>`. Nothing in the Application event log; no process had WinGDK modules
    loaded afterwards, so it can't be checked post hoc.
  - **GyroUE 0.5.1:** every process that loads the proxy appends one line to `GyroUE\GyroUE-boot.log` (time, version,
    pid, thread, exe, outcome: runs GyroUE / another process already runs it / mutex failed) before the mutex decides.
    Restarts past 64 KB. Tested with two processes. Deployed to Gears.
  - The 02:18 run had no boot log either; the user had deleted `GyroUE.log` by hand before launching. Launched again
    at 02:25 (0.5.1): `runs GyroUE`. So the two silent runs (02:08, 02:18) remain unexplained; watch the boot log.
  - **0.5.1 Gears run (02:25): inline hooks work.** `imports: ... no name tables`, `inline hooks on exports`,
    MH_OK on `USER32!PeekMessageW` and ReShade's `dxgi.dll!CreateDXGIFactory/1/2` (+0x17be70/+0x17be80/+0x17c320).
    The gyro frame ran on the game thread (9412 = "GameThread" in the dump): GyroLib ABI 1, `gyrolib.ini` created
    (menu key F10, gamepad shortcut 1), SDL reader started, `motion=1 samples=61`. FNamePool found at
    `GoWEDay.exe+0xEBA1CD0`. `view=none pc=` at the front end (UE4SS sees `FairlightPlayerController_FrontEnd_BP_C`).
    First host frame took 2.43 s (GyroLib/SDL init).
  - **Crash 02:26:36:** AV in `C:\WINDOWS\system32\dxgi.dll+0x2c459` (read [rbx+0x128], list walk), game thread,
    dump `%LOCALAPPDATA%\CrashDumps\GoWEDay.exe.1052.dmp`. Stack: GoWEDay -> `sl_interposer` (NVIDIA Streamline) ->
    Streamline plugin (`slGetPluginFunction`) -> **our inline `CreateDXGIFactory*` hook** -> `RTSSHooks64` (RivaTuner)
    -> system dxgi. Same DLL stack ran fine with 0.4.x import hooks (which did nothing here), so the inline hooks on
    the factory exports (four layers chained on them: Streamline, ReShade, RTSS, us) are the cause.
  - **GyroUE 0.5.2:** in inline mode the overlay no longer hooks dxgi exports. A thread started from DllMain (runs
    after the loader lock) loads `System32\dxgi.dll`, creates its own factory and patches that **real factory's
    vtable** (CreateSwapChain 10, CreateSwapChainForHwnd 15): the wrappers all call it with the real queue, and the
    real swapchain's Present is hooked as before. The build now emits `build/GyroUE/Release/version.pdb` (`/Zi`,
    `/DEBUG /OPT:REF /OPT:ICF`; not shipped) for dumps. Test: D3D12 sample app with `GYROUE_INLINE=1` -> factory
    vtable hooked, swapchain captured, `DX12 panel ready`. Deployed to Gears.
  - **0.5.2 Gears run (02:32): no crash; F10 panel works** (user-verified: opens/closes in game). Log: system factory
    vtable hooked, `captured D3D12 swapchain`, ResizeBuffers(6, 3232x1888, format 24), `DX12 panel ready (6 buffers)`.
    Auto-calibration ran (bias (0.13 -0.18 -0.69)). Gyro not applied: `view=none pc= pawn=` for 3 minutes, because
    **GUObjectArray was never found** (only `ue: FNamePool.Blocks at GoWEDay.exe+0xEBA1CD0`), so `ue::init()` fails
    every second. UE4SS also fails several AOBs here (GMalloc, FName::ToString, FName ctor) but its object cache works.
  - **GyroUE 0.5.3:** the GUObjectArray scan also tries strides 0x28/0x30, and after ~10 failed attempts also scans
    writable+executable sections (protectors may merge .data into RWX sections), every 10 s. The first time it logs
    the exe's section table and up to 8 header-valid candidates with their first items (object, InternalIndex,
    vtable module). Deployed to Gears; not yet run.
  - **0.5.3 Gears run (02:39):** exe sections: unnamed R-X (rva 0x1000, 0xAD9EA3B), R--, **RW- data at rva 0x0E497000
    (0x956D08)** (FNamePool at +0xEBA1CD0 is in it), small RW/R sections, .debug, .edata, .idata RW, .tls, .rsrc,
    **.themida RWX (rva 0x0FBF7000, 0x206C000)**, **.boot R-X (rva 0x11C63000, 0x1280800)**, .reloc. Themida-protected.
    Not a single object-array header candidate anywhere, RWX included.
  - Explanation found in `ue4ss/UE4SS_Signatures/GUObjectArray.lua` (a custom signature someone wrote for the Steam
    build; anchored in UObjectBase::AddObject): "The global holds an XOR-encoded FUObjectArray pointer, not an inline
    array." Pattern there: `0F BA EF 19 83 E0 FB 89 46 ?? 48 8B 0D <rel32> 48 BB <imm64 key> 48 33 CB 75 ?? B9 B8 00 00
    00` (mov rcx,[global]; mov rbx,key; xor rcx,rbx; jnz; mov ecx,0xB8 = sizeof(FUObjectArray) for the lazy
    allocation). UE4SS 3.0.1 failed with it on the Xbox build too ("Was unable to find AOB for 'GUObjectArray'"), and
    also fails GMalloc, FName::ToString/ctor, StaticConstructObject, FUObjectHashTables, GNatives, ConsoleManager,
    GameEngineTick AOBs.
  - **GyroUE 0.5.4:** `ue.cpp` scans the executable sections once (VirtualQuery, committed readable pages, SEH-guarded)
    for the generic idiom `REX.W 8B /r [rip+disp32]` -> `REX.W B8+r imm64` -> `REX.W 33/31` (any registers), keeps up
    to 8192 unique (global, key) pairs, and on each retry decodes `*global ^ key` and accepts it only if a valid
    ObjObjects header + 16 live items (InternalIndex, vtable in a module) sits within the first 0xB8 bytes. Logged as
    `ue: N XOR-encoded pointer loads ...` and `ue: FUObjectArray <p> decoded from GoWEDay.exe+X ^ key (code at ...);
    ObjObjects at +0xNN`. Header check refactored into `object_array_stride()`. Deployed to Gears; not yet run.
  - **0.5.4 Gears run (02:48): `ue: 0 XOR-encoded pointer loads in the game code (109 ms)`.** Either that code is
    virtualized/compiled differently in the Xbox build, or the scan didn't cover the code (no stats logged then).
  - **GyroUE 0.5.5:** the code scan logs MB scanned, regions, unreadable regions and faults. New last resort, independent
    of code: a background thread scans every committed `MEM_PRIVATE` `PAGE_READWRITE` region (raw prefilter on
    Max/Num/MaxChunks/NumChunks, then the full `object_array_stride()` check), at most every 30 s and 10 times; the
    game thread picks the result up in `ue::init()`. `module_offset()` prints `heap 0x...` for non-module addresses.
    Test: a fake heap FUObjectArray (5000 objects, stride 0x18) is found at the exact address. Deployed to Gears.
- 2026-10-09 **GyroUE 0.5.6:** the DLL carries a Windows version resource (`src/GyroUE/version.rc.in`, configured by
  CMake from project(), so the version still lives only in CMakeLists.txt): FileDescription "GyroUE - gyro aiming for
  Unreal Engine games", ProductName GyroUE, File/ProductVersion, InternalName, OriginalFilename version.dll,
  LegalCopyright "MIT License" (LICENSE.txt still has the [year] [fullname] placeholders), Comments (removed in 0.5.7). Verified with
  Get-Item VersionInfo. Not deployed to Gears (the user is testing 0.5.5 renamed to version.dll there).
- 2026-10-09 **GyroUE 0.5.7:** Comments field removed from the version resource (user request). Not deployed to Gears.
- 2026-10-09 **Gears of War: E-Day WORKS (user-verified, 0.5.5 renamed to `version.dll`, run 19:22-19:28): gyro turns
  the camera and the F10 panel works.** Pad/connection/Steam Input state not reported.
  - 0.5.5 as `winmm.dll` at 19:06 didn't load at all (no boot line), like the 02:08/02:18 runs. Running processes then:
    RTSS + RTSSHooksLoader64, MSI Afterburner, NVIDIA Overlay, Steam. `RTSSHooks64.dll` statically imports WINMM.dll
    (ReShade's dxgi.dll delay-imports it). Hypothesis (unconfirmed): an injected DLL sometimes loads System32
    winmm.dll first, so ours is never mapped. `version.dll` loaded first try. **Use `version.dll` for Gears.**
  - Code scan stats: 224.5 MB in 3 regions, 0 unreadable, 0 faults, **0 XOR-encoded loads**: the Steam-build idiom is
    not native code in the Xbox build (virtualized or different codegen).
  - **Heap scan found GUObjectArray:** `heap scan found ObjObjects (1.37 GB in 1959 regions, 344 ms)`, ObjObjects at heap
    0x7FF4FC720010, stride 0x18, 75910 objects, picked up ~30 s after start (scan starts at the 3rd failed init).
    Layout: SuperStruct 0x40, ChildProperties 0x50, FField.Next 0x18, FField.Name 0x20, ElementSize 0x34,
    Offset_Internal 0x44, sizeof(FProperty) 0x70, ControlRotation StructProperty. Engine `GameEngine_2147482572`.
  - Front end: pc `FairlightPlayerController_FrontEnd_BP_C`, view target `BP_UIActor_FrontendCam_C`, no pawn (view=none,
    calibration runs there). Campaign: pc `BP_PlayerController_Campaign_C`, pawn/target
    `BP_Pawn_Marcus_Casual_Holster_LipScar_C`, **generic ControlRotation view works, no profile needed**
    (`applied yaw=109.5 pitch=-17.4`). Pause detected (paused=1 during the intro).
  - Perf (game thread, PeekMessageW frame): idle avg 90 us p99 274 us; active avg 109 us p99 373 us; Present hook avg
    25 us p99 59 us.
  - Open: why winmm.dll sometimes doesn't load (repeat version.dll launches with RTSS open to confirm it's reliable);
    the 30 s delay before the gyro is available (heap scan could start earlier).
  - 0.5.7 deployed to Gears as `version.dll` (19:43; no winmm.dll left in WinGDK).
- 2026-10-09 **0.5.7 deployed (Steam) to Cronos (was 0.4.6), Pacific Drive (was 0.3.4), Life is
  Strange Remastered and Out of Sight (both new, not yet run).** Saves backed up first:
  `um backup` lis-remastered-saves (%LOCALAPPDATA%\LiS\Saved\SaveGames) and outofsight-saves
  (%LOCALAPPDATA%\OutOfSight\Saved). Profiles copied for Cronos and Pacific Drive; users' gyrolib.ini kept.
  - LiS Remastered: `LIS\Binaries\Win64\LiS-Win64-Shipping.exe`, UE4SS present as dwmapi.dll (left as is), SDL2.dll.
  - Out of Sight: `OutOfSight\Binaries\Win64` holds two full game builds, `OutOfSight-Win64-Shipping.exe` (153 MB)
    and `OutOfSight.exe` (309 MB, imports WINMM/dxgi/..., delay-loads steam_api64/EOSSDK); one runs at a time, so the
    instance mutex is not an issue; the boot log will show which one Steam launches. ReShade 6.7.3 (dxgi.dll) +
    RenoDX UE add-on, a d3d12.dll and the D3D12 Agility folder present. No anti-cheat files in any of the four.

- 2026-10-09 **GyroUE 0.5.8: probe output in its own file.** With `Probe=1`, results go to
  `GyroUE\GyroUE-probe.log` (recreated each session, header with the F7/F8 steps and how to read rot/raw lines);
  `GyroUE.log` only gets `probe: snapshot #N taken ... -> GyroUE-probe.log` / `probe: diff of snapshot #N (...)`.
  `log.cpp`: `probe_log_open`/`probe_log` (falls back to GyroUE.log). Tested: file + header created with a test
  profile; F7/F8 output path compiled only (needs a UE game). README probe section updated. Not deployed.

- 2026-10-09 **GyroUE 0.6.0: probe for non-experts (steps 1-3 of the plan).**
  1. **Follow check (always on):** each frame compares the gyro rotation written (|yaw|+|pitch|) with how much the
     camera's POV moved (`PlayerCameraManager.CameraCachePrivate.POV.Rotation`, new `ue::camera_pov`, offsets cached
     per class; `CameraCache` fallback). 2 s windows with >= 10 deg of gyro: ratio < 0.2 = ignored, > 0.5 = followed.
     Only when view target == pawn (or a profile view with ViewTargetClass), not paused, no cursor. 3 ignored windows
     and none followed -> one `camera: the gyro turned X ... the game ignores it. ... put [GyroUE] Probe=1 in ...`;
     3 followed -> one `camera: confirmed, the camera follows the gyro (...)`.
  2. **Automatic capture (Probe=1):** snapshot once the POV has been still 0.5 s (min 1.5 s after the last capture);
     when the POV has turned > 30 deg yaw and is still again for 0.3 s, a scored diff, unless the gyro wrote more
     than 20% of the turn ("discarded"). Re-snapshot if the pawn changes or after 2 min. Up to 8 per session.
     F7/F8 still work and switch the automatic capture off for the session.
  3. **Ranking + suggested profile:** each diff with |yaw| >= 10 deg is scored. Candidates = reflected rotators and raw
     float/double (pitch, yaw) pairs that now equal the POV pitch/yaw (0.6 deg) and whose yaw changed. Ignored with a
     reason: `Camera` root (output), RelativeRotation / raw offsets inside the SceneComponent part (transforms),
     PC.ControlRotation (the default view), nested struct properties. Usable ones that hit every scored diff (all but
     one from 4 on) are grouped per object (raw offsets of one size together, like Cronos' two copies) and ranked
     Pawn > ViewTarget > PC, more rotators first, named before raw. From 2 scored diffs on, written to
     `GyroUE\profiles\<exe>.suggested.ini` (top pick as [View.Default] + commented alternatives + ignored ones) and
     to GyroUE-probe.log; GyroUE.log gets `probe: suggested profile: <pick> -> <path>`.
  Read-only (no memory writes). Not run in a UE game yet. README probe section rewritten around the automatic mode.
  **Validation set up in Cronos:** 0.6.0 deployed, verified profile kept as `Cronos-Win64-Shipping.ini.verified`,
  `Cronos-Win64-Shipping.ini` = `[GyroUE] Probe=1` only. Expected: the follow check warns (ControlRotation ignored)
  and the suggestion is `Object=Pawn.View Offsets=0x2A0,0x420 RotatorSize=24`. Restore afterwards by redeploying.

- 2026-10-09 **GyroUE 0.6.1: on-screen notices + follow check stops after its verdict.**
  - `notify.cpp`: a GDI box at the top centre of the game's largest visible window (layered 225 alpha, click-through,
    WS_EX_NOACTIVATE/TOOLWINDOW/TOPMOST, own thread + message loop created on first use, 250 ms timer to follow the
    window, hidden when the game isn't foreground, font = client height/40 clamped 16-36 px Segoe UI). Renderer-agnostic
    (DX11/DX12); not visible in exclusive fullscreen. `Notices=0` under [GyroUE] in the profile turns it off.
    Verified visually with a test window (DPI-unaware test exe, so scaled).
  - Messages: follow-check warning (3 variants: probe on / no profile -> how to create the Probe=1 profile / profile
    outdated), probe ready, turn discarded (gyro moved), capture N done (+ suggested profile written, rename hint),
    no usable candidate yet.
  - Follow check: off after the first verdict (confirmed or warned); the POV is then only read if Probe=1.
  - Cronos re-deployed with 0.6.1, verified profile refreshed as `.ini.verified`, `.ini` = Probe=1 only.

- 2026-10-09 **Crash in Cronos with 0.6.1 at startup: EXCEPTION_STACK_OVERFLOW** (UE crash folder
  %LOCALAPPDATA%\Cronos\Saved\Crashes\UECC-Windows-84ADE0C5..., analysed with cdb + version.pdb): endless recursion
  `hk_create_swapchain_hwnd+0x40 -> d3d12!Ordinal196+0x2399c -> hk_create_swapchain_hwnd -> ...`. The d3d12.dll is
  **SCSKiller 1.2.4 (D3D12 proxy, "shader compilation stutter killer")**, installed in Cronos' Win64 on 2026-10-09
  02:08 (also present in Out of Sight). Not specific to 0.6.x: **bug in patch_vtable since the first panel versions.**
  We hooked factory vtable slot 15, SCSKiller hooked the same slot after us (saving ours as its original), and on the
  next factory patch_vtable saw a foreign function in the slot and patched it again, saving SCSKiller's function as
  our original: we call it, it calls us.
  - **GyroUE 0.6.2:** `patch_vtable` patches each slot once and keeps the original per slot address
    (`g_slot_originals`); every hook calls `original_of(object, index, fallback)`, the original saved for the slot its
    object's vtable points to (CreateSwapChain 10, CreateSwapChainForHwnd 15, Present 8, ResizeBuffers 13, Present1 22,
    ResizeBuffers1 39). Test `chain.cpp` (a second hooker overwrites slot 15 after us, then a second factory and the
    swapchain): 0.6.1 loops (depth 51), 0.6.2 calls it once, swapchain captured, panel ready.
  - Also in that log: `ue: ChildProperties/FField layout not found` at 7810 objects, 4 s after start (retried every
    5 s; earlier Cronos runs found it later). Watch on the next run.
  - 0.6.2 deployed to Cronos (probe test profile kept, verified one as .ini.verified) and Out of Sight (SCSKiller too).
    Gears, Pacific Drive and LiS still run 0.5.7 (same latent bug if a chaining hooker shows up).

- 2026-10-09 **GyroUE 0.6.3:** probe-ready notice reworded (user text: "GyroUE probe ready to find cameras. In game, put the controller down, turn the camera with the RIGHT STICK (more than 30 deg), then stop. Repeat 3-4 times.") and shown 30 s instead of 15. Deployed to Cronos (probe test profile kept).

- 2026-10-09 **GyroUE 0.6.4: global settings file + the probe writes the game's profile.**
  - `GyroUE\GyroUE.ini` (all games in that folder): created with the defaults on the first run if missing, never
    overwritten, not shipped in releases (like gyrolib.ini). `[GyroUE] Probe=0`, `Notices=1`, commented. `setting()`
    reads the game's profile `[GyroUE]` first (override), then GyroUE.ini, then the default. Log:
    `settings: Probe=N Notices=N (...)`. Tested: created with defaults; Probe=1 there enables the probe; a profile with
    Probe=0 overrides it.
  - Probe output: if the game had no profile when it started and the result has a usable view, the probe writes
    `profiles\<exe>.ini` itself (header "created by the camera probe ... NOT VERIFIED. Restart ... then set Probe=0 in
    GyroUE\GyroUE.ini") and keeps updating that file during the session; otherwise `<exe>.suggested.ini` (header "the
    game already has a profile ..."). A result with no usable view never becomes the profile.
  - Notices/log point to GyroUE.ini ("set Probe=1 in GyroUE\GyroUE.ini, restart ..."); capture notice says "Game
    profile created ... Restart the game to use it, then set Probe=0" or "already has a profile ... .suggested.ini".
  - README (install layout, probe steps 2-3), CLAUDE.md (releases ship no GyroUE.ini) updated.
  - **Cronos test set up the new way:** 0.6.4 deployed, `Cronos-Win64-Shipping.ini` removed (verified copy kept as
    `.ini.verified`), `GyroUE\GyroUE.ini` with Probe=1. Expected: warning, captures, then the probe creates
    `profiles\Cronos-Win64-Shipping.ini` = Pawn.View 0x2A0,0x420.

- 2026-10-09 **Life is Strange Remastered probe test (0.6.4, DX11, GyroUE.ini Probe=1, no profile).** UE4 build with a
  non-stock FField layout: `ue: layout SuperStruct=0x40 ChildProperties=0x48 FField.Next=0x28 FField.Name=0x18
  FProperty.ElementSize=0x34 Offset_Internal=0x44 sizeof(FProperty)=0x70 ControlRotation type=` (empty). Engine found
  (`GameEngine_...`), PC `LiSPlayerController_BP_C`, pawn `MaxCharacter_BP_C`, camera `LiSPlayerCameraManager`, view
  target mostly `CameraActor_10xx` (scripted cameras), sometimes the pawn. GUObjectArray at exe+0x35A3908, FNamePool
  exe+0x358C390.
  - **Bug:** `field_type()` read the FFieldClass at a hard-coded +0x08, wrong in this build, so every property type was
    "" and every probe snapshot had **0 values** (145-161 objects). The automatic capture still measured POV turns
    (`camera_pov` only needs names/offsets: 41-174 deg yaw, 0-4 deg pitch) but each diff hit the empty-leaves branch
    and printed the misleading "no snapshot (press F7 first)". 5 turns were discarded because the gyro moved too.
  - Gyro not moving the camera: ControlRotation is written (applied yaw -117) but the camera doesn't follow; no
    follow-check verdict because most of the time the view target is a CameraActor (check skipped by design).
  - **GyroUE 0.6.5:** `detect_layout` finds FField::ClassPrivate (0x08-0x40, the pointer whose FName is
    "StructProperty" on ControlRotation and "ObjectProperty" on Pawn); logged as `FField.Class=0x..` in the layout
    line; falls back to 0x08 with a warning. Empty-snapshot message now says the snapshot holds no values (layout
    problem) instead of "press F7 first". Deployed to LiS (GyroUE.ini Probe=1 kept). Not yet run.

- 2026-10-09 **LiS run with 0.6.5:** `ue: FField class offset not found` -> still 0 values. Reason: **LiS Remastered is
  UE <= 4.24 (UProperty objects, no FField)**. The "FField" layout detected earlier (Name 0x18, Next 0x28,
  ChildProperties 0x48) is really UObject::NamePrivate / UField::Next / UStruct::Children; the property's type is
  its UClass at +0x10 (UObject ClassPrivate), named by the UClass's own UObject name (+0x18), not FFieldClass::Name
  at +0. All 8 automatic captures (61-174 deg yaw, 0-12 deg pitch) had nothing to compare and showed nothing on
  screen. One "turn discarded" during the opening cutscene: the cutscene turned the camera 30 deg while the
  uncalibrated resting controller drifted a few degrees (> 20 % of 30).
  - **GyroUE 0.6.6:** the type detection tries FFieldClass::Name at +0 first, then a UClass name at +0x18
    (`g_field_class_name`); `field_type()` uses it; layout line says "FField properties, UE 4.25+" or "UProperty
    objects, UE <= 4.24" and `Field.Class=`. Discard only if the gyro moved > 5 deg and > 20 % of the turn; on-screen
    notice only if > 10 deg. A capture that can't be compared now shows "capture N could not be compared".
    Retry message reworded ("property layout not found yet"). Deployed to LiS. Not yet run.

- 2026-10-09 **GyroUE 0.6.7:** GyroUE-boot.log removed (user: no longer useful). It was the 0.5.1 diagnostic for the winmm.dll runs that never loaded; the instance mutex stays, a second process just stays a plain proxy silently. README troubleshooting row removed. Old GyroUE-boot.log files in game folders are left as they are. Deployed to LiS.

- 2026-10-09 **LiS run with 0.6.7: the probe reads LiS now** (`ue: layout (UProperty objects, UE <= 4.24) ...
  Field.Class=0x10 ... ControlRotation type=StructProperty`, ~3300-3780 values per snapshot). Every scored turn
  (8, 61-174 deg yaw) matched the same 9-11 rotations: PC.ControlRotation (excluded: default view), Camera
  ViewTarget/CameraCachePrivate POV, Camera/PC TransformComponent0 transforms, and **raw float rotators
  LiSPlayerCameraManager +0x33CC and +0x33FC** (excluded as "camera output") -> "no usable candidate".
  Discards: during the opening cutscene (30 deg turn, gyro drift 10 deg) and while turning the stick with the pad on
  the table (gyro 223-294 deg: the sum is of absolute degrees, so the pad rocking under the thumb adds up).
  - **GyroUE 0.6.8:** (1) the gyro output is paused while the automatic capture runs (`probe_capturing()`), so the
    gyro-discard path is gone and the pad can stay in the hands; (2) a turn only counts if the right stick was
    deflected (> 0.3) since the snapshot (`gl_get_flick_input`, GyroLib's selected controller); otherwise "ignored:
    the right stick wasn't used (cutscene or scripted camera)" (filter off if GyroLib never reports the stick);
    (3) raw rotators of the Camera root past the engine's PlayerCameraManager part (`engine_part_size`) are
    candidates (rank 1, like PC), the engine part stays "camera output". Probe-ready notice text unchanged (user's
    wording; "put the controller down" is no longer required). README capture steps updated. Deployed to LiS
    (old .suggested.ini removed). Expected: profile Object=Camera Offsets=0x33CC,0x33FC RotatorSize=12 - unverified.

- 2026-10-09 **GyroUE 0.6.9:** "put the controller down" removed (the gyro is paused while capturing and only stick turns count): probe-ready notice now "GyroUE probe ready to find cameras. In game, turn the camera with the RIGHT STICK (more than 30 deg), then stop. Repeat 3-4 times." (30 s); same in the GyroUE-probe.log header and the default GyroUE.ini comment (existing GyroUE.ini files keep their old comment). Deployed to LiS.

- 2026-10-09 **LiS with 0.6.9: the probe wrote the game's profile, and after a restart the camera follows the gyro
  (log).** Probe session (22:49-22:50): 6 turns without the right stick ignored (opening cutscene/scripted cameras),
  then 4 stick turns scored (124-176 deg yaw, one with 36 deg pitch), 11 rotations each. New candidates vs 0.6.7:
  `Camera(LiSPlayerCameraManager)+0x3ACC` and `Pawn.LiSCamera(LiSCameraComponent)+0x1B0` (the latter a transform).
  The probe created `profiles\LiS-Win64-Shipping.ini`: `Object=Camera Offsets=0x33CC,0x33FC,0x3ACC RotatorSize=12`.
  - Next session (22:51, Probe=0, that profile): `camera: confirmed, the camera follows the gyro (the profile's raw
    offsets, view 'Camera')` at 22:53:33. The status lines show **+0x3ACC accumulates exactly the applied gyro**
    (applied yaw -8.4/pitch -7.3 -> +0x3ACC (P -7.26 Y -8.43); applied yaw 37.6 -> Y 37.63) while **+0x33CC/+0x33FC
    are rewritten by the game every frame** (they stay at the POV, e.g. Y -108.1, whatever the gyro adds). So
    +0x3ACC is the working input (an additive look offset the game keeps) and +0x33CC/+0x33FC are outputs; writing
    them is harmless but useless. Awaiting the user's in-game confirmation.
  - Idea: the profile view could read each raw offset back next frame and log the ones the game overwrites
    ("output, not input"), and the probe could drop them from the suggestion the same way.

- 2026-10-09 **LiS: user confirms the gyro works, but it "suddenly stops" while walking and turning the pad sideways.**
  Status lines (every 10 s): with a fixed CameraActor target, +0x3ACC == applied gyro (an additive look offset only
  the gyro touches); following Max (target MaxCharacter_BP_C) +0x3ACC is the full look rotation (e.g. Y -166, 145),
  also driven by the game. `cursor=1 gyro enabled=0` appears at times in gameplay (GyroUE pauses the gyro when the
  mouse cursor is shown). Suspects: (1) LiS interaction cursor/icons near objects -> cursor=1 -> gyro paused;
  (2) auto-recentering behind Max while walking overriding +0x3ACC; (3) less likely, the writes to the output
  offsets +0x33CC/+0x33FC. 10 s samples can't tell which.
  - **GyroUE 0.6.10:** logs every change of the gyro gate with its reason (`gyro on` / `gyro off: mouse cursor shown`
    / `off: game paused` / `off: no gameplay view`, plus the view target class). LiS profile trimmed by hand to
    `Offsets=0x3ACC` (probe's original kept as `LiS-Win64-Shipping.ini.probe.bak`). Deployed to LiS.

- 2026-10-09 **LiS with the profile trimmed to +0x3ACC: the gyro didn't move the camera at all (user).** The 0.6.9
  analysis was wrong: +0x33CC/+0x33FC staying equal to the POV did NOT prove the game overwrites them. GyroUE writes
  before the game ticks; the game reads our value, integrates it and writes the result back, so they always equal the
  camera *including* the gyro. They are the input; +0x3ACC only accumulates the gyro (in follow-Max mode it is a
  game-driven absolute rotation). Lesson: a read-back after one frame can't tell "integrated and written back" from
  "overwritten"; the "drop outputs by read-back" idea from the previous entry is invalid as stated.
  - The log said `camera: confirmed` anyway: false positive, the follow check counted camera motion from the stick.
  - **GyroUE 0.6.11:** follow-check windows in which the right stick moved are not judged.
  - LiS profile now `Offsets=0x33CC,0x33FC` (A/B test: works + no cut while walking -> +0x3ACC was interfering;
    works + yaw still cut while walking -> the game's auto-recentering; doesn't work -> restore the probe original
    `.probe.bak` = 0x33CC,0x33FC,0x3ACC, which worked). 0.6.11 deployed to LiS.

- 2026-10-09 **LiS A/B with Offsets=0x33CC,0x33FC (0.6.11): only pitch moves the camera** (user). The follow check
  caught it by itself: `the gyro turned the profile's raw offsets by 1274 degrees but the camera moved only 90`.
  With +0x3ACC alone nothing moved; with all three everything worked except yaw while walking. So the LiS camera
  splits the axes: **pitch from +0x33CC/+0x33FC, yaw (following Max) from +0x3ACC**, and while Max walks the game
  turns +0x3ACC toward her walking direction (auto-recentering), overriding the gyro yaw.
  - Profile restored to the probe's original (0x33CC,0x33FC,0x3ACC) with a `[GyroUE] Probe=1` override for a manual
    F7 (standing) / walk without touching the camera / F8 capture, to find what drives the recentering (timers,
    flags, speeds in the `num` lines), like Pacific Drive's car (`OnGyro=`).

- 2026-10-09 **LiS manual probe (F7 standing, walk without touching the camera, F8):** the walking camera is the camera
  mode object `PC.TP_Walk_Manual_C_<n>` (TP_Walk_Manual_C, outer = the PC). In both walking diffs (82 and 51 deg of
  camera yaw with no right stick) its `SmoothedAutoManualRatio` fell toward 0 (0.0719 -> 0.0025): the blend between the
  automatic follow camera (0) and manual control (1). The gyro isn't manual input for the game, so walking blends to
  auto and the follow camera wins the yaw. Other changes while walking: PC `LastTimeLeftStickChanged`,
  `TimeBeforeAdditiveWalk` (counts down), `fCurrentTimeoutNoVelocityForceIdle`; TP_Walk_Manual `fTimeSinceActivation`,
  `DampedPitch`, `SmoothedLocationZ/TargetZ`, `CurrentGroundSpeedSmoothed`.
  - **GyroUE 0.6.12:** path step `@Class` = first subobject of that class (`ue::find_subobject_of_class`, cached like
    name lookups), for instance names that change every session; `OnGyro=` entries may carry an object path
    (`Root.Sub.Name:value`), resolved like `Object=` (`resolve_path`); a failed write is logged once. README profile
    reference updated. Pacific Drive's OnGyro (no path) unchanged.
  - LiS profile: `OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1` added, `[GyroUE] Probe=1` removed. Deployed.
    Not yet verified in game.

- 2026-10-09 **LiS verified (user): gyro yaw and pitch, fixed cameras and following Max, also while walking** (0.6.12,
  OnGyro on PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1, no OnGyro write failures).
- 2026-10-09 **Cronos probe validation (0.6.4, run 21:56, found while cleaning up):** with only Probe=1 and no profile,
  the probe wrote `profiles\Cronos-Win64-Shipping.ini` itself after 5 scored captures: `Object=Pawn.View
  Offsets=0x2A0,0x420 RotatorSize=24`, exactly the manually verified profile.
- 2026-10-09 **GyroUE 0.6.13 (cleanup after LiS):**
  - Conflict check against the other games' logs: every UE 4.25+/UE 5 game read `ControlRotation type=StructProperty`
    with the old fixed FFieldClass at +0x08 (Cronos/Out of Sight/Gears: ChildProperties 0x50, Next 0x18, Name 0x20;
    Pacific Drive/Returnal: Next 0x20, Name 0x28). The new detection tries FFieldClass::Name at +0x08 first, so they get
    the same result; the UProperty pass only runs if that fails. Other LiS-driven changes only act with Probe=1
    (stick filter, gyro pause, camera-manager candidates ranked below Pawn), on new profile syntax (`@Class`, OnGyro
    paths; Pacific Drive's OnGyro unchanged) or narrow the follow check (stick windows skipped).
  - No test code in the repo (test programs live in the scratchpad). `ue: property layout not found yet` logs once.
    The `gyro on/off` gate lines stay (only on change; general diagnostic).
  - `profiles/LiS-Win64-Shipping.ini` added to the repo (clean, verified), shipped in releases. README: LiS row in the
    compatibility table; engine requirement now "UE 4 and UE 5, both property systems".
  - Deployed to LiS (repo profile) and Cronos (verified profile restored, GyroUE.ini Probe=0). Left in game folders:
    LiS `LiS-Win64-Shipping.ini.probe.bak` / `.suggested.ini`, Cronos `Cronos-Win64-Shipping.ini.verified`, old
    GyroUE-boot.log files.

- 2026-10-10 **Switch Pro Controller judder (user, Cronos).** With GyroLib/SDL (`input.steam_mouse=1` Block), the camera
  isn't smooth; Steam Input gyro-to-mouse (Pass through) is. The DualSense shows almost no difference.
  - gyroprobe with the Switch Pro: ~200 accepted samples/s (695 in 3.5 s), 0 rejected/dropped. SDL delivers all 3 IMU
    samples per report, but they arrive together about every 15 ms (~66 reports/s). The DualSense gives ~460/s.
  - Cause: at frame rates above ~66 fps, GyroLib outputs rotation only on frames where a burst arrived and 0 on the
    others. `gyro.smoothing_ms=20` + `smoothing_threshold_dps=100` barely helped (user): GyroLib's filter only steps when
    samples arrive, so the empty frames stay empty. Steam's gyro-to-mouse emits at a steady rate.
  - **GyroUE 0.6.14: pacing.** GyroUE measures the interval between non-zero GyroLib outputs (EMA, gaps over 100 ms
    ignored). If it is longer than 1.25 frames and at most `PacingMaxMs` (50 since 0.6.15), each burst is released at a constant rate
    over that interval (rate = pending / interval, never overshooting). Otherwise the output is applied directly, so no
    latency is added on the DualSense. Pending motion is dropped on view change, camera gate or probe capture.
    `GyroUE.ini`: `Pacing=1` (default), `PacingMaxMs=50`. The status line logs the burst interval, the frame interval
    and the pacing state. Pending user test with the Switch Pro (USB and BT) and the DualSense.
  - Possible upstream issue for GyroLib: optional output pacing/interpolation for burst-reporting controllers.
  - **Luto (UE5, Epic, `winmm.dll` proxy) test of 0.6.14, Switch Pro, Block:** pacing engaged the whole time
    (`motion bursts every 20-26 ms, frames every ~12 ms -> pacing on`). The sample rate was only **~67/s** this session,
    against ~200/s in the 01:31 Luto session and in gyroprobe, so the source path differed (likely Steam-filtered isolated
    reader or BT vs USB; to confirm). The 20-26 ms burst interval was close to the 30 ms limit, so **0.6.15 raises the
    `PacingMaxMs` default to 50**. The user set `gyro.smoothing_ms=25` mid-session, which adds lag on top of pacing.
  - User: pacing "improved a bit, but nothing like Steam Input mouse" (pass-through, or Steam's own mouse output).
  - **Luto, Steam Input OFF (0.6.15, Block, smoothing 0):** ~200 samples/s again (vs ~67/s with Steam Input on, so the
    Steam-filtered isolated reader loses about 2/3 of the samples). But the burst cadence seen per frame is unchanged:
    a non-zero GyroLib output every 21-27 ms (~40-45/s, about 5 samples each) at ~83 fps. So the samples reach GyroUE
    in groups more widely spaced than the controller's ~15 ms reports. Not yet known whether that is the controller/BT,
    SDL/HIDAPI or GyroLib's reader.
  - **GyroUE 0.6.16: motion trace.** `MotionTrace=1` in GyroUE.ini records 30 s from the first gameplay frame to
    `GyroUE\motion-trace.csv`: every accepted sample with sensor/arrival ns (GyroLib `gl_set_sample_observer`), and
    every frame with `new_samples`, raw GyroLib yaw/pitch and the paced yaw/pitch. `tools/motion_trace_report.py`
    summarises the sensor interval, arrival interval, samples per arrival, frame interval, frames with no rotation,
    and the speed CV while turning (raw vs paced). Checked against a synthetic trace.
  - **Trace, Luto, Switch Pro over Bluetooth, Steam Input off, 0.6.16 (30 s):**
    - 198 samples/s. The sensor interval is regular: 5.24 ms mean (p10-p90 4.8-6.1). SDL delivers all 3 IMU samples
      per report. Sensor timestamps drift against the host clock (about 0.7 s over 30 s), so they can't be mapped
      directly to host time.
    - Arrivals are bunched in the transport (BT / Windows HID): about 45 arrival bursts/s instead of ~66 reports/s,
      gaps median 25.4 ms (p10 2.9, p90 38, max 68). New samples per update: 0 (73%), 3, 6, 9 or 12, so 1-4 reports
      come at once.
    - GyroUE's hook ran ~143 times/s at ~85 fps (median interval 1.9 ms): Luto pumps messages twice per frame.
    - Raw GyroLib yaw: 73% of updates had no rotation, speed CV 1.99. 0.6.16 pacing: 26%, CV 0.78. That matches the
      user's "a bit better, nowhere near Steam".
  - **GyroUE 0.6.17: jitter-buffer pacer** (replaces the per-burst rate):
    - Each update's GyroLib rotation is split over its samples, weighted by gyro speed (the sample observer now always
      runs). The samples are queued and played back at the measured sample rate (1 s windows, EMA), with speed nudged
      ×0.5–2 to keep `target = min(gap_mean + 0.5·σ_gap, PacingMaxMs)` seconds queued.
    - Pacing is active only when `gap_mean > 1.5 × update interval`, so pads that report every update (DualSense)
      aren't delayed. The queue is capped at 1.5 × PacingMaxMs after stalls and flushed when pacing turns off or the
      view/gate changes.
    - Replaying the recorded trace (`tools/sim_pacer.py`), speed CV over 12 ms slots: raw 1.56, 0.6.16 0.75, 0.6.17 0.44
      (empty slots 51% / 9% / 2%). Queue delay median 18 ms, p90 32 ms. A margin of 1.0-1.5σ didn't improve CV
      (plateau ~0.43), only added delay (24-30 ms median).
    - Status line: `motion N samples/s, bursts every X ms, updates every Y ms -> pacing on, Z ms buffer`.
    - Release zip built.
  - **0.6.17 user test (Luto, Switch Pro BT, Steam Input off):** "clearly smoother, but not at the level of Steam
    Input gyro-to-mouse". Status: ~200 samples/s, bursts every 20-25 ms, updates every 6.5 ms, pacing on with a 26-32 ms
    buffer (larger than the 18 ms simulated, because the in-game gap mean was higher). The new trace had too little
    motion to score (11° total in 30 s); the applied output had 0-1% empty slots.
  - **Decision:** stop investing in GyroUE-side pacing. The jitter belongs in acquisition/timing, which GyroLib owns
    (sensor timestamps, its device-clock estimator, per-sample processing). Keep the 0.6.17 pacer as a contained,
    switchable stop-gap (`Pacing=0`). Report to GyroLib: (1) BT burst delivery makes output steppy; it should play back in
    sensor time with a small adaptive delay; (2) the Steam-filtered isolated reader delivers ~67 instead of ~200
    samples/s for the Switch Pro. Steam Input's implementation is closed source and can't be compared. The closest open
    references are SDL's HIDAPI Switch driver and JoyShockMapper/GamepadMotionHelpers.

- 2026-10-10 **GyroUE 0.6.18: ships as `winmm.dll` by default, `version.dll` is the fallback name** (user: winmm has
  wider support to start with). Build output, version resource OriginalFilename and `proxy.def` LIBRARY are now winmm;
  the DLL still exports both sets and forwards by its own file name, so nothing changes at runtime. `deploy-gyroue.sh`
  defaults to `winmm.dll` (`PROXY=version.dll` for the fallback; Gears of War: E-Day needs it, see 2026-10-09: with
  RTSS running the system winmm.dll is loaded first), the release zip holds `winmm.dll`, `bench.sh` handles either name.
  Docs (README, TROUBLESHOOTING, ADVANCED, AGENTS, CREDITS, BENCHMARKS) updated. Already-installed games keep their
  current name; the installer removes the other name when redeploying. Not yet run in game.

- 2026-10-11 **GyroUE 0.8.0 = main's 0.7.0 merged into `feature/xinput-translation`** (user's decision: minor bump for
  the XInput translation). Conflicts resolved: `host.cpp` default `GyroUE.ini` keeps `Notices` plus the XInput keys
  (the `Pacing` keys are gone with the pacer), CMake builds `deps.cpp` and `xinput.cpp`, docs keep both sides. Builds
  clean against GyroLib 1.3.1.
- 2026-10-11 **Release: GyroUE 0.7.0** (minor bump, user's decision: new features, not only fixes). Contains
  everything after 0.6.18 that was numbered 0.6.19 below: guard-page fix, gyro-camera notice wording,
  `WorldRotation=1` + probe ranking + Severed Steel profile, GyroLib 1.3.1 (GyroUE pacer removed), dependency check
  (`deps.cpp`). Zip `build/release/GyroUE-v0.7.0.zip` (gyrolib.dll = the 1.3.1 SDK's).
- 2026-10-11 **Release numbering:** production is still 0.6.18, so everything after it ships as **0.6.19**: the
  guard-page fix below plus the Severed Steel work (status addresses, `WorldRotation=1`, probe ranking, profile). The
  0.6.20 / 0.6.21 numbers were local test builds only (their zips deleted); the demo and Cronos were redeployed as 0.6.19.

- 2026-10-10 **GyroUE 0.6.19: guarded reads no longer swallow stack guard-page hits** (ported from PSButtonsUE, see its
  MODLOG "Luto crash with v0.1.0"). `guarded_copy`, `guarded_store`, `scan_xor_loads` and `scan_heap_region` used
  `__except(EXCEPTION_EXECUTE_HANDLER)`. A stray but plausible pointer landing on another thread's stack guard page raised
  STATUS_GUARD_PAGE_VIOLATION; swallowing it lets Windows clear PAGE_GUARD, so that thread can't grow its stack and the
  game later crashes in ntdll (AV at `ntdll+0x12c083`, Windows 11 26100; PSButtonsUE hit it in Luto, UE 5.5.4, while
  init() retried every 100 ms). New `guard_filter` in `ue.cpp`: on a guard-page violation it re-applies PAGE_GUARD to the
  faulting page (VirtualQuery + VirtualProtect on `ExceptionInformation[1]`) and handles it; access violations and
  in-page errors are handled as before; anything else is no longer swallowed (EXCEPTION_CONTINUE_SEARCH). No GyroUE crash
  was attributed to this yet; it's a latent bug in the shared core. Built with `tools/build-gyroue.sh`, release zip
  packaged. Deployed to Luto (`D:\Epic\LutoF2kGB\Luto\Binaries\Win64`, `winmm.dll`, replacing 0.6.17; alongside
  PSButtonsUE 0.1.0 `dsound.dll` and scskiller `d3d12.dll`; generic profile, gyrolib.ini kept). Not run in game yet.

- 2026-10-11 **Severed Steel (Demo, UE 4.27.2, exe `ThankYouVeryCool-Win64-Shipping.exe`, DX11).**
  - **Crash at startup (0.6.18, also reported by a user on the full game):** the log stops after `gyro: settings ...`
    (inside `gl_sdl_create`). UE crash report: EXCEPTION_ACCESS_VIOLATION reading 0x0 in `MSVCP140+0x12590` (the
    internal lock routine behind `_Mtx_lock`) called from `gyrolib.dll`. The game ships the VC++ runtime 14.24 next to
    its exe (`msvcp140*.dll`); the process loads that copy and `gyrolib.dll` (dynamic CRT, built with MSVC >= 14.40)
    gets it too. Known MSVC break: binaries built with 14.40+ construct `std::mutex` with a constexpr constructor that
    old `msvcp140` (< 14.40) can't lock. Our `winmm.dll` uses the static CRT and is not affected. Workaround verified:
    renaming the game's `MSVCP140.dll` and `MSVCP140_2.dll` makes it start (system 14.51 runtime used). Real fix belongs
    in GyroLib (static CRT `/MT`, or `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` on all its C++ code); reported upstream by
    the user.
  - **Camera not found yet.** The generic view does nothing (user: no movement, no jitter). Auto probe: the profile it
    wrote (Pawn raw 0x1284,0x818), ReplicatedControlRotation and all raw Pawn rotators at once (0x7EC, 0x818, 0x1278,
    0x1284) are all ignored: 0x7EC/0x818/0x1284 snap back to the camera every frame, 0x1278 keeps our writes but isn't
    read. Manual probe (F7/F8, 4 diffs, yaw only): every rotator, float and raw value that moves is a copy of
    `PC.ControlRotation` (= POV), or yaw - 90 (Pawn+0xCF8, Pawn+0x1D20, HeadSpringArm). So ControlRotation is rebuilt
    every frame from a source outside the probe's roots or in another form (vector, quaternion).
  - **GyroUE 0.6.19 (test build 0.6.20 during the session):** the status block logs `status: addresses pc=0x... (ControlRotation +0x...) pawn=0x...
    camera=0x...` whenever they change, for a debugger write breakpoint on ControlRotation (next step: find who writes
    it each frame). Deployed to the Severed Steel demo (`winmm.dll`, generic view, the probe's profile parked as
    `.ini.probe`).
  - **Write breakpoint on `PC.ControlRotation.Yaw` (cdb, `ba w4`, 6 hits):** besides GyroUE's own writes (several per
    frame: the game pumps messages from more than one place, each runs `hooked_peek`), the only writer is
    `movsd [rbx+288h], xmm0` after an `Equals` tolerance test, i.e. `APlayerController::SetControlRotation`, once per
    frame, called through the Blueprint VM (ProcessEvent / script frames on the stack). So a Blueprint computes the view
    rotation from its own state each frame and sets it; ControlRotation is never read back. Candidate state: Pawn+0x1D20
    (unwrapped yaw - 90, looks accumulated) and Pawn+0xCF8 (wrapped yaw - 90), both raw and not reflected floats, so
    probably inside a Vector/Transform BP variable. Next: `Dump=1` on the pawn to name them.
  - cdb note: quitting with `qd` from inside a `ba` command string ended the game process (no crash report). Detach
    with a cleared breakpoint (`bc *; .detach; q`) next time.
  - Correction: the caller is native, not Blueprint. `Dump=1` on the pawn: `PlayerCharacter` (native C++ class under
    `ALSBaseCharacter`) has 419 reflected properties; +0x1D20 sits in an unreflected gap (0x1D04-0x1D2B). Writing it
    (`Offsets=0x1D1C`) is ignored too (yaw snaps back, pitch at 0x1D1C keeps our writes but isn't read).
  - Static disassembly (`cdb -z` on the exe, addresses relative to its preferred base): PlayerCharacter's tick reads
    the look input `+0xBD8` (yaw) and `+0xBDC` (pitch), calls two functions (yaw, pitch) that turn the
    `FirstPersonCamera` component (`+0x12A0`) from its current world rotation (`ComponentToWorld` quat at comp+0x1C0,
    cached rotator at comp+0x190), then `Controller->SetControlRotation(camera world rotation)` (vtable +0x688). So
    ControlRotation is an output of the camera component. Writing `Pawn.FPCamera.RelativeRotation` is ignored as well
    (user: nothing, standing or walking): the camera's world rotation isn't driven by its RelativeRotation.
  - The look inputs are written by the game's axis handlers every frame: the mouse handlers store
    `+0xBD8`/`+0xBDC` unconditionally (0 when the mouse is still); the gamepad ones only past a dead zone, into
    `+0xBD8`/`+0xBDC` or `+0x1B2C`/`+0x1BC4` depending on two settings bytes (`[pawn+0x1228]+0xDFD/0xDFE`). They run in
    the PlayerController's input processing, after our PeekMessage frame, so writes there are overwritten too.
  - ~~Conclusion: no memory location GyroUE can write drives this camera.~~ **Wrong, corrected on review:** the
    look functions (pitch one at `0x141705510`, preferred image base) read the camera's world rotation
    (ComponentToWorld quat at comp+0x1C0, rotator cache comp+0x180/0x190), apply the input (pitch clamped to
    StandingPitchMin/Max, pawn+0x14A4/0x14A0) and call `SetWorldRotation` on the camera (quat path while falling,
    rotator path otherwise). So the persistent state IS the camera's world rotation. The camera uses absolute
    rotation (its RelativeRotation equals its world rotation while the capsule is turned too). Writing RelativeRotation
    alone failed because the game rebuilt it from the stale ComponentToWorld in the same frame.
  - USceneComponent layout seen (UE 4.27, float): RelativeRotation +0x128, WorldRotationCache +0x180 (quat) / +0x190
    (rotator), RelativeRotationCache +0x1A0 / +0x1B0, ComponentToWorld +0x1C0 (rotation quat first).
  - **One-shot proof (cdb noninvasive, `ed` with float bit patterns; `ef` can't parse negative numbers):** wrote
    RelativeRotation yaw 91.31 -> 136.31 plus the matching quaternion at +0x1C0. Read back seconds later: both kept,
    and `PC.ControlRotation` became (P -2.64, Y 136.31) by itself. User: the camera turned 45 degrees and stayed,
    and the stick keeps working from there.
  - **GyroUE 0.6.19 (test build 0.6.21 during the session): `WorldRotation=1`** (view option, with `Property=RelativeRotation`). `ue::add_world_rotation`
    finds ComponentToWorld by value once per component class (16-byte aligned quat in the SceneComponent part equal
    to quat(RelativeRotation), skipping the two rotation caches, which are followed by the same rotator; floats or
    doubles from the property size), checks every frame that the stored quat still matches RelativeRotation (else
    skips the frame and logs the reason when it changes), adds the delta (pitch kept within +-89 so the rotator
    doesn't flip) and writes the rotator and quaternion (FRotator::Quaternion formula). Probe: an attached
    component's `RelativeRotation` whose world rotation matches it is now a usable candidate (`WorldRotation=1` in
    the suggestion) instead of "component transform (output)"; a CameraComponent ranks first. Root components stay
    outputs (actor rotation). ADVANCED.md: Example 3 + key docs.
  - Deployed the 0.6.21 test build to the Severed Steel demo with `Object=Pawn.FPCamera`, `Property=RelativeRotation`,
    `WorldRotation=1`. **Verified (user, DualSense): smooth and correct.** Log: `ue: CameraComponent
    ComponentToWorld rotation at +0x1C0 (absolute rotation, float)` at the first gameplay frame, `camera: confirmed`
    3 s later, no `WorldRotation not applied` lines. Profile added as `profiles/ThankYouVeryCool-Win64-Shipping.ini`;
    README lists Severed Steel with the msvcp140 rename note. Test profiles removed from the demo folder.
  - **Probe verified (0.6.21 test build, no profile, Probe=1, 3 scored stick captures):** the profile it wrote picks
    `Object=Pawn.FPCamera`, `Property=RelativeRotation`, `WorldRotation=1` (CameraComponent ranked first); the old
    picks (Pawn raw 0x1284,0x818, RightHandSpringArm raw 0x258, ReplicatedControlRotation) and
    `Pawn.RIghtHandGripLoc` RelativeRotation + WorldRotation=1 follow as alternatives. User restarted with it: works
    (`camera: confirmed`). The repo profile was put back afterwards.
  - **Regression check, Cronos (UE 5.4.4) updated 0.6.14 -> 0.6.21 test build (`version.dll`), no profile, Probe=1, 4 scored
    captures:** the pick is unchanged, `Object=Pawn.View`, `Offsets=0x2A0,0x420`, `RotatorSize=24` (alternatives
    Pawn.SpringArm raw 0x2D8 and Pawn.Animation.LastAimRotation); no WorldRotation candidate appeared. User restarted
    with it: works (`camera: confirmed`). Verified profile restored, Probe=0.

- 2026-10-11 **GyroUE 0.6.19: dependency check before loading GyroLib (`deps.cpp`).**
  - Why: GyroLib (1.3.0 and 1.3.1) links the VC++ runtime as DLLs (MSVCP140, MSVCP140_2, VCRUNTIME140,
    VCRUNTIME140_1; linker 14.51) and delay-loads SDL3.dll. Windows shares a DLL by name, so a copy the game already
    loaded, or one next to its exe (searched before System32), is the one GyroLib gets. Re-checked with 1.3.1 in a
    standalone test (static-CRT exe + gyrolib.dll + the demo's 14.24 runtime copies in its folder): `gl_sdl_create`
    segfaults; with the system's 14.51 runtime it works. So 1.3.1 still has the bug (good repro for the upstream report).
  - What: `deps::check_gyrolib()` runs in `start_gyro()` before `LoadLibrary(gyrolib.dll)`. It reads gyrolib.dll's
    linker version and its regular + delay-load imports (mapped with LOAD_LIBRARY_AS_IMAGE_RESOURCE, nothing runs),
    finds the copy of each VC++ runtime DLL GyroLib would get (already loaded > exe folder > System32) and reads its
    version from the module's version resource (not GetFileVersionInfo: version.dll is one of GyroUE's own proxy names).
    MSVCP* older than 14.40 with GyroLib built by 14.40+ = blocking: GyroLib isn't loaded, the gyro stays off for the
    session, the game starts normally, `deps:` log lines + a 45 s notice name the files and the fix (rename the game's
    copies, or install the VC++ Redistributable if System32's copy is the old one). Older than the build tools but
    14.40+ = log warning only. SDL3.dll already loaded by the game = log warning with both versions. Otherwise one
    `deps:` info line per runtime DLL (useful in future crash reports).
  - Tested on the Severed Steel demo (launched via Steam by the agent): with the game's 14.24 MSVCP140/MSVCP140_2 in
    place, no crash, the main menu loads, the log names both files (MSVCP140 already loaded by the game) and the notice
    is shown complete over the menu (screenshot); VCRUNTIME140/_1 14.24 only warn. With the two files renamed back to
    `-bak`: info lines (MSVCP140 14.51 from System32), `gyro: SDL reader started`. TROUBLESHOOTING, ADVANCED (log
    reference) and README updated.

- 2026-10-11 **GyroUE 0.8.0: SDL controller shown to the game as XInput** (branch `feature/xinput-translation`; the
  local test builds deployed below were numbered 0.7.0 and 0.7.1 before main released its own 0.7.0).
  Goal: games that only read XInput (UE's `XInputInterface`) and run without Steam Input (Epic: Luto) ignore a
  DualSense / Switch Pro except for the gyro. New `xinput.cpp` answers the game's `XInputGetState` for one slot with the
  SDL state of GyroLib's selected controller (`gl_output.physical_id` / `gl_get_selected_device` -> `gl_get_endpoint`
  -> `gl_sdl_endpoint_for_instance` -> `SDL_GetGamepadFromID`, no extra reference), `XInputSetState` -> `SDL_RumbleGamepad`
  (refreshed every 500 ms, 1 s duration), `XInputGetCapabilities`, `XInputEnable`, `XInputGetStateEx` (ordinal 100, adds
  Guide 0x0400). State is built on the game thread right after `gl_update` and read under a mutex by the hooks.
  - Findings: **Luto (UE 5.5.4) imports `XINPUT1_4.dll!XInputGetState/XInputSetState` by name through the delay-load
    table**; `hooks.cpp` only walked the regular import table, so it now also walks `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT`
    (an unresolved slot points to the linker stub inside the module: it is patched before the first call, so the stub
    never runs, and the original is resolved later from `hooks::source_dll()`). The **Windows SDK's `xinput.lib` imports
    by ordinal** (xinput1_3/1_4: 2 GetState, 3 SetState, 4 GetCapabilities, 5 Enable, 100 GetStateEx; xinput9_1_0 uses
    other ordinals but exports by name), hence the new `hooks::add_ordinal` (exact DLL name required).
  - Only game modules are patched, so SDL in GyroLib (which loads XInput dynamically) never sees the virtual pad: no
    feedback loop. That's also why it's off in inline-hook mode (protected exes): an export hook would reach SDL too.
  - UE only re-polls empty XInput slots after `WM_DEVICECHANGE` (`bNeedsControllerStateUpdate`), so GyroUE posts
    `WM_DEVICECHANGE(DBT_DEVNODES_CHANGED)` to the game thread's `UnrealWindow`s when the pad appears or goes. Not yet
    verified in a real UE game.
  - Settings (`GyroUE.ini`, profile overrides): `XInput` 0 off / 1 auto (default) / 2 always, `XInputNintendoLabels`
    (swaps A<->B and X<->Y; the first version used `SDL_GetGamepadButtonLabel`, replaced by an unconditional
    swap so it also works for pads SDL can't label), `XInputTouchpadBack` (touchpad click = View).
  - Auto mode keeps it off when the pad is Xbox-type or has a Steam handle, when any real XInput slot is connected
    (watcher thread, 1 s, so empty-slot polling never costs the game thread), or when the game reads pads itself:
    `libScePad*.dll` or a non-GyroLib `SDL2/SDL3.dll` loaded, `RegisterRawInputDevices` usage page 1 / usage 4,5,8,
    `CreateFileW` on `\\?\hid#vid_<selected pad vendor>`, or `DirectInput8Create` (all observed from game modules only).
    It waits 2 s after the pad appears before showing it.
  - While translating, GyroUE also declares `GL_HOST_NATIVE_STICK_SUPPRESSION` and zeroes the right stick when GyroLib
    asks (`suppress_native_right_stick`, flick stick). Untested.
  - Verified so far: builds clean (/W4); a stand-in exe (`*-Win64-Shipping.exe`, Windows SDK `xinput.lib` with
    `/DELAYLOAD:XINPUT1_4.dll`, an `UnrealWindow`, PeekMessageW loop) gets `XINPUT1_4.dll!XInputGetState (ordinal 2)`
    and `XInputSetState (ordinal 3)` patched in the delay-load table and its calls reach GyroUE. **No controller was
    connected, so the translation itself, rumble and the `WM_DEVICECHANGE` re-poll are not verified yet; nothing run
    in game.**
  - Deployed to Luto (`winmm.dll`, replacing 0.6.19; existing `GyroUE.ini`/`gyrolib.ini` kept, so the new keys
    take their defaults: `XInput=1` auto). Awaiting the in-game test.
  - **Luto test of 0.7.0, DualSense (connection not reported), Epic, no Steam Input: works.** User: every input they
    could try worked. Log: both delay-load slots patched before the first call; `DualSense Wireless Controller shown as
    XInput controller 0` 2.8 s after the SDL reader started, `WM_DEVICECHANGE posted to 1 game window(s)`, and the game's
    first `XInputGetState` call came 1.1 s later (so Luto doesn't poll XInput before its engine loop runs; whether the
    `WM_DEVICECHANGE` was needed isn't proven). No auto-mode false positive (Raw Input, HID, DirectInput) in Luto.
    Frame cost unchanged (idle avg ~58 us, active ~70 us).
  - **Luto, Switch Pro over Bluetooth, no Steam Input: basic input works (user).** `Nintendo Switch Pro Controller shown
    as XInput controller 0` 4.6 s after start, game's first poll 0.7 s later. Default mapping (by position).
  - Decision: ship as **alpha with the default mapping**. Extra buttons stay unmapped for now (DualSense mic button,
    Edge paddles/Fn, Switch Capture; PS/Home only through `XInputGetStateEx`). Ideas parked in "Ideas for later".
  - **Terminator: Resistance (UE4, Steam)** imports `XINPUT1_3.dll` **by ordinal (2 GetState, 3 SetState) in the
    regular import table**: covered by the ordinal hooks. Updated there from 0.6.17 to 0.7.0, kept as `version.dll`
    (alongside PSButtonsUE `dsound.dll` and ReShade `dxgi.dll`). **Tested: works perfectly (user, DualSense)**; log:
    ordinal 2/3 patched in the import table, `DualSense Wireless Controller shown as XInput controller 0`.
- 2026-10-11 **GyroUE 0.8.0 (local test build 0.7.1): native pad libraries detected from the import tables.** The Outer Worlds: Spacer's Choice
  (Epic, UE4, exe `IndianaEpicGameStore-Win64-Shipping.exe`, native DualSense) ships `libScePad.dll` and imports it
  **through the delay-load table** (`scePadInit`, `scePadOpen`, `scePadReadState`, `scePadSetTriggerEffect`), plus
  `XINPUT1_3.dll` by ordinal 2/3 and `RegisterRawInputDevices`. A delay-loaded libScePad only appears in the process on
  the game's first scePad call, possibly after the 2 s grace, so 0.7.0 could show the pad as XInput and then withdraw
  it. New `hooks::imported_dlls()` lists the game modules' regular and delay-load imports; auto mode treats an import of
  `libScePad*` or `SDL2/SDL3.dll` as native support, checked in DllMain (`xinput::install`) and on every watcher pass
  (modules loaded later). Deployed there as `winmm.dll` (replacing 0.6.18). Expected log: `xinput: the game reads
  controllers itself: it imports Sony's pad library (libScePad.dll, delay-load, ...)` and `not shown`.
  - **Tested (user, DualSense, Epic): all good.** Log: native support detected in DllMain from the import table, before
    the pad was found; `DualSense Wireless Controller not shown: it imports Sony's pad library`; the game still polls
    XInput (`XINPUT1_3` ordinals, real devices only), and its native DualSense input is untouched. Generic gyro path
    works (`view=Camera`, `IndianaPlayerCharacter_BP_C`); main menu correctly `view=none` (DefaultPawn, CineCameraActor).

- 2026-10-11 **Hogwarts Legacy (Steam, UE 4.27, exe `HogwartsLegacy.exe`, UE4SS 3.0.1 also installed), GyroUE 0.8.0.**
  - **Engine not readable:** GUObjectArray found (`ObjObjects` at exe+0x97EB7E0, the same array UE4SS finds at
    exe+0x97EB7D0) and FNamePool (header len high bits), but `detect_layout` fails every 5 s with `ue: core classes not
    found` (UClass `Class` itself was found). So no scene, no view, gyro off, and no notice: the "camera ignores the
    gyro" notice needs a view, and the probe needs the same reflection data. Not a regression: this code is unchanged
    since the first commit; the game was never supported. UE4SS reports a standard UObjectBase (Class 0x10, Name 0x18,
    Outer 0x20) but `FProperty::Offset_Internal = 0x4C` (stock 4.25+: 0x44), and resolves names through
    `FName::ToString` rather than the pool, so the name decoding or the property layout may differ.
  - Added: `ue: core classes missing: ...` logs once which of Object/Actor/Controller are missing, how many objects are
    classes and the first 12 class names, and for each missing name the object with that name and its class (to tell
    a name-decoding problem from a class-pointer one). After 60 s without readable engine data, GyroUE logs it and
    shows a 20 s notice (game not supported yet, report it with the log) instead of staying silent.
  - **XInput:** the DualSense was shown as XInput at +3 s, then withdrawn at +18 s when the game opened its HID device
    (`it opens the controller's HID device itself (vendor 1356)`): Hogwarts has native DualSense support. User: the
    notices appeared and the translation stopped correctly. The exe **imports `hid.dll`** (`HidD_GetFeature`,
    `HidD_SetFeature`, `HidD_GetAttributes`, `HidP_GetCaps`...), which stock UE doesn't (Luto doesn't); UE's RawInput
    plugin does, also native support. Auto mode now treats a `hid.dll` import as native, so such games never get the
    XInput pad, not even for those 15 s.
  - Built as 0.8.0 (user's decision: not released yet) and deployed to Hogwarts (`winmm.dll`). Waiting for the log with
    the class diagnostics.
  - **Diagnostics run (user, while the game compiled shaders):** `core classes missing: Controller; 9929 of 96600
    objects are classes (first: Object, p, i, e, s, r, i, ...)`; Object (#1) and Actor (#22) were found. Single letters
    = names decoded from the wrong memory. The 60 s notice fired too (correctly: the data really couldn't be read).
  - **Root cause: a false FNamePool.** Read-only `ReadProcessMemory` look at the running game: the exe image holds three
    qwords equal to name block 0. GyroUE took the first, rva 0x9754380 (CurrentBlock 0, block 1 null, blocks 2+ set
    0x20000000 apart): it isn't the pool, so only block-0 names (Object 0x162, Actor 0x16A) decoded; index 0xFC10C
    (block 15) landed in UTF-16 config text. The real pool is at **rva 0x97AF290** (CurrentBlock 131, blocks every
    0x20000). Name entries are stock (2-byte header, len in the high 10 bits, stride 2).
  - Fix in `find_name_pool`: a candidate must have every block up to CurrentBlock set and the next four null (the
    allocator fills blocks in order). Checked against the live game: both false candidates rejected, the real one
    accepted, and with it Controller (#828), PlayerController, PlayerCameraManager and GameEngine decode. All of those
    exist from startup, so the engine can be read in menus/shader compilation too. Still open: UE4SS reports
    `FProperty::Offset_Internal = 0x4C` (stock 0x44); `detect_layout` measures it at runtime, to be seen in game.
  - **In game with the pool fix (user):** `ue: layout (FField properties, UE 4.25+) SuperStruct=0x40 ChildProperties=0x50
    Field.Class=0x8 Field.Next=0x20 Field.Name=0x28 Property.ElementSize=0x3C Offset_Internal=0x4C sizeof(Property)=0x78`
    (FField is 8 bytes larger than stock from Next on; detected at runtime, no change needed), `ue: engine GameEngine_...`,
    `view: Camera`, `gyro on`. PC `BP_Phoenix_Player_Controller_C` (ControlRotation +0x2B0), pawn `BP_Biped_Player_C`,
    view target **`BP_ThirdPersonCameraStackActor_C`** (gameplay) / `BP_SceneRigCameraStackActor_C` (scene rigs).
    GyroUE writes ControlRotation (applied yaw grows) but no notice appeared: `follow_check` only judged the camera
    when the view target is the pawn (other targets were taken as scripted cameras), and Hogwarts' gameplay camera
    is its own actor. Also seen: calib=0 and bias 0 the whole session, so applied yaw drifted ~0.5 deg/s while the pad
    lay still (no menu calibration happened yet).
  - Fix: `player_camera()` also accepts a view target class whose camera turned 30+ degrees (POV) while the right
    stick was held; logged once as `camera: view target X turns with the right stick: treated as the player's camera`.
    Then the usual 2 s windows decide (notice to set Probe=1 if the gyro is ignored). The auto probe never had the
    pawn-only filter. Built as 0.8.0; Hogwarts was running again, not deployed yet.
  - **Probe run (user, DualSense, Probe=1, 5 scored captures):** `camera: view target BP_ThirdPersonCameraStackActor_C
    turns with the right stick` logged as intended. Roots: PC `BP_Phoenix_Player_Controller_C`, camera manager
    `PhoenixCameraStackManager`, view target `BP_ThirdPersonCameraStackActor_C`. Six rotations followed the camera every
    time; five are outputs (`Camera.TransformComponent0` RelativeRotation and +0x1D0, both POV caches,
    `ViewTarget.Camera(CameraStackCineComponent)+0x1B0`); the pick is raw float rotator **+0x1C8 in
    `ViewTarget.CameraStackBehaviorConvertToTranslation_<n>`**. ControlRotation never followed (ignored by this camera).
  - **The written profile couldn't work:** `Object=ViewTarget.CameraStackBehaviorConvertToTranslation_2147460121`, but
    that number is a per-session generated name (`_2147460128` in the next session), so the object was never found.
    Probe fix: a path step whose name is `<Class>_<digits>` is labelled `@Class` when `@Class` resolves to that same
    object (first object of that class under the same outer, by index, as `find_subobject_of_class` does).
    The deployed profile was edited by hand to `Object=ViewTarget.@CameraStackBehaviorConvertToTranslation`
    (original kept as `HogwartsLegacy.ini.probe-original`), Probe=0. Whether +0x1C8 is an input or another output is
    the open question for the next test.
  - **Test of the @Class profile (user): no camera motion.** The object resolved
    (`view object CameraStackBehaviorConvertToTranslation_2147460037 +0x1C8=(P -5.55 Y 158.49)`), but after the gyro
    added 71.6 degrees the value was back at Y 158.49: +0x1C8 is rewritten every frame, another output.
  - **Read-only memory hunt** (`ReadProcessMemory`, scratch script, game running): roots = the camera-stack objects,
    pawn and PC (0x1000 bytes each) plus every heap block one pointer away (0x600 bytes); 12 snapshots taken while the
    camera was still between stick turns (yaw both ways, pitch -18..+26); floats whose change matched the camera's in
    every turn. Results: PC `ControlRotation` (+0x2B0) tracks the camera exactly (an output, which is why writing it
    did nothing); **`BehaviorStack` (class `CameraStack`, subobject of the view target, also at view target +0x340)
    holds a float rotator at +0x68 (P, Y, R) mirrored at +0x80**, about 3 degrees above the final pitch and slightly
    behind in yaw (the base rotation before the stack's offsets/smoothing), which is why the probe (exact matches
    only) missed it. Others: `CameraStackBehaviorMaintainTargetFraming+0x1D4` (yaw - 20),
    `CameraStackBehaviorAddOffsetsWhileCrouching+0x418` (pitch), CineComponent +0x14C (yaw relative to the actor).
  - **One-shot write proof (`WriteProcessMemory`, user agreed):** +45 yaw at BehaviorStack +0x6C and +0x84: camera
    output -37.26 -> 7.75 and stayed (checked over 3 s); +10 pitch at +0x68 and +0x80: -20.89 -> -10.58, stayed.
  - Profile `profiles/HogwartsLegacy.ini`: `Object=ViewTarget.BehaviorStack`, `Offsets=0x68,0x80`, `RotatorSize=12`
    (the name has no instance number; it resolves as a subobject of the view target). Deployed; needs a game restart
    and a gyro test. Lesson for the probe: a camera's state can be a smoothed/offset copy of the POV, not an exact
    match; a delta-based scan (as above) would find it.

- 2026-10-11 **Probe with live test (built as 0.8.0, not verified in game yet).** Why: the passive probe accepted any
  rotation that held the camera's values and wasn't in a known output location, so outputs elsewhere slipped through
  and became the profile (Hogwarts: `ConvertToTranslation+0x1C8`), while a look rotation kept with an offset was never
  seen (Hogwarts: `BehaviorStack+0x68`, pitch 3 degrees above the POV). User's design: test every candidate by writing.
  - Candidates (`follows()` in ue.cpp): a rotator that holds the POV's exact pitch/yaw, **or changed by the POV's
    turn** (tolerance max(0.6, 3%) per axis; scored turns are 10+ degrees of yaw), reflected or raw, float or double.
    `Candidate.exact` counts the exact hits. `probe_groups()` groups them (raw rotators of one object and size
    together, as a profile writes them; reflected properties one by one), usable first, passive "outputs" after.
  - Live test (host.cpp `run_live_test`, Probe=1 automatic mode only), after 3 scored captures, up to 40 groups:
    camera at rest 600 ms, then a 250 ms null window (no drift > 0.2), write +3 yaw through the same `apply()` a
    profile uses (so the object path, @Class, WorldRotation are exercised too); pass if the POV turns +3 (+-0.5)
    within 700 ms and holds 150 ms, then the turn is undone and the camera must come back; then the same in pitch
    (towards level). Fail: `overwritten (output)` (value rewritten) or `kept, camera didn't move`. Round 2 retests the
    passes; a pass in round 1 only is `inconsistent`. Stick, pause, cursor, focus loss or another view target abort a
    test and undo its turn (pending until it can be written), 3 retries. Notes whether the pawn's own rotation turned.
  - Result: a verified group becomes the profile (or `.suggested.ini` if the game had one), header `VERIFIED LIVE`;
    otherwise up to 8 captures, testing the new candidates each time, then only an unverified `.suggested.ini`. **An
    unverified guess is never written as the game's profile any more** (also for manual F7/F8 diffs). The gyro stays
    paused until the probe is done. On-screen notices guide each step.
  - Deployed to Hogwarts with the working profile set aside (`HogwartsLegacy.ini.verified`) and Probe=1, to check
    that the probe finds and verifies BehaviorStack by itself and rejects +0x1C8.
  - **First run (user, Hogwarts, 3 captures): it verified the wrong camera.** 8 groups tested: MotionCamera raw
    and TransformComponent0 raw `kept, camera didn't move`; the RelativeRotations `overwritten (output)`;
    **`ViewTarget.Camera` (CameraStackCineComponent) raw 0x1B0,0x1D0 MOVES THE CAMERA** in both rounds, written as
    the profile. User: it works, but it is a free camera (no arm: it turns on the spot instead of orbiting the
    character). BehaviorStack +0x68/+0x80 was a candidate in captures 1 and 2 but not in capture 3 (52 degrees of
    pitch: its offset to the POV depends on the pitch, framing), so the old every-capture rule kept it out of the
    test. Also a candidate: `BP_RotationLag_GamepadInput_C` +0x1E0/+0x1EC/+0x1F8 (captures 1-2).
  - Fixes: (1) groups need most scored diffs, at least two (`max(2, (n+1)/2)`), since the live test decides; (2)
    **orbit check**: every capture measures how far the camera's POV location moves for the turn's view angle
    (radius = chord / (2 sin(angle/2)), median of the captures, logged); with a radius of 30+ units (third
    person) a candidate's +3 yaw must also move the camera at least 40% of the expected chord, else verdict
    `kTurnsInPlace` (turned the view in place, not like the stick). First-person cameras (radius about 0) skip it.
    `ue::camera_pov_location` reads `CameraCachePrivate.POV.Location` (float or double). Redeployed; the free
    camera profile kept as `HogwartsLegacy.ini.probe-freecam`, Probe=1 again.
  - **Second run (user, Hogwarts, DualSense, 3 captures): correct.** Orbit radius 235/205/202 (median 205, third
    person). 15 groups tested: **`ViewTarget.BehaviorStack` raw 0x68,0x80 MOVES THE CAMERA in both rounds (yaw and
    pitch; camera moved 10.7 / 10.8 for an expected 10.6)**; `ViewTarget.Camera` raw 0x1B0,0x1D0 rejected as turning in
    place (moved 0.0 of 10.6); outputs rejected as overwritten (ConvertToTranslation +0x1C8, PC ControlRotation and
    +0x8F0, BP_RotationLag_GamepadInput_C +0x1E0/+0x1EC/+0x1F8, RelativeRotations); MotionCamera and
    TransformComponent0 raw kept without effect. The generated profile equals the hand-verified one. Fixed the
    log label of kTurnsInPlace (it printed "not testable").
  - **Regression check, Severed Steel demo (UE 4.27.2, first person), no profile, Probe=1, 3 captures (user: correct,
    profile works).** Installed fresh as `winmm.dll` (the game's 14.24 `msvcp140.dll`/`msvcp140_2.dll` renamed to
    `.bak` again, user agreed; the repo profile set aside as `.verified`). Orbit radius 0 in all captures (turns on the
    spot), so no orbit check. 16 groups: **`Pawn.FPCamera.RelativeRotation` (WorldRotation=1)** and `Pawn.FPCamera` raw
    0x190,0x1B0 both MOVE THE CAMERA in both rounds (noted: also turns the character, expected in first person); the
    named property ranks first, so the profile equals the verified one. Outputs (Pawn raw 0x818,0x1284,
    ReplicatedControlRotation, RIghtHandGripLoc, ControlRotation) overwritten; spring arm and TransformComponent0 raw
    kept without effect. The live test exercised the WorldRotation write path.
  - **Regression check, Cronos (UE 5.4.4, third person, double rotators), no profile, Probe=1, 3 captures (user: works).**
    First attempt ran the old 0.6.19: the folder had GyroUE 0.6.19 as `version.dll` (loaded first) and 0.8.0 as
    `winmm.dll` (stayed inactive, one GyroUE per process), so the old probe wrote an unverified profile after two
    captures. Redeployed 0.8.0 as `version.dll` (the script removed `winmm.dll`). Orbit radius 190/166/194. 6 groups:
    **`Pawn.View` raw 0x2A0,0x420 (RotatorSize 24) MOVES THE CAMERA in both rounds (camera moved 9.8 for an expected
    9.6)**; SpringArm raw 0x2D8, Animation.LastAimRotation, TransformComponent0.RelativeRotation overwritten. The
    profile equals the verified one. With Hogwarts and Severed Steel: the live-test probe reproduced all three
    verified profiles by itself (third person / first person with WorldRotation / UE5 double raw rotators).

- 2026-10-12 **Luto, GyroUE 0.8.0 (local build): flick stick and `XInputNintendoLabels` tested (user: works as it
  should).** `XInputNintendoLabels=1` now swaps A<->B and X<->Y unconditionally (any controller) instead of relying on
  `SDL_GetGamepadButtonLabel`. Tested with a DualSense (swap applied), Epic, no Steam Input: `XInput=1 (auto)
  XInputNintendoLabels=1`, `DualSense Wireless Controller shown as XInput controller 0`. GyroLib flick stick
  (`context.1.flick.mode=2`) works with the translated pad: `GL_HOST_NATIVE_STICK_SUPPRESSION` is advertised while the
  pad is shown as XInput and the virtual pad's right stick is zeroed when GyroLib asks (`suppress_native_right_stick`).

- 2026-10-12 **`XInput=2` now keeps the game from reading PlayStation/Nintendo controllers itself.** Before, `XInput=2` in a game with native DualSense support gave double input (native + translated).
  - Finding (`dumpbin /imports`): `libScePad.dll` (Pacific Drive, It Takes Two) imports exactly the `hid.dll` set that
    Hogwarts Legacy, Cronos, Banishers, Days Gone and Jedi: Fallen Order import from their exe (HidD_GetAttributes/
    GetFeature/SetFeature/Get*String/GetHidGuid/GetPreparsedData, HidP_GetCaps/GetValueCaps, plus SetupDi*, ReadFile,
    GetOverlappedResult(Ex)): those five most likely link Sony's pad library statically. Either way the controller is
    opened with `CreateFileW` on its HID path.
  - Change: with `XInput=2` the `CreateFileW` hook (now installed in auto and always modes, and also patched into a
    separate `libScePad*.dll` through `hooks::add_with_pad_library`) refuses HID paths of Sony (054C) and Nintendo
    (057E) devices and of the selected controller with `ERROR_ACCESS_DENIED`; `RegisterRawInputDevices` entries for
    joysticks/gamepads are dropped. HID path IDs are now parsed for Bluetooth paths too
    (`_vid&0002054c_pid&0ce6`; before, only USB `vid_054c` was recognised by the auto-mode HID check).
  - `XInput=2` now also stays off while a real XInput controller is connected (DS4Windows/DSX, Steam Input, Xbox pad):
    the same pad through a remapper would act twice.
  - **Tested, DualSense (user: works in both, no double input).** Pacific Drive (`XINPUT1_3`): `libScePad.dll` loads
    after the controller is shown (+5 s) and is patched as it loads (`hooks: CreateFileW patched in the pad library
    module`), then its first opens of `054C:0CE6` are refused 0.1 s later. Cronos (`XINPUT1_4` delay-load, Sony's
    library in the exe): the exe's opens of `054C:0CE6` are refused right after its first `XInputGetState`. Both
    show `DualSense Wireless Controller shown as XInput controller 0`. Bluetooth not tested yet.

- 2026-10-12 **Native flick stick (`NativeFlickStick=1`, off by default (user), new `nativepad.cpp`).** Goal: GyroLib's
  flick stick in games with their own DualSense support without giving it up for `XInput=2`. Switching between
  native and XInput at runtime was rejected: the game keeps its HID handle open (a switch means faking a disconnect,
  possibly a "controller disconnected" pause), and GyroLib's `suppress_native_right_stick` changes with views and the
  aim context, so it would switch on every aim.
  - All eight native-DualSense games checked read through Sony's pad library (separate or linked in), which opens the
    pad with `CreateFileW` and reads with `ReadFile` + `GetOverlappedResult(Ex)` (It Takes Two's older
    `libScePad.dll` imports only `ReadFile`: synchronous reads, handled too).
  - `nativepad.cpp` now owns the `CreateFileW` hook (moved from `xinput.cpp`: XInput=2 refusal and auto mode's
    opened-vendor check use it through `nativepad::refuse_pads` / `opened_vendor`) and adds `ReadFile`,
    `GetOverlappedResult(Ex)` and `CloseHandle` (game modules + `libScePad*.dll`). Sony handles opened with read
    access are followed (max 8; a handle value reused by another `CreateFileW` or closed is dropped); pending
    overlapped reads are matched by `OVERLAPPED*` and handled when `GetOverlappedResult(Ex)` reports them done.
  - Layouts: `0x01` RX/RY bytes 3-4, `0x31` (DualSense BT) 4-5 + CRC32 at 74, `0x11` (DS4 BT) 5-6 + CRC32 at 74 (CRC
    of 0xA1 + bytes 0..73). Reports whose CRC doesn't verify are left alone. The layout counts as checked when the
    report's right stick agrees with SDL's (byte*257-32768, tolerance 48) in >= 15 deflected frames (and >= 4x the
    misses); 30+ misses dominating -> never suppressed for that pad. Only then `GL_HOST_NATIVE_STICK_SUPPRESSION`
    is declared (host: `xinput_shown || native`), and while GyroLib asks, RX/RY in the game's copy become 128.
  - **Pacific Drive, first test (DualSense USB, `XInput=1`, `NativeFlickStick=1`): flick works, but the stick still
    turned the camera too** (holding it up pitched the camera up while the flick rotated). Log: libScePad opens
    `054C:0CE6` (opens, closes, reopens 0.5 s later), reads overlapped, done at once, **640 bytes per read = ten 64-byte
    USB reports**; only the first was centred, so the other nine still carried the stick. Layout check passed (15/15),
    suppression on, 28156 reads changed. Fix: the device's `InputReportByteLength` (`HidP_GetCaps` on the game's
    handle, fallback 64/78 by report ID) splits each read, and every report in it is centred.
  - Pacific Drive, second test (same setup): `64-byte input reports`, reads of 768 bytes (12 reports), every report
    centred (~490/s, matching the pad's USB rate), 15/15 layout check; suppression follows GyroLib (centred on foot
    and in the car, passed through in pause and main menu).
  - **Cronos (Sony's library linked into the exe, DualSense USB, `XInput=1`): works (user).** Same read pattern as
    `libScePad.dll`: opens `054C:0CE6` (closes and reopens at once, same handle value), overlapped reads of 640 bytes
    (10 x 64), 15/15 layout check, suppression on/off with gameplay and menus; the game closed the pad on exit and
    suppression was withdrawn.
  - Still to verify: Bluetooth (reports `0x31` + CRC), DualShock 4, a game with the separate DLL over Bluetooth.

## Status summary (2026-10-08)
- Pacific Drive: **done** (on foot + car profile, in-game panel, calibration). Install:
  `games/PacificDrive/deploy-gyroue.sh "<Win64 dir>"`; uninstall with `remove`. UE4SS is parked as `dwmapi.dll.off`.
- Returnal (UE 4.25, modular): **done**, generic view with no profile (v0.3.2), user-verified in combat.
- Cronos: The New Dawn (UE 5.4.4): **works** with a profile (raw double rotators in the pawn's `View` component),
  user-verified with 0.4.0. Covers UE5 double rotators.
- Cronos 0.4.5: calibration in menus (no cursor) and the F10 panel with HDR/RenoDX are both user-verified.
- Next: redeploy 0.4.5 to Pacific Drive/Returnal and smoke-test (render worker, menu detection, view keys).
- Life is Strange Remastered (UE 4, UProperty objects, DX11): **works** with the probe-found profile (+OnGyro for the
  walking camera), user-verified 2026-10-09. First DX11 game and first pre-4.25 property system.
- Gears of War: E-Day (UE 5.6, Xbox/GDK build, Themida): **works** with the generic view (no profile), user-verified with
  0.5.5 as `version.dll` (2026-10-09). Needs inline hooks, the System32 DXGI factory vtable and the heap scan for
  GUObjectArray. Install with `PROXY=version.dll bash tools/deploy-gyroue.sh "<...>\Binaries\WinGDK"`.

## Ideas for later
- XInput translation extras (0.7.0 is alpha with the default mapping): split touchpad (left half View, right half
  Menu, via `SDL_GetGamepadTouchpadFinger`), extra buttons (Edge paddles, mic, Capture) copying a standard button
  from the ini. Not keyboard keys: UE would switch to keyboard/mouse prompts.

- **Auto-probe with closed-loop check** (discussed 2026-10-09, parked): find the camera's real rotation state without a
  manual probe. Candidates = float/double rotators in objects reachable from Pawn/PC/Camera that equal the POV; drop
  per-frame output copies; write a small delta (~0.5 deg) to each and keep the one that turns the camera next frame.
  Only in stable gameplay. Risks: writing to look-alike non-rotation data, cameras that aren't stored rotators
  (quaternions, input accumulators, animation-driven). Would cover Cronos-like cameras, not ADS/vehicle/menu signals.
  Worth it only if more custom-camera games show up.
- Heap scan as the universal GUObjectArray finder: works for any build, but the data-section scan stays first (instant
  in normal games); the heap scan remains the fallback.

- 2026-10-11 **GyroLib 1.3.1: upstream burst delivery replaces GyroUE's pacer.** GyroLib
  commit 9addfd9 answers our report (GyroLib issue #1). SHA256SUMS verified for the SDK, the demo and `gyrolib.dll`
  6841434…; ABI 1, INI schema 0.2.0 and headers unchanged apart from a comment. Also fixes GyroLib issue #2
  (INI `gyro.acceleration=0` with stale fast sensitivities stays Off).
  - **How GyroLib does it** (`src/detail/delivery.hpp`, `MotionDelivery`, per endpoint, inside `gl_update` after the
    camera/cursor angles and before trackball/flick): a sample batch counts as a burst when consecutive samples are
    >= 2 ms apart in sensor time but arrive < 1 ms apart; three consecutive bursts separated by updates with no
    samples latch pacing until the source restarts. Each update's integrated angle is then spread linearly over a
    window (peak-hold of the gap between batches, x0.98 per batch, 15-60 ms), starting at the next update; time-based,
    so a second update at the same timestamp outputs nothing. Pending angle is dropped on gates, calibration,
    view/target/profile/modifier changes and stalls > 100 ms. SDL source only (not Steam), no INI setting, flick stick
    and trackball not buffered. Synthetic tests only; no real Switch Pro test upstream.
  - **Compared with GyroUE 0.6.17** (queue per sample weighted by gyro speed, played at the measured sample rate,
    holding `gap_mean + 0.5σ` capped by PacingMaxMs, statistical on/off every frame, switchable). Synthetic replay of a
    Switch Pro BT stream modelled on the Luto trace (5.24 ms sensor, bunched arrivals, 60/85x2/144 fps): both cut empty
    frames from 42-75% to 0.3-2.6% and speed CV from 1.0-1.9 to 0.31-0.36 (GyroUE) / 0.38-0.44 (GyroLib; ideal
    0.24-0.44), and both add about 25 ms over the raw output. GyroLib's window sat at a median 39-42 ms.
    Risks to check on hardware: the latch never releases (a DualSense over BT whose reports bunch could get a
    permanent >= 15 ms window), and Steam-provider motion isn't paced.
  - **GyroUE change (no version bump yet):** the Pacer, `Pacing` and `PacingMaxMs` are removed (both pacers stacked
    would delay twice and re-bunch); old keys in existing `GyroUE.ini` files are ignored. `MotionTrace` stays (raw
    and applied columns are now equal). New status line from the sample observer and `gl_gyro_state.new_samples`:
    `motion N samples/s in M arrivals/s | camera updates U/s, T% turned, P% of those without a new sample (GyroLib
    burst delivery)`. P > 0 while turning means GyroLib is pacing; it must stay ~0 for a DualSense.
  - **Luto test (user), Switch Pro over Bluetooth, Steam Input off:** works well with GyroLib 1.3.1 and no GyroUE
    pacer (tested with a test build of this change; an existing `Pacing=1` line in GyroUE.ini is ignored).
  - **Still to test:** Switch Pro over USB, DualSense over USB and BT (the status-line P% must stay ~0, see the latch
    risk above), Steam Input on, and a `MotionTrace=1` capture for `tools/motion_trace_report.py`.

## Dead ends

(none yet)
