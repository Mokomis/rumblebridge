#!/system/bin/sh
# KernelSU boot script (/data/adb/service.d/): starts rumblebridged at boot and again if it ever
# exits. The daemon itself waits for the Kishi, and through its sleep.
DIR=/data/adb/rumblebridge
: > "$DIR/log"
echo $$ > "$DIR/pid"
while true; do
  "$DIR/rumblebridged" > "$DIR/last-run" 2>&1
  status=$?
  echo "$(date '+%F %T') exit $status: $(tail -1 "$DIR/last-run")" >> "$DIR/log"
  sleep 3
done
