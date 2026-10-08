#!/bin/bash
# Installs rumblebridged on the tablet as a KernelSU boot service and starts it now.
set -euo pipefail
cd "$(dirname "$0")"
# ADB_SERIAL picks the device when more than one is attached.
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
"${ADB[@]}" push rumblebridged /data/local/tmp/rumblebridged.new >/dev/null
"${ADB[@]}" push rumblebridged.sh /data/local/tmp/rumblebridged.sh.new >/dev/null
"${ADB[@]}" shell 'su -c "
  [ -f /data/adb/rumblebridge/pid ] && kill \$(cat /data/adb/rumblebridge/pid) 2>/dev/null
  pkill -x rumblebridged; sleep 1
  mkdir -p /data/adb/rumblebridge /data/adb/service.d
  mv /data/local/tmp/rumblebridged.new /data/adb/rumblebridge/rumblebridged
  mv /data/local/tmp/rumblebridged.sh.new /data/adb/service.d/rumblebridged.sh
  chown root:root /data/adb/rumblebridge/rumblebridged /data/adb/service.d/rumblebridged.sh
  chmod 755 /data/adb/rumblebridge/rumblebridged /data/adb/service.d/rumblebridged.sh
  rm -f /data/local/tmp/rumblebridged /data/local/tmp/rumblebridged.log
  (nohup /data/adb/service.d/rumblebridged.sh > /dev/null 2>&1 &)
"'
sleep 5
"${ADB[@]}" shell 'echo "daemon pid: $(pidof rumblebridged)"; cat /data/local/tmp/rumblebridged.status'
