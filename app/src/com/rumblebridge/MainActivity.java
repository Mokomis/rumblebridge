package com.rumblebridge;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.VibratorManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * The control panel for rumblebridged (daemon/), which does the work: shows its status, changes
 * its settings live and tests it. Everything goes over the daemon's UDP port on this tablet. A
 * probe port lets a test script rumble the Kishi and read how much the tablet shook; it and the
 * status only work while this screen is showing, since the tablet freezes the app otherwise.
 */
public final class MainActivity extends Activity {
    private static final String TAG = "Rumblebridge";
    static final int PROBE_PORT = 47900;
    static final int DAEMON_PORT = 47811;

    /** A setting the daemon knows by [key], shown as a slider from [min] to [max]. */
    private final class Setting {
        final String key;
        final int min, max;
        final java.util.function.IntFunction<String> describe;
        TextView label;
        SeekBar bar;

        Setting(String key, int min, int max, java.util.function.IntFunction<String> describe) {
            this.key = key;
            this.min = min;
            this.max = max;
            this.describe = describe;
        }

        void show(int value) {
            label.setText(describe.apply(value));
            if (!dragging) bar.setProgress(value - min);
        }
    }

    private final Setting[] settings = {
        new Setting("strength", 0, 100, v -> "Strength: " + v + "%"),
        new Setting("curve100", 30, 150, v -> "Faint effects: " + (v < 60 ? "lifted a lot" : v < 90 ? "lifted" : v <= 110 ? "as the game sends them" : "reduced")
            + " (curve " + String.format("%.2f", v / 100.0) + ")"),
        new Setting("heavy", 0, 100, v -> "Heavy motor level: " + v + "%"),
        new Setting("light", 0, 100, v -> "Light motor level: " + v + "%"),
        new Setting("heavy_freq", 0, 127, v -> "Heavy motor frequency: about " + hz(v) + " Hz"),
        new Setting("light_freq", 0, 127, v -> "Light motor frequency: about " + hz(v) + " Hz"),
    };
    private static final String DEFAULTS = "strength=60 curve100=75 heavy=100 light=100 heavy_freq=10 light_freq=35 enabled=1";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Runnable pollRunnable = this::poll;
    private TextView status;
    private Switch enabled;
    private volatile boolean dragging;
    private boolean showing;
    private volatile ServerSocket probe;
    private byte[] token;

    private static int hz(int index) { return Math.round(30 + index * 370f / 127); }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(64, 48, 64, 48);

        status = new TextView(this);
        status.setTextSize(16);
        status.setText("Looking for the rumble service…");
        page.addView(status);

        enabled = new Switch(this);
        enabled.setText("Rumble");
        enabled.setTextSize(20);
        enabled.setPadding(0, 32, 0, 16);
        enabled.setOnClickListener(v -> send("KC enabled=" + (enabled.isChecked() ? 1 : 0)));
        page.addView(enabled);

