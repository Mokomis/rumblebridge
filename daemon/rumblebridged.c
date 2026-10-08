// Gives the Razer Kishi V3 Pro rumble on Android. Runs as root from boot, Kishi or no Kishi.
//
// Android reports the Kishi as a gamepad with no motors: its haptics sit on a separate USB
// interface with Razer's own protocol. This
//  - grabs the Kishi's input device and mirrors it onto a uinput gamepad that also offers
//    FF_RUMBLE, so any app that rumbles "the controller" reaches us;
//  - listens on UDP 127.0.0.1:47811 for the DroidDeck hook's packets ("KR" + strong, weak,
//    duration ms, slot; little-endian u16), since DroidDeck never asks Android to rumble;
//  - plays whatever is asked for on the Kishi's "Razer HD Haptics Specifications" interface.
//
// What the Kishi's haptics interface tolerates, measured on firmware 2.0.0.0, 2026-10-07:
//  - Only stream frames (command 0x0E) may be sent. A query or a setting on this interface (mode
//    0x87, gain 0x88 and 0x08 were tried) stops it playing frames until it is unplugged.
//  - It answers every frame; Razer's SDK reads each answer, and so does this.
//  - Nothing is sent while idle, on purpose. The Kishi puts itself to sleep after fifteen minutes
//    without input: it drops off USB and comes back when a button is pressed (seen with Razer's
//    own app, which sends it nothing while idle). A once-a-second silent frame, tried here first,
//    kept the USB link up through that, and the Kishi was then found answering frames but not
//    vibrating until it was unplugged. So this lets it sleep, and takes it up again when it wakes.
//
// The virtual pad outlives the Kishi's sleep: while the Kishi is off USB but still plugged into
// the port, the pad stays, with every button released, so a game sees one controller throughout
// instead of a disconnect and a new controller. It goes when the Kishi is really unplugged.
//
// "razer_app=1" hands the Kishi to Razer's own app: this lets go of it entirely and enables that
// app; "razer_app=0" disables the app again and takes the Kishi back. Both want the haptics
// interface, so it is one or the other.
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/usbdevice_fs.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KISHI_VENDOR 0x1532
#define KISHI_PRODUCT_HID 0x0724  // HID mode; XInput mode is another product and has no haptics interface
#define HAPTICS_INTERFACE 4
#define HAPTICS_OUT 0x04
#define HAPTICS_IN 0x84
#define RUMBLE_PORT 47811
#define FRAME_MS 30  // rumble goes out as 30 ms frames, one per 30 ms
#define STATUS_MS 1000
#define MAX_EFFECTS 16
#define STATUS_FILE "/data/local/tmp/rumblebridged.status"
#define CONFIG_FILE "/data/adb/rumblebridge/config"
// Loopback UDP reaches any app with INTERNET permission, so "KC" (which persists settings) must
// prove it came from the control app: that app keeps a random token in its own private storage,
// readable by it and by us (root) but not by any other app, and appends the raw bytes to "KC"
// packets. "KR"/"KQ" stay open: the DroidDeck hook has no Android app identity to present, and a
// spoofed KR only plays a capped, transient effect rather than persisting anything.
#define TOKEN_FILE "/data/data/com.rumblebridge/files/token"
#define TOKEN_LEN 16
#define VIRTUAL_NAME "Razer Kishi V3 Pro (rumble)"
#define RAZER_APP "com.razer.bianca"
#define DEAF_FRAMES 10             // this many frames in a row unanswered, and the user is told
#define HOLD_GRACE_MS 5000         // a pad is never dropped sooner than this after the Kishi goes
#define HOLD_UNKNOWN_MS 3600000    // how long it is kept where the port cannot say what is plugged in
#define BITS_PER_LONG (8 * sizeof(long))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bits, n) (((bits)[(n) / BITS_PER_LONG] >> ((n) % BITS_PER_LONG)) & 1)

static volatile sig_atomic_t stopping;
static void on_signal(int sig) { (void)sig; stopping = 1; }

