# Sourced by bash scripts in the DroidDeck session (BASH_ENV in Download/droiddeck-env).
# Copies the hook into the Linux runtime, since Android's shared storage cannot map code, and
# adds it to the session's preload list, which DroidDeck rewrites at every session start.
#
# Download/ is Android shared storage: any app with storage access can write a .so there. So the
# install only trusts a staged .so the first time (pinning its hash into the Linux rootfs, which
# shared storage cannot touch); a later staged file that doesn't match the pin is logged and
# ignored rather than silently installed. To accept a rebuilt .so, remove $_rb_pin yourself
# from a DroidDeck terminal.
if [ -z "${RUMBLE_HOOK_DONE:-}" ]; then
  export RUMBLE_HOOK_DONE=1
  _rb_src=/root/Storage/Download/rumblebridge/librumblehook.so
  _rb_dst=/usr/local/lib/librumblehook.so
  _rb_pin=/usr/local/lib/librumblehook.so.sha256
  _rb_log=/root/Storage/Download/rumblebridge/hook.log
  if [ -f "$_rb_src" ]; then
    _rb_hash=$(sha256sum "$_rb_src" 2>/dev/null | cut -d' ' -f1)
    _rb_pinned=$(cat "$_rb_pin" 2>/dev/null)
    if [ -z "$_rb_pinned" ]; then
      if cp -f "$_rb_src" "$_rb_dst.new" 2>/dev/null && mv -f "$_rb_dst.new" "$_rb_dst" 2>/dev/null; then
        echo "$_rb_hash" > "$_rb_pin" 2>/dev/null
      fi
    elif [ "$_rb_hash" != "$_rb_pinned" ]; then
      echo "$(date +%T) staged librumblehook.so does not match the pinned hash - not installing. Remove $_rb_pin to accept it." >> "$_rb_log" 2>/dev/null
    fi
    if [ -f "$_rb_dst" ] && ! grep -qxF "$_rb_dst" /etc/ld.so.preload 2>/dev/null; then
      echo "$_rb_dst" >> /etc/ld.so.preload 2>/dev/null
    fi
    echo "$(date +%T) hook script ran: $(grep -c . /etc/ld.so.preload 2>/dev/null) preload lines" >> "$_rb_log" 2>/dev/null
  fi
  unset _rb_src _rb_dst _rb_pin _rb_log _rb_hash _rb_pinned
fi
