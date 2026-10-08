#!/bin/bash
# Builds librumblehook.so for DroidDeck's Linux session (64-bit ARM, glibc) with Zig.
set -euo pipefail
cd "$(dirname "$0")"
zig cc -target aarch64-linux-gnu.2.28 -shared -fPIC -O2 -Wall -Wextra -o librumblehook.so rumblehook.c
ls -l librumblehook.so