static long long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

// ---- what is playing ----

static pthread_mutex_t effect_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t effect_changed = PTHREAD_COND_INITIALIZER;
static int effect_low, effect_high;  // 0-255
static long long effect_until;
static unsigned long requests, frames, replies, failures;

// How long the Kishi's input takes to pass through here: from the kernel's timestamp on an event
// to the moment it has been written to the virtual pad. Microseconds, since the service started.
static const int LAT_EDGES[] = {50, 100, 200, 500, 1000, 2000, 5000};
#define LAT_BUCKETS (sizeof LAT_EDGES / sizeof LAT_EDGES[0] + 1)
static unsigned long lat_count, lat_bucket[LAT_BUCKETS];
static long long lat_sum_us, lat_max_us;

static void record_latency(long long us) {
  size_t b = 0;
  while (b < LAT_BUCKETS - 1 && us >= LAT_EDGES[b]) b++;
  lat_bucket[b]++;
  lat_count++;
  lat_sum_us += us;
  if (us > lat_max_us) lat_max_us = us;
}

// Settings, kept in CONFIG_FILE as "key=value" words and changed live by a "KC" packet from the
// control app. strength scales everything; curve bends the response (1 is proportional, lower
// lifts faint effects: 0.5 makes a tenth of full strength play at about a third); heavy and light
// scale each motor; the two frequencies are the frame's 7-bit index (about 30 Hz + 2.9 Hz a step).
//
// Frequency defaults were measured with the tablet's accelerometer at full strength, 2026-10-07:
// index 10 (about 59 Hz) shakes the tablet nine times as hard as index 0, which Razer's own rumble
// conversion uses; the response falls away above index 14, with a smaller peak at 35 (about
// 132 Hz, a third as strong). So the heavy motor plays at the strong peak and the light motor at
// the lesser one, as on a pad with two different motors.
static volatile int cfg_strength = 60, cfg_curve100 = 75, cfg_heavy = 100, cfg_light = 100;
static volatile int cfg_heavy_freq = 10, cfg_light_freq = 35, cfg_enabled = 1, cfg_razer_app = 0;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void apply_setting(const char *key, int v) {
  if (!strcmp(key, "strength")) cfg_strength = clamp(v, 0, 100);
  else if (!strcmp(key, "curve100")) cfg_curve100 = clamp(v, 30, 200);
  else if (!strcmp(key, "heavy")) cfg_heavy = clamp(v, 0, 100);
  else if (!strcmp(key, "light")) cfg_light = clamp(v, 0, 100);
  else if (!strcmp(key, "heavy_freq")) cfg_heavy_freq = clamp(v, 0, 127);
  else if (!strcmp(key, "light_freq")) cfg_light_freq = clamp(v, 0, 127);
  else if (!strcmp(key, "enabled")) cfg_enabled = v != 0;
  else if (!strcmp(key, "razer_app")) cfg_razer_app = v != 0;
}

static int print_settings(char *out, size_t len) {
  return snprintf(out, len, "strength=%d curve100=%d heavy=%d light=%d heavy_freq=%d light_freq=%d enabled=%d razer_app=%d",
                  cfg_strength, cfg_curve100, cfg_heavy, cfg_light, cfg_heavy_freq, cfg_light_freq, cfg_enabled,
                  cfg_razer_app);
}

static void read_config(void) {
  FILE *f = fopen(CONFIG_FILE, "r");
  if (!f) return;
  char key[32];
  int v;
  while (fscanf(f, " %31[a-z0-9_]=%d", key, &v) == 2) apply_setting(key, v);
  fclose(f);
}

