#!/bin/bash
# Stages the DroidDeck hook on the tablet: the library and its start-up script go to
# Download/rumblebridge/, and Download/droiddeck-env gets the line that makes DroidDeck's session run the
# script. Takes effect at the next DroidDeck session.
set -euo pipefail
cd "$(dirname "$0")"
# ADB_SERIAL picks the device when more than one is attached.
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
"${ADB[@]}" shell mkdir -p /sdcard/Download/rumblebridge
"${ADB[@]}" push librumblehook.so rumble-hook.sh /sdcard/Download/rumblebridge/ >/dev/null
# droiddeck-env is DroidDeck's own file and may hold the user's other settings: add our line, keep theirs.
line=$(cat droiddeck-env)
"${ADB[@]}" shell "grep -qxF '$line' /sdcard/Download/droiddeck-env 2>/dev/null || echo '$line' >> /sdcard/Download/droiddeck-env"
echo "staged; start a new DroidDeck session"
