# GyroUE: problems?

| Problem | What to do |
|---|---|
| Nothing happens and there is no `GyroUE\GyroUE.log` | GyroUE isn't loading. Check it's next to the `-Win64-Shipping.exe` ([installation](README.md#install), step 2), then try renaming `winmm.dll` to `version.dll`. |
| The camera doesn't move, but the log exists | Close DS4Windows/DSX/reWASD, plug the controller in by USB, check Steam Input. If a box at the top of the screen says **the game ignores the gyro**, the game needs a profile: see [Finding the camera of a new game](ADVANCED.md#finding-the-camera-of-a-new-game). |
| The camera slowly drifts on its own | Put the controller on a table for a few seconds in a menu, or use **"Recalibrate gyro"** in the panel. |
| The camera moves twice as much as it should | Steam Input (or another tool) is also turning the gyro into mouse movement. Turn that off. |
| `F10` does nothing | Try it from the pause menu. DirectX 11 games have no panel: edit `GyroUE\gyrolib.ini` instead. |
| A box says **gyro disabled for this session** (old Visual C++ runtime) | The game ships its own old `MSVCP140*.dll` that would crash GyroLib, so GyroUE left the gyro off and the game runs normally. Rename the files the box names (they're in the game's `Binaries\Win64` folder, e.g. add `.bak`) and restart. The `deps:` lines in `GyroUE.log` give the details. |
| The game crashes | Uninstall GyroUE and report it (below). |

**Reporting a problem:** open an issue with the game name and version, your controller and how it's connected
(USB / Bluetooth, Steam Input on or off), and attach `GyroUE\GyroUE.log` from the session where it went wrong.

For the meaning of the lines in `GyroUE.log`, see the [log reference](ADVANCED.md#log-reference).