static void save_config(void) {
  char line[240];
  int n = print_settings(line, sizeof line);
  int fd = open(CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  (void)!write(fd, line, n);
  (void)!write(fd, "\n", 1);
  close(fd);
}

/** Re-read on every "KC": the app may not have launched yet, or may have rotated the token. */
static int load_token(unsigned char *out) {
  int fd = open(TOKEN_FILE, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t n = read(fd, out, TOKEN_LEN);
  close(fd);
  return n == TOKEN_LEN;
}

/** A game's 16-bit motor strength as the Kishi's 0-255, for a motor scaled to [motor] percent. */
static int level(unsigned strength, int motor) {
  if (strength == 0 || !cfg_enabled) return 0;
  return (int)lround(255 * (cfg_strength / 100.0) * (motor / 100.0) * pow(strength / 65535.0, cfg_curve100 / 100.0));
}

static void play(unsigned strong, unsigned weak, unsigned ms) {
  pthread_mutex_lock(&effect_lock);
  requests++;
  effect_low = level(strong, cfg_heavy);
  effect_high = level(weak, cfg_light);
  effect_until = (effect_low || effect_high) && ms ? now_ms() + (ms > 5000 ? 5000 : ms) : 0;
  pthread_cond_signal(&effect_changed);
  pthread_mutex_unlock(&effect_lock);
}

// ---- the Kishi's haptics interface, over usbfs ----

static int read_sysfs_hex(const char *dir, const char *file, int base) {
  char path[300], buf[32] = "";
  snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/%s", dir, file);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  return n > 0 ? (int)strtol(buf, NULL, base) : -1;
}

static int open_haptics(void) {
  DIR *dir = opendir("/sys/bus/usb/devices");
  if (!dir) return -1;
  struct dirent *e;
  int fd = -1;
  while (fd < 0 && (e = readdir(dir))) {
    if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
    if (read_sysfs_hex(e->d_name, "idVendor", 16) != KISHI_VENDOR ||
        read_sysfs_hex(e->d_name, "idProduct", 16) != KISHI_PRODUCT_HID) continue;
    char path[64];
    snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", read_sysfs_hex(e->d_name, "busnum", 10),
             read_sysfs_hex(e->d_name, "devnum", 10));
    fd = open(path, O_RDWR | O_CLOEXEC);
  }
  closedir(dir);
  if (fd < 0) return -1;
  // The kernel's generic HID driver holds the interface; take it over. The gamepad is another one.
  struct usbdevfs_disconnect_claim claim = {.interface = HAPTICS_INTERFACE};
  if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &claim) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/**
 * One stream frame of two-motor rumble (Kishi firmware 2.0.0.0): a 7-bit duration in ms,
 * then per channel one band of four (6-bit amplitude, 7-bit frequency) pairs and no transients,
 * packed MSB first. It goes out as output report 2:
 * [2][length from here on][0][fragment 1][command 0x0E][frame].
 */
static void put_bits(uint8_t *out, int *bit, unsigned value, int count) {
  for (int i = count - 1; i >= 0; i--, (*bit)++)
    if ((value >> i) & 1) out[*bit / 8] |= 1 << (7 - *bit % 8);
}

static int send_frame(int usb, int low, int high) {
  uint8_t report[5 + 15] = {2, 4 + 15, 0, 1, 0x0E};
  int bit = 5 * 8;
  put_bits(report, &bit, FRAME_MS, 7);
  // Left plays the heavy motor and right the light one. A game that drives only one motor (No
  // Man's Sky drives only the heavy one) gets it on both sides rather than in one hand.
  int amps[2] = {low, high}, freqs[2] = {cfg_heavy_freq, cfg_light_freq};
  if (!high) amps[1] = low, freqs[1] = cfg_heavy_freq;
  if (!low) amps[0] = high, freqs[0] = cfg_light_freq;
  for (int c = 0; c < 2; c++) {
    put_bits(report, &bit, 1, 1);
    for (int i = 0; i < 4; i++) {
      put_bits(report, &bit, amps[c] * 63 / 255, 6);
      put_bits(report, &bit, freqs[c], 7);
    }
    put_bits(report, &bit, 0, 1);  // no further bands
    put_bits(report, &bit, 0, 1);  // no transients
  }
  struct usbdevfs_bulktransfer out = {.ep = HAPTICS_OUT, .len = sizeof report, .timeout = 100, .data = report};
  if (ioctl(usb, USBDEVFS_BULK, &out) < 0) return -1;
  uint8_t answer[64];
  struct usbdevfs_bulktransfer in = {.ep = HAPTICS_IN, .len = sizeof answer, .timeout = 30, .data = answer};
  int answered = ioctl(usb, USBDEVFS_BULK, &in) > 0;
#ifdef TEST_DEAF  // test builds only: a Kishi that plays but is reported as not answering
  if (access("/data/local/tmp/rumblebridge-test-deaf", F_OK) == 0) answered = 0;
#endif
  return answered;
}

// Where things stand. attached: the Kishi's buttons and haptics are open. Otherwise the virtual
// pad may be held for a sleeping Kishi, or Razer's app may have the Kishi, or there is none.
static volatile int attached, usb_lost;
static pthread_mutex_t usb_lock = PTHREAD_MUTEX_INITIALIZER;  // held to use or close usb_fd
static int usb_fd = -1, gamepad = -1, vpad = -1;

static const char *state_name(void) {
  return attached ? "attached" : cfg_razer_app ? "razer" : vpad >= 0 ? "held" : "waiting";
}

static int print_status(char *line, size_t len) {
  int n = snprintf(line, len, "state=%s requests=%lu frames=%lu replies=%lu failures=%lu playing=%d,%d\n",
                   state_name(), requests, frames, replies, failures, effect_until > now_ms() ? effect_low : 0,
                   effect_until > now_ms() ? effect_high : 0);
  n += snprintf(line + n, len - n, "input batches=%lu mean_us=%lld max_us=%lld under_us", lat_count,
                lat_count ? lat_sum_us / (long long)lat_count : 0, lat_max_us);
  for (size_t b = 0; b < LAT_BUCKETS - 1; b++) n += snprintf(line + n, len - n, " %d:%lu", LAT_EDGES[b], lat_bucket[b]);
  n += snprintf(line + n, len - n, " over:%lu\n", lat_bucket[LAT_BUCKETS - 1]);
  n += print_settings(line + n, len - n);
  n += snprintf(line + n, len - n, "\n");
  return n;
}

static void write_status(void) {
  char line[760];
  int n = print_status(line, sizeof line);
  int fd = open(STATUS_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  (void)!write(fd, line, n);
  close(fd);
}

/**
 * Puts a notification in the tablet's shade, replacing the last one from here. "cmd notification"
 * refuses root but serves the shell user, so the child becomes that first; and it must not be
 * handed this daemon's log file as its output (see set_razer_app).
 */
static void notify(const char *text) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0), dup2(null, 1), dup2(null, 2);
    if (setresgid(2000, 2000, 2000) < 0 || setresuid(2000, 2000, 2000) < 0) _exit(126);
    execl("/system/bin/cmd", "cmd", "notification", "post", "-t", "Rumblebridge", "rumblebridge", text, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  printf("notified: %s (status %d)\n", text, pid > 0 ? WEXITSTATUS(status) : -1);
}

static void *stream(void *arg) {
  (void)arg;
  int playing = 0, unanswered = 0, warned = 0;
  long long last_status = 0;
  while (!stopping) {
    pthread_mutex_lock(&effect_lock);
    if (!attached) effect_until = 0, playing = 0, unanswered = 0;  // nothing to play on
    if (!playing && effect_until <= now_ms()) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += STATUS_MS / 1000;
      pthread_cond_timedwait(&effect_changed, &effect_lock, &until);
    }
    int on = attached && effect_until > now_ms();
    int low = on ? effect_low : 0, high = on ? effect_high : 0;
    pthread_mutex_unlock(&effect_lock);
    if (!on && !playing) {  // idle: say nothing to the Kishi, so it can go to sleep
      write_status();
      continue;
    }
    // One silent frame ends an effect at once instead of letting the last frame run out.
    playing = on;
    long long start = now_ms();
    pthread_mutex_lock(&usb_lock);
    int r = usb_fd >= 0 ? send_frame(usb_fd, low, high) : -1;
    int gone = usb_fd < 0 || (r < 0 && errno == ENODEV);
    pthread_mutex_unlock(&usb_lock);
    if (r < 0) {
      failures++;
      if (gone) usb_lost = 1, playing = 0;  // asleep or unplugged; the main loop lets go of it
    } else {
      frames++;
      replies += r;
      // A healthy Kishi answers every frame. One that has stopped answering has stopped vibrating
      // too, and only unplugging it brings that back, so say so rather than leave it a mystery.
      unanswered = r ? 0 : unanswered + 1;
      if (unanswered == DEAF_FRAMES && !warned) {
        warned = 1;
        notify("Rumble is not reaching the Kishi. Unplug it and plug it back in.");
      } else if (r && warned) {
        warned = 0;
        notify("Rumble is reaching the Kishi again.");
      }
    }
    if (start - last_status >= 1000) {
      last_status = start;
      write_status();
    }
    long long spent = now_ms() - start;
    if (spent < FRAME_MS) usleep((FRAME_MS - spent) * 1000);
  }
  return NULL;
}

