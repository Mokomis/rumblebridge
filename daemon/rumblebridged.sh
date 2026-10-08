#!/system/bin/sh
# KernelSU boot script (/data/adb/service.d/): keeps rumblebridged running whenever the Kishi is
# attached. The daemon exits when the Kishi is unplugged, or with status 2 when there is none.
DIR=/data/adb/rumblebridge
: > "$DIR/log"
echo $$ > "$DIR/pid"
while true; do
  "$DIR/rumblebridged" > "$DIR/last-run" 2>&1
  status=$?
  [ "$status" = 2 ] || { echo "$(date '+%F %T') exit $status: $(tail -1 "$DIR/last-run")" >> "$DIR/log"; }
  sleep 3
done
