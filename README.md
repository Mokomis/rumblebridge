# Rumblebridge

Rumble for the Razer Kishi V3 Pro on Android, for games and streaming clients that use ordinary controller vibration.

Android reports the Kishi as a controller with no rumble motors, so games have nowhere to send rumble. The Kishi's haptics sit on a separate USB interface with Razer's own protocol. Rumblebridge is a small root service that bridges the two.

**Status: one person's working setup, not a product.** It has run on a single tablet (OPPO Pad Mini OPD2515, ColorOS 16, KernelSU) with one Kishi V3 Pro on firmware 2.0.0.0. It runs as root and takes over the controller's input. Read [What is not proven](#what-is-not-proven) before installing it anywhere else.

## Why it was built

The Kishi V3 Pro has two haptic motors, and on Android ordinary controller rumble never reaches them. A game or streaming client asks Android to vibrate the controller, Android says the controller has no motors, and nothing happens. The Kishi's XInput mode is the usual fallback, but it needs a kernel option (`CONFIG_JOYSTICK_XPAD_FF`) that the tablet this was built on does not have.

Rumblebridge was written to get rumble in remote-play sessions and in PC games run through DroidDeck on that tablet, without patching any of the apps involved. Putting the fix under Android's controller layer means an app that already supports controller vibration works unchanged.

## How it works

`rumblebridged` runs whenever the Kishi is attached and does three things:

1. **Creates a virtual controller.** It grabs the Kishi's buttons and sticks and mirrors them onto a virtual gamepad, "Razer Kishi V3 Pro (rumble)", which Android sees as having motors. Apps use that pad and never see the difference.
2. **Collects rumble.** Anything an app plays on the virtual pad arrives here. So do packets on UDP `127.0.0.1:47811` from the optional DroidDeck hook (below).
3. **Plays it on the Kishi** as a stream of 30 ms haptic frames, and sends nothing when nothing is playing.

Measured on the tablet above: the virtual pad adds about 0.15 ms to controller input on average and about 1 ms at worst.

## Requirements

- **Root.** The service opens the Kishi's USB interface directly. Installed through `/data/adb/service.d/`, which KernelSU and Magisk both run at boot.
- **64-bit ARM Android** with a kernel that has `uinput` and force feedback (`CONFIG_INPUT_UINPUT`, `CONFIG_INPUT_FF_MEMLESS`).
- **Razer Kishi V3 Pro in HID mode.** XInput mode has no haptics interface. Other Kishi models are not matched.
- **To build:** `adb`, the Android NDK, and for the control app JDK 17 and the Android SDK build tools 35. The DroidDeck hook needs Zig.

If your device's kernel has rumble support for Xbox-style USB pads (`CONFIG_JOYSTICK_XPAD_FF`), try the Kishi's XInput mode first: it may rumble with no extra software.

## Install

```bash
daemon/build.sh && daemon/install.sh
app/build.sh && adb install -r app/build/rumblebridge.apk
```

`install.sh` copies the service to `/data/adb/rumblebridge/`, adds the boot script, and starts it. Set `ADB_SERIAL` if more than one device is attached. The app is optional.

## Using it

Attach the Kishi. The service starts by itself, at boot as well, and stops when the Kishi is unplugged. Open games after the Kishi is attached; an app already running may need restarting to see the new pad.

The **Rumblebridge** app is a control panel, not part of the rumble path. It shows the service's status and sets:

| Setting | What it does |
|---|---|
| Rumble | Mutes rumble without stopping the service |
| Strength | Overall level |
| Faint effects | The response curve: lower lifts quiet effects, 1.00 is proportional |
| Heavy / light motor level | Each motor's share |
| Heavy / light motor frequency | Where each motor plays; the defaults are the Kishi's strongest points |

Two buttons test rumble the way a game sends it and the way DroidDeck sends it. Settings are saved in the service and apply whether or not the app is open.

## Rules

- **Keep Razer Nexus closed while this runs.** Both want the haptics interface. If Android opens Nexus by itself whenever the Kishi is attached, it will start on every plug-in and every wake; clear that default or disable Nexus while you use this. To use Nexus, stop the service first, then replug the Kishi when you are done:

  ```bash
  adb shell 'su -c "kill \$(cat /data/adb/rumblebridge/pid); pkill -x rumblebridged"'    # stop
  adb shell 'su -c "nohup /data/adb/service.d/rumblebridged.sh >/dev/null 2>&1 &"'      # start again
  ```
- **The Kishi sleeps after fifteen minutes without input, and that is fine.** It drops off USB and returns when a button is pressed; Razer's own app sees the same. The service sends nothing while idle, stops when the Kishi leaves and starts again when it returns. Measured on 2026-10-08: asleep at fifteen minutes, back within seconds of a button press, rumble at full strength afterwards with no replug. An app may need a moment to pick the controller up again.
- **If rumble stops, unplug and replug the Kishi.** That resets its haptics, and the service restarts by itself.
- **Do not keep the Kishi awake with idle traffic.** An earlier version sent a silent frame every second. That kept the USB link up through the Kishi's sleep timer, and after about twenty minutes untouched the Kishi answered frames without vibrating until it was unplugged.
- **Never send the Kishi's haptics interface anything but stream frames.** A query or a setting on that interface (haptics mode and gain were tried) stops it playing frames until it is replugged. It still accepts them, but neither answers nor vibrates. `rumblebridged` only sends frames; this matters if you change the code.

## DroidDeck

[DroidDeck](https://github.com/Droid-Deck/DroidDeck) works without anything extra. It plays rumble on the active controller's motors when Android reports any, and the virtual pad has them. Checked on 2026-10-08 with DroidDeck 0.3.1 and no hook, in Risk of Rain 2: DroidDeck logged rumble as playing on "Razer Kishi V3 Pro (rumble)" and the Kishi answered every frame. DroidDeck picks the controller at its first input, so rumble before any button press goes nowhere.

If the controller is dead inside a DroidDeck session, stop the session, close DroidDeck fully and start again. This happened once, in a session started seconds after another was stopped.

**The hook in `hook/` is no longer needed.** It dates from before the virtual pad: a small preload library for DroidDeck's Linux session which copies each rumble event straight to the service. With both in place every event arrives twice; that combination has not been tested. To install it anyway:

```bash
hook/build.sh && hook/install.sh
```

This stages the library and a start-up script in the tablet's `Download/rumblebridge/` folder and adds one line to DroidDeck's `Download/droiddeck-env`. It takes effect at the next session. The script copies the library into DroidDeck's Linux files and records its hash there; a different file staged later is refused until you delete `/usr/local/lib/librumblehook.so.sha256` from a DroidDeck terminal.

`Download/` is shared storage. Any app with storage access can edit `droiddeck-env` or the start-up script and so run commands in the DroidDeck session; the hash only protects the library. That is a property of DroidDeck's `droiddeck-env` mechanism.

## Uninstall

```bash
daemon/uninstall.sh
adb uninstall com.rumblebridge
```

For the DroidDeck hook, delete `Download/rumblebridge/` and the `BASH_ENV` line in `Download/droiddeck-env`. DroidDeck rewrites its preload list at every session start, so the hook is gone from the next session. The copied library stays in DroidDeck's Linux files, unused, until you remove `/usr/local/lib/librumblehook.so*` there.

## What is not proven

- **Other devices, ROMs or root managers.** Only the one tablet above.
- **Other firmware.** The frame format and the frequencies were found on firmware 2.0.0.0.
- **The DroidDeck hook since the rename.** It builds, but DroidDeck no longer needs it and it has not been run under its new name.

## The haptics protocol, briefly

Details are in the comments of `daemon/rumblebridged.c`. This project is not affiliated with Razer.

- The haptics interface is HID, named "Razer HD Haptics Specifications" (interface 4, interrupt endpoints `0x04` out and `0x84` in).
- A frame goes out as output report 2: `[2][length from here on][0][fragment 1][command 0x0E][frame]`.
- A rumble frame is a 7-bit duration in ms, then for each of two channels one band of four (6-bit amplitude, 7-bit frequency) pairs, packed most significant bit first: 15 bytes.
- The Kishi answers every frame. The service reads each answer.
- Frequency index 10 (about 59 Hz) shakes hardest, about nine times index 0; index 35 (about 132 Hz) is a smaller peak.

## Layout

| Path | What it is |
|---|---|
| `daemon/` | `rumblebridged`, its boot script, install and uninstall |
| `app/` | The control panel, built without Gradle |
| `hook/` | The DroidDeck preload library and its start-up script, no longer needed |
| `probe.py` | Drives the control panel's test port over `adb`, including a shake reading from the tablet's motion sensors |
| `frames.py` | The frame format in Python, for experiments |

## Licence

MIT. See [LICENSE](LICENSE).
