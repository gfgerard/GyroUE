#!/usr/bin/env bash
# Builds GyroUE and its unit tests, then runs the tests (tests/profile_tests.cpp: game profile, Match=Camera choice).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/tools/build-gyroue.sh"
"$ROOT/build/GyroUE/Release/gyroue_tests.exe"
