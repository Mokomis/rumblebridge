// Preloaded into DroidDeck's Linux session. libfakeinput sends each rumble effect to the app on
// the abstract socket "droiddeck-rumble"; this copies the same 8 bytes (strong, weak, duration ms,
// pad slot; little-endian u16) to UDP 127.0.0.1:KISHI_PORT, where rumblebridged plays them
// on the Kishi. DroidDeck's own path is left as it is.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define KISHI_PORT 47811
static const char RUMBLE_NAME[] = "droiddeck-rumble";

static __thread int rumble_fd = -1;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
  const char *path = getenv("RUMBLE_HOOK_LOG");
  if (!path || !*path) return;
  int saved = errno;
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    char line[256];
    va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof line) n = sizeof line;
    if (n > 0) (void)!write(fd, line, n);
    close(fd);
  }
  errno = saved;
}

static void relay(const void *buf, size_t len) {
  int saved = errno;
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (s >= 0) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(KISHI_PORT);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint8_t packet[2 + 8] = {'K', 'R'};
    memcpy(packet + 2, buf, len < 8 ? len : 8);
    sendto(s, packet, sizeof packet, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&to, sizeof to);
    close(s);
  }
  errno = saved;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  static int (*real)(int, const struct sockaddr *, socklen_t);
  if (!real) real = dlsym(RTLD_NEXT, "connect");
  const size_t n = sizeof RUMBLE_NAME - 1;
  if (addr && addr->sa_family == AF_UNIX && len == offsetof(struct sockaddr_un, sun_path) + 1 + n) {
    const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
    if (un->sun_path[0] == 0 && memcmp(un->sun_path + 1, RUMBLE_NAME, n) == 0) {
      int r = real(fd, addr, len);
      // Kept even when the app is not listening: the Kishi can still play it.
      rumble_fd = fd;
      if (r < 0) {
        // libfakeinput gives up here and never calls send(); nothing to relay without the data.
        logf_("%d rumble connect failed errno=%d\n", getpid(), errno);
        rumble_fd = -1;
      }
      return r;
    }
  }
  return real(fd, addr, len);
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
  static ssize_t (*real)(int, const void *, size_t, int);
  if (!real) real = dlsym(RTLD_NEXT, "send");
  if (fd >= 0 && fd == rumble_fd) {
    rumble_fd = -1;
    if (len == 8 && buf) {
      const uint16_t *v = buf;
      logf_("%d rumble strong=%u weak=%u ms=%u slot=%u\n", getpid(), v[0], v[1], v[2], v[3]);
      relay(buf, len);
    }
  }
  return real(fd, buf, len, flags);
}

extern char *program_invocation_short_name;

__attribute__((constructor)) static void loaded(void) {
  const char *debug = getenv("RUMBLE_HOOK_DEBUG");
  if (debug && *debug == '1') logf_("%d loaded in %s\n", getpid(), program_invocation_short_name);
}
