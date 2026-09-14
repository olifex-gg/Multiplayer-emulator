#!/usr/bin/env bash
# Build the Windows host/join launcher. Run from the repository root.
#   Native on Windows (MSYS2 MINGW32):  bash launcher/build-windows.sh
#   Cross from Linux (mingw-w64):        CC=i686-w64-mingw32-gcc bash launcher/build-windows.sh
set -euo pipefail
CC=${CC:-gcc}
OUT=${1:-AnimalCrossingOnline.exe}
"$CC" -O2 -Wall -mwindows launcher/launcher.c -o "$OUT" -lws2_32 -lshell32 -luser32 -lgdi32
echo "built $OUT"