// ---- the Kishi's buttons and sticks, and the virtual pad ----

/**
 * The Kishi's gamepad node: a Razer USB device with a south button and a left stick. Not the
 * virtual pad, which carries the same identity and is still there while the Kishi sleeps.
 */
static int open_gamepad(char *path, size_t path_len, struct input_id *id) {
  DIR *dir = opendir("/dev/input");
  if (!dir) return -1;
  struct dirent *e;
  while ((e = readdir(dir))) {
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    snprintf(path, path_len, "/dev/input/%s", e->d_name);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long keys[NLONGS(KEY_CNT)] = {0}, abs[NLONGS(ABS_CNT)] = {0};
    char name[80] = "";
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    if (strcmp(name, VIRTUAL_NAME) != 0 && ioctl(fd, EVIOCGID, id) == 0 && id->vendor == KISHI_VENDOR && id->product == KISHI_PRODUCT_HID &&
        id->bustype == BUS_USB && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 &&
        TEST_BIT(keys, BTN_SOUTH) && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) >= 0 && TEST_BIT(abs, ABS_X)) {
      closedir(dir);
      return fd;
    }
    close(fd);
  }
  closedir(dir);
  return -1;
}

// What the virtual pad offers, and where each axis sits untouched, for releasing everything when
// the Kishi goes away with a button or a stick held.
static unsigned long pad_keys[NLONGS(KEY_CNT)], pad_abs[NLONGS(ABS_CNT)];
static int pad_rest[ABS_CNT];

