#!/bin/bash
# Removes rumblebridged and its boot script from the tablet. The Kishi goes back to being a plain
# controller at once. Leaves the control app and the DroidDeck hook; see the README for those.
set -euo pipefail
# ADB_SERIAL picks the device when more than one is attached.
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
"${ADB[@]}" shell 'su -c "
  [ -f /data/adb/rumblebridge/pid ] && kill \$(cat /data/adb/rumblebridge/pid) 2>/dev/null
  pkill -x rumblebridged
  rm -rf /data/adb/rumblebridge /data/adb/service.d/rumblebridged.sh /data/local/tmp/rumblebridged.status
"'
echo "removed"