        for (Setting s : settings) {
            s.label = new TextView(this);
            s.label.setTextSize(16);
            s.label.setPadding(0, 24, 0, 0);
            s.bar = new SeekBar(this);
            s.bar.setMax(s.max - s.min);
            s.bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                    if (fromUser) s.label.setText(s.describe.apply(progress + s.min));
                }
                @Override public void onStartTrackingTouch(SeekBar bar) { dragging = true; }
                @Override public void onStopTrackingTouch(SeekBar bar) {
                    dragging = false;
                    send("KC " + s.key + "=" + (bar.getProgress() + s.min));
                }
            });
            page.addView(s.label);
            page.addView(s.bar);
        }

        page.addView(button("Test rumble as a game would", () -> toast(rumbleController(1000))));
        page.addView(button("Test rumble as DroidDeck would", () -> sendRumble(0xFFFF, 0xFFFF, 1000)));
        page.addView(button("Reset to defaults", () -> send("KC " + DEFAULTS)));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        // Keeps the page clear of the status and navigation bars; apps targeting Android 15 draw under them.
        scroll.setFitsSystemWindows(true);
        if (getActionBar() != null) getActionBar().hide();
        TextView title = new TextView(this);
        title.setText("Rumblebridge");
        title.setTextSize(26);
        title.setPadding(0, 0, 0, 24);
        page.addView(title, 0);
        setContentView(scroll);
        new Thread(this::serveProbe, "probe").start();
    }

    @Override protected void onResume() {
        super.onResume();
        showing = true;
        poll();
    }

    @Override protected void onPause() {
        showing = false;
        main.removeCallbacks(pollRunnable);
        super.onPause();
    }

    @Override protected void onDestroy() {
        try { if (probe != null) probe.close(); } catch (Exception e) { /* closing */ }
        super.onDestroy();
    }

    private Button button(String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextSize(18);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = 24;
        b.setLayoutParams(lp);
        return b;
    }

    private void toast(String text) {
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show();
    }

    /** Asks for the status once a second while this screen shows. */
    private void poll() {
        if (!showing) return;
        send("KQ");
        main.postDelayed(pollRunnable, 1000);
    }

    /** The daemon (root) can read this across the app sandbox; no other app can read or write it. */
    private synchronized byte[] authToken() {
        if (token != null) return token;
        byte[] t = new byte[16];
        try {
            java.io.File f = new java.io.File(getFilesDir(), "token");
            if (f.exists()) {
                try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
                    if (in.read(t) != t.length) throw new java.io.IOException("short token file");
                }
            } else {
                new java.security.SecureRandom().nextBytes(t);
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) { out.write(t); }
            }
        } catch (Exception e) {
            Log.w(TAG, "auth token", e);
            t = new byte[0];
        }
        token = t;
        return token;
    }

    /** Sends one message to the daemon and shows the status it answers with, or that it did not. */
    private void send(String message) {
        new Thread(() -> {
            String reply = null;
            try (DatagramSocket s = new DatagramSocket()) {
                byte[] text = message.getBytes(StandardCharsets.US_ASCII);
                byte[] out = text;
                if (message.startsWith("KC")) {
                    byte[] t = authToken();
                    out = new byte[text.length + t.length];
                    System.arraycopy(text, 0, out, 0, text.length);
                    System.arraycopy(t, 0, out, text.length, t.length);
                }
                s.send(new DatagramPacket(out, out.length, InetAddress.getByAddress(new byte[] {127, 0, 0, 1}), DAEMON_PORT));
                byte[] in = new byte[1024];
                DatagramPacket p = new DatagramPacket(in, in.length);
                s.setSoTimeout(400);
                s.receive(p);
                reply = new String(in, 0, p.getLength(), StandardCharsets.US_ASCII);
            } catch (Exception e) {
                // no answer: the daemon is not running, which it is not without the Kishi attached
            }
            final String text = reply;
            main.post(() -> show(text));
        }, "daemon").start();
    }

    private void show(String reply) {
        View[] controls = {enabled};
        if (reply == null) {
            status.setText("The rumble service is not answering.\nIt runs only while the Kishi is attached, in HID mode.");
            for (View v : controls) v.setEnabled(false);
            for (Setting s : settings) s.bar.setEnabled(false);
            return;
        }
        Map<String, String> v = new HashMap<>();
        for (String word : reply.split("\\s+")) {
            int eq = word.indexOf('=');
            if (eq > 0) v.put(word.substring(0, eq), word.substring(eq + 1));
        }
        long frames = number(v, "frames"), replies = number(v, "replies");
        status.setText("Kishi attached, rumble service running.\n"
            + number(v, "requests") + " rumble requests; the Kishi answered " + replies + " of " + frames + " frames"
            + (number(v, "failures") > 0 ? " (" + number(v, "failures") + " failed)" : "") + ".\n"
            + "Controller pass-through: " + number(v, "mean_us") + " µs on average, " + number(v, "max_us") + " µs at worst.");
        for (View c : controls) c.setEnabled(true);
        enabled.setChecked(number(v, "enabled") != 0);
        for (Setting s : settings) {
            s.bar.setEnabled(true);
            if (v.containsKey(s.key)) s.show((int) number(v, s.key));
        }
    }

    private static long number(Map<String, String> v, String key) {
        try { return Long.parseLong(v.getOrDefault(key, "0")); } catch (NumberFormatException e) { return 0; }
    }

    /** Asks Android to rumble a controller, the way a game does. Only the daemon's virtual pad has a vibrator. */
    private String rumbleController(int ms) {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || (d.getSources() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD) continue;
            VibratorManager vm = d.getVibratorManager();
            if (vm.getVibratorIds().length == 0) continue;
            vm.getDefaultVibrator().vibrate(VibrationEffect.createOneShot(ms, 255));
            return "Rumbling " + d.getName();
        }
        return "No controller with rumble. Is the Kishi attached?";
    }

    /** One rumble request in the DroidDeck hook's format. */
    private static void sendRumble(int strong, int weak, int ms) {
        new Thread(() -> {
            byte[] p = {'K', 'R', (byte) strong, (byte) (strong >> 8), (byte) weak, (byte) (weak >> 8), (byte) ms, (byte) (ms >> 8), 0, 0};
            try (DatagramSocket s = new DatagramSocket()) {
                s.send(new DatagramPacket(p, p.length, InetAddress.getByAddress(new byte[] {127, 0, 0, 1}), DAEMON_PORT));
            } catch (Exception e) {
                Log.w(TAG, "test rumble", e);
            }
        }).start();
    }

    private void serveProbe() {
        try (ServerSocket s = new ServerSocket(PROBE_PORT, 4, InetAddress.getLoopbackAddress())) {
            probe = s;
            while (true) {
                try (Socket c = s.accept()) {
                    BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
                    PrintWriter w = new PrintWriter(c.getOutputStream(), true);
                    String line;
                    while ((line = r.readLine()) != null) {
                        try { w.println(run(line.trim().split("\\s+"))); } catch (Exception e) { w.println("error " + e); }
                    }
                } catch (Exception e) {
                    if (s.isClosed()) return;
                    Log.w(TAG, "probe client", e);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "probe server", e);
        }
    }

    /** vib MS | pad MS | udp STRONG WEAK MS | sleep MS */
    private String run(String[] a) throws Exception {
        switch (a[0]) {
            case "vib": return Shake.measure(this, Integer.parseInt(a[1]));
            case "pad": return "pad " + rumbleController(Integer.parseInt(a[1]));
            case "udp": sendRumble(Integer.parseInt(a[1]), Integer.parseInt(a[2]), Integer.parseInt(a[3])); return "udp sent";
            case "sleep": Thread.sleep(Integer.parseInt(a[1])); return "slept";
            default: return "unknown command";
        }
    }
}