static int make_virtual(int gamepad, const struct input_id *id) {
  int fd = open("/dev/uinput", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -1;
  unsigned long *keys = pad_keys, *abs = pad_abs;
  memset(pad_keys, 0, sizeof pad_keys);
  memset(pad_abs, 0, sizeof pad_abs);
  ioctl(gamepad, EVIOCGBIT(EV_KEY, sizeof pad_keys), keys);
  ioctl(gamepad, EVIOCGBIT(EV_ABS, sizeof pad_abs), abs);
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  for (int k = 0; k < KEY_CNT; k++)
    if (TEST_BIT(keys, k)) ioctl(fd, UI_SET_KEYBIT, k);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (int a = 0; a < ABS_CNT; a++) {
    if (!TEST_BIT(abs, a)) continue;
    struct uinput_abs_setup setup = {.code = a};
    if (ioctl(gamepad, EVIOCGABS(a), &setup.absinfo) < 0) continue;
    // A d-pad hat and a signed stick rest at 0 and a trigger at its minimum. Any other axis is
    // taken to be at rest now, as the Kishi has only just been plugged in or woken.
    pad_rest[a] = (a >= ABS_HAT0X && a <= ABS_HAT3Y) || setup.absinfo.minimum < 0 ? 0
                  : a == ABS_GAS || a == ABS_BRAKE ? setup.absinfo.minimum : setup.absinfo.value;
    ioctl(fd, UI_SET_ABSBIT, a);
    ioctl(fd, UI_ABS_SETUP, &setup);
  }
  ioctl(fd, UI_SET_EVBIT, EV_FF);
  ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
  // The Kishi's own vendor and product, so Android picks the same key layout for it.
  struct uinput_setup setup = {.id = *id, .ff_effects_max = MAX_EFFECTS};
  snprintf(setup.name, sizeof setup.name, VIRTUAL_NAME);
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/** Every button up and every axis at rest on the virtual pad. */
static void release_all(void) {
  if (vpad < 0) return;
  struct input_event ev[KEY_CNT / 8];
  size_t n = 0;
  for (int k = 0; k < KEY_CNT; k++) {
    if (!TEST_BIT(pad_keys, k)) continue;
    ev[n++] = (struct input_event){.type = EV_KEY, .code = k, .value = 0};
    if (n == sizeof ev / sizeof ev[0] - 1) break;
  }
  (void)!write(vpad, ev, n * sizeof ev[0]);
  n = 0;
  for (int a = 0; a < ABS_CNT; a++)
    if (TEST_BIT(pad_abs, a)) ev[n++] = (struct input_event){.type = EV_ABS, .code = a, .value = pad_rest[a]};
  ev[n++] = (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
  (void)!write(vpad, ev, n * sizeof ev[0]);
}

static void drop_virtual(void) {
  if (vpad < 0) return;
  ioctl(vpad, UI_DEV_DESTROY);
  close(vpad);
  vpad = -1;
  printf("virtual pad removed\n");
}

/** Takes up a Kishi that is on USB: its haptics, its buttons, and the virtual pad if there is none yet. */
static void attach(void) {
  static char said[160];
  char path[64], problem[160] = "";
  struct input_id id;
  int g = open_gamepad(path, sizeof path, &id);
  if (g < 0) return;
  int u = open_haptics();
  if (u < 0) snprintf(problem, sizeof problem, "could not take the Kishi's haptics interface: %s", strerror(errno));
  if (!*problem && vpad < 0 && (vpad = make_virtual(g, &id)) < 0)
    snprintf(problem, sizeof problem, "could not create the virtual pad: %s", strerror(errno));
  // Event timestamps on the monotonic clock, to time the pass-through against.
  int clock_id = CLOCK_MONOTONIC;
  ioctl(g, EVIOCSCLOCKID, &clock_id);
  // From here Android hears the Kishi only through the virtual pad.
  if (!*problem && ioctl(g, EVIOCGRAB, 1) < 0) snprintf(problem, sizeof problem, "could not grab %s: %s", path, strerror(errno));
  if (*problem) {
    if (strcmp(problem, said)) fprintf(stderr, "%s\n", problem);  // once, not at every retry
    snprintf(said, sizeof said, "%s", problem);
    if (u >= 0) close(u);
    close(g);
    return;
  }
  said[0] = 0;
  pthread_mutex_lock(&usb_lock);
  usb_fd = u;
  pthread_mutex_unlock(&usb_lock);
  gamepad = g;
  usb_lost = 0;
  attached = 1;
  printf("Kishi on %s: virtual pad with rumble is up\n", path);
}

/** Lets go of the Kishi (asleep, unplugged, or wanted by Razer's app). The virtual pad stays. */
static void detach(const char *why) {
  attached = 0;
  pthread_mutex_lock(&usb_lock);
  close(usb_fd);
  usb_fd = -1;
  pthread_mutex_unlock(&usb_lock);
  ioctl(gamepad, EVIOCGRAB, 0);
  close(gamepad);
  gamepad = -1;
  release_all();
  printf("let go of the Kishi: %s\n", why);
}

/** Whether anything is plugged into a USB-C port: 1, 0, or -1 where the kernel does not say. */
static int port_occupied(void) {
  DIR *dir = opendir("/sys/class/typec");
  if (!dir) return -1;
  struct dirent *e;
  int found = 0;
  while (!found && (e = readdir(dir))) {
    size_t n = strlen(e->d_name);
    found = n > 8 && !strcmp(e->d_name + n - 8, "-partner");
  }
  closedir(dir);
  return found;
}

/** Enables or disables Razer's app, as "cmd package" would from a root shell. */
static void set_razer_app(int on) {
  pid_t pid = fork();
  if (pid == 0) {
    // "cmd" hands its own stdin, stdout and stderr to the package service, which may not use this
    // daemon's log file: the call then fails outright. /dev/null it accepts.
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0), dup2(null, 1), dup2(null, 2);
    if (on) execl("/system/bin/cmd", "cmd", "package", "enable", RAZER_APP, (char *)NULL);
    else execl("/system/bin/cmd", "cmd", "package", "disable-user", "--user", "0", RAZER_APP, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  printf("Razer's app %s (status %d)\n", on ? "enabled" : "disabled", pid > 0 ? WEXITSTATUS(status) : -1);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  read_config();
  int razer_app = cfg_razer_app;  // as last applied; the app's state is left alone at start-up
  int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(RUMBLE_PORT)};
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(udp, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "UDP %d is taken: %s\n", RUMBLE_PORT, strerror(errno));
    return 1;
  }
  // Wakes the loop the moment an input device appears, so a waking Kishi is taken up at once.
  int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  inotify_add_watch(watch, "/dev/input", IN_CREATE | IN_ATTRIB);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, SIG_IGN);
  pthread_t streamer;
  pthread_create(&streamer, NULL, stream, NULL);
  // The input pass-through runs at real-time priority so a busy game cannot delay a button press.
  // The streaming thread above keeps normal priority.
  struct sched_param rt = {.sched_priority = 2};
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt) != 0) fprintf(stderr, "no real-time priority for input\n");
  printf("rumblebridged is up\n");

  struct ff_effect effects[MAX_EFFECTS] = {0};
  struct input_event ev[64];
  long long next_try = 0, eager_until = 0, detached_at = now_ms();
  while (!stopping) {
    if (cfg_razer_app != razer_app) {
      razer_app = cfg_razer_app;
      if (razer_app && attached) detach("handed to Razer's app");
      if (razer_app) drop_virtual();
      set_razer_app(razer_app);
      next_try = 0;
    }
    if (attached && usb_lost) detach("it left USB"), detached_at = now_ms();
    if (!attached && !razer_app) {
      long long now = now_ms();
      if (now >= next_try) {
        // Often while a device has just appeared (its USB node can lag its input node), else rarely.
        next_try = now + (now < eager_until ? 200 : 5000);
        attach();
      }
      // A sleeping Kishi is still in the port; an unplugged one is not, and its pad goes with it.
      if (!attached && vpad >= 0 && now - detached_at > HOLD_GRACE_MS) {
        int occupied = port_occupied();
        if (occupied == 0 || (occupied < 0 && now - detached_at > HOLD_UNKNOWN_MS)) drop_virtual();
      }
    }
    struct pollfd fds[4] = {{.fd = gamepad, .events = POLLIN}, {.fd = vpad, .events = POLLIN},
                            {.fd = udp, .events = POLLIN}, {.fd = watch, .events = POLLIN}};
    if (poll(fds, 4, attached ? 500 : 1000) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[3].revents & POLLIN) {
      char skip[1024];
      while (read(watch, skip, sizeof skip) > 0) {}
      next_try = 0;
      eager_until = now_ms() + 5000;
    }
    if (fds[0].revents & POLLIN) {
      ssize_t n = read(gamepad, ev, sizeof ev);
      if (n <= 0) {
        detach("it left USB"), detached_at = now_ms();
        continue;
      }
      if (write(vpad, ev, n) < 0) break;
      struct timespec t;
      clock_gettime(CLOCK_MONOTONIC, &t);
      record_latency((t.tv_sec - ev[0].time.tv_sec) * 1000000LL + (t.tv_nsec / 1000 - ev[0].time.tv_usec));
    } else if (fds[0].revents & (POLLERR | POLLHUP)) {
      detach("it left USB"), detached_at = now_ms();
      continue;
    }
    if (fds[1].revents & POLLIN) {
      ssize_t n = read(vpad, ev, sizeof ev);
      for (ssize_t i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
        if (ev[i].type == EV_UINPUT && ev[i].code == UI_FF_UPLOAD) {
          struct uinput_ff_upload up = {.request_id = ev[i].value};
          if (ioctl(vpad, UI_BEGIN_FF_UPLOAD, &up) < 0) continue;
          if (up.effect.id >= 0 && up.effect.id < MAX_EFFECTS) effects[up.effect.id] = up.effect;
          up.retval = 0;
          ioctl(vpad, UI_END_FF_UPLOAD, &up);
        } else if (ev[i].type == EV_UINPUT && ev[i].code == UI_FF_ERASE) {
          struct uinput_ff_erase er = {.request_id = ev[i].value};
          if (ioctl(vpad, UI_BEGIN_FF_ERASE, &er) < 0) continue;
          er.retval = 0;
          ioctl(vpad, UI_END_FF_ERASE, &er);
        } else if (ev[i].type == EV_FF && ev[i].code < MAX_EFFECTS) {
          const struct ff_effect *e = &effects[ev[i].code];
          if (ev[i].value && e->type == FF_RUMBLE)
            // A length of 0 means "until stopped"; play() caps one request at five seconds.
            play(e->u.rumble.strong_magnitude, e->u.rumble.weak_magnitude, e->replay.length ? e->replay.length : 5000);
          else
            play(0, 0, 0);
        }
      }
    }
    if (fds[2].revents & POLLIN) {
      // "KR" plays (the DroidDeck hook, and the control app's test); "KC key=value ..." changes
      // settings; "KQ" asks for the status text. The last two are the control app's. "KC" also
      // carries the control app's token as trailing raw bytes; see TOKEN_FILE above.
      char p[256];
      struct sockaddr_in from;
      socklen_t from_len = sizeof from;
      ssize_t n = recvfrom(udp, p, sizeof p - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
      const uint8_t *u = (const uint8_t *)p;
      if (n >= 10 && p[0] == 'K' && p[1] == 'R') play(u[2] | u[3] << 8, u[4] | u[5] << 8, u[6] | u[7] << 8);
      if (n >= 2 && p[0] == 'K' && p[1] == 'C') {
        unsigned char token[TOKEN_LEN];
        if ((size_t)n >= 2 + TOKEN_LEN && load_token(token) && !memcmp(p + n - TOKEN_LEN, token, TOKEN_LEN)) {
          int body_len = n - TOKEN_LEN;
          p[body_len] = 0;
          char key[32];
          int v, used;
          for (const char *at = p + 2; sscanf(at, " %31[a-z0-9_]=%d%n", key, &v, &used) == 2; at += used) apply_setting(key, v);
          save_config();
          play(0, 0, 0);  // nothing keeps playing at the old settings
        }
      }
      if (n >= 2 && p[0] == 'K' && (p[1] == 'Q' || p[1] == 'C')) {
        char status[760];
        int len = print_status(status, sizeof status);
        sendto(udp, status, len, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&from, from_len);
      }
    }
  }
  stopping = 1;
  pthread_mutex_lock(&effect_lock);
  pthread_cond_signal(&effect_changed);
  pthread_mutex_unlock(&effect_lock);
  pthread_join(streamer, NULL);
  if (attached) detach("stopping");
  drop_virtual();
  unlink(STATUS_FILE);
  return 0;
}
