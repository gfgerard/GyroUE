#!/usr/bin/env bash
# Generic GyroUE installer for any UE4/UE5 game.
#   bash tools/deploy-gyroue.sh "<game>/<Project>/Binaries/Win64"          install / update
#   bash tools/deploy-gyroue.sh "<game>/<Project>/Binaries/Win64" remove   uninstall
#   PROXY=winmm.dll bash tools/deploy-gyroue.sh ...                        install under another proxy name
# Copies version.dll (or $PROXY: the same DLL renamed, see src/GyroUE/proxy.cpp) + GyroUE/gyrolib.dll and the
# matching profile (profiles/<exe stem>.ini) if any. Use winmm.dll when the game doesn't import version.dll
# or another mod already uses it.
# gyrolib.ini (user settings) is never overwritten.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN64="${1:?usage: deploy-gyroue.sh <Win64 dir> [remove]}"
source "$ROOT/tools/gyrolib-sdk.sh"; SDK="$GYROLIB_SDK"
PROXY="${PROXY:-version.dll}"
case "${PROXY,,}" in version.dll|winmm.dll) ;; *) echo "PROXY must be version.dll or winmm.dll"; exit 1;; esac
# The game exe: <Project>-Win64-Shipping.exe, else the largest exe (renamed builds, e.g. Xbox/GDK GoWEDay.exe).
EXE="$(ls "$WIN64" | grep -i -E -- '-Win64-(Shipping|Test)\.exe$' | head -1 || true)"
[ -n "$EXE" ] || EXE="$(ls -S "$WIN64" | grep -i -E '\.exe$' | head -1 || true)"
[ -n "$EXE" ] || { echo "no game exe in $WIN64"; exit 1; }
STEM="${EXE%.exe}"

if [ "${2:-}" = "remove" ]; then
  for p in version.dll winmm.dll; do
    if grep -aqs "GyroUE " "$WIN64/$p"; then rm -f "$WIN64/$p"; fi
  done
  rm -rf "$WIN64/GyroUE"; echo "GyroUE removed from $WIN64"; exit 0
fi
if [ -e "$WIN64/$PROXY" ] && ! grep -aqs "GyroUE " "$WIN64/$PROXY"; then
  echo "a foreign $PROXY already exists in $WIN64 (another mod?); not overwriting"; exit 1
fi
# Only one GyroUE proxy per folder: drop ours under the other name.
for p in version.dll winmm.dll; do
  if [ "${p,,}" != "${PROXY,,}" ] && grep -aqs "GyroUE " "$WIN64/$p"; then rm -f "$WIN64/$p"; echo "removed old GyroUE $p"; fi
done
mkdir -p "$WIN64/GyroUE/profiles"
cp "$ROOT/build/GyroUE/Release/version.dll" "$WIN64/$PROXY"
cp "$SDK/bin/gyrolib.dll" "$WIN64/GyroUE/gyrolib.dll"
# License notices that must ship with gyrolib.dll (GyroLib THIRD_PARTY.md), plus GyroUE's own license.
mkdir -p "$WIN64/GyroUE/licenses"
cp "$SDK/share/doc/GyroLib/licenses/"* "$WIN64/GyroUE/licenses/"
cp "$SDK/share/doc/GyroLib/sdl-changes/README-GyroLib.md" "$WIN64/GyroUE/licenses/SDL-CHANGES.md"
cp "$ROOT/LICENSE.txt" "$WIN64/GyroUE/licenses/GyroUE-LICENSE.txt"
cp "$ROOT/third_party/minhook-1.3.4/LICENSE.txt" "$WIN64/GyroUE/licenses/MinHook-LICENSE.txt"  # linked into version.dll
if [ -f "$ROOT/profiles/$STEM.ini" ]; then cp "$ROOT/profiles/$STEM.ini" "$WIN64/GyroUE/profiles/"; echo "profile: $STEM.ini"
else echo "profile: none (generic ControlRotation view)"; fi
echo "GyroUE deployed to $WIN64 as $PROXY for $EXE"
