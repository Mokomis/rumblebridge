#!/bin/bash
# Builds rumblebridged for the tablet with the Android NDK.
set -euo pipefail
cd "$(dirname "$0")"
NDK=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-commandlinetools/ndk/27.0.12077973}
"$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)" -O2 -Wall -Wextra -o rumblebridged rumblebridged.c -lm
ls -l rumblebridged
