#!/usr/bin/env bash
# Builds GyroUE and packs the installable release zip: build/release/GyroUE-v<version>.zip
# (version from project() in src/GyroUE/CMakeLists.txt). Extract the zip into the game's
# <Project>/Binaries/Win64 folder: it holds winmm.dll and the GyroUE folder, the same files
# tools/deploy-gyroue.sh installs, plus every profile and the user docs (README, TROUBLESHOOTING, ADVANCED, CREDITS). No gyrolib.ini: users keep theirs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/gyrolib-sdk.sh"; SDK="$GYROLIB_SDK"
VERSION="$(sed -nE 's/^project\(GyroUE VERSION ([0-9.]+).*/\1/p' "$ROOT/src/GyroUE/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "no version in project() of src/GyroUE/CMakeLists.txt"; exit 1; }

bash "$ROOT/tools/build-gyroue.sh"
DLL="$ROOT/build/GyroUE/Release/winmm.dll"
# The DLL must carry this exact version (its log line), so a stale build can't be shipped under a new name.
grep -aq "GyroUE $VERSION in " "$DLL" || { echo "$DLL is not version $VERSION (build failed?)"; exit 1; }

NAME="GyroUE-v$VERSION"
OUT="$ROOT/build/release"
STAGE="$OUT/$NAME"
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE/GyroUE/profiles" "$STAGE/GyroUE/licenses"
cp "$DLL" "$STAGE/winmm.dll"
cp "$SDK/bin/gyrolib.dll" "$STAGE/GyroUE/gyrolib.dll"
cp "$ROOT/profiles/"*.ini "$STAGE/GyroUE/profiles/"
# License notices that must ship with gyrolib.dll (GyroLib THIRD_PARTY.md), plus GyroUE's own license.
cp "$SDK/share/doc/GyroLib/licenses/"* "$STAGE/GyroUE/licenses/"
cp "$SDK/share/doc/GyroLib/sdl-changes/README-GyroLib.md" "$STAGE/GyroUE/licenses/SDL-CHANGES.md"
cp "$ROOT/LICENSE.txt" "$STAGE/GyroUE/licenses/GyroUE-LICENSE.txt"
cp "$ROOT/third_party/minhook-1.3.4/LICENSE.txt" "$STAGE/GyroUE/licenses/MinHook-LICENSE.txt"  # linked into winmm.dll
for doc in README TROUBLESHOOTING ADVANCED CREDITS; do cp "$ROOT/$doc.md" "$STAGE/GyroUE/$doc.md"; done

# Python's zipfile writes forward-slash paths (PowerShell 5.1's Compress-Archive writes backslashes).
python -I - "$(cygpath -w "$STAGE")" "$(cygpath -w "$OUT/$NAME.zip")" <<'EOF'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in sorted(os.walk(src)):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, src).replace(os.sep, "/"))
EOF
rm -rf "$STAGE"
echo "release: build/release/$NAME.zip"
python -I -c "import sys, zipfile; [print(f'  {i.file_size:>9}  {i.filename}') for i in zipfile.ZipFile(sys.argv[1]).infolist()]" "$(cygpath -w "$OUT/$NAME.zip")"
