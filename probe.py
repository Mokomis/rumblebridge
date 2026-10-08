#!/usr/bin/env python3
"""Sends commands (one per argument) to the test panel's probe port over adb forward.
The panel must be on screen. Commands: vib MS | pad MS | udp STRONG WEAK MS | sleep MS"""
import os, socket, subprocess, sys
# ADB_SERIAL picks the device when more than one is attached.
adb = ["adb"] + (["-s", os.environ["ADB_SERIAL"]] if os.environ.get("ADB_SERIAL") else [])
subprocess.run(adb + ["forward", "tcp:47900", "tcp:47900"], capture_output=True)
with socket.create_connection(("127.0.0.1", 47900), timeout=8) as s:
    # The connect above is bounded. A reply can take as long as its command (vib 9000, sleep 9000),
    # but the tablet may also have frozen the panel, which never answers: so a long bound, not none.
    s.settimeout(60)
    f = s.makefile("rw")
    for cmd in sys.argv[1:]:
        f.write(cmd + "\n"); f.flush()
        print(cmd[:60], "->", f.readline().strip())
