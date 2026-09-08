/* Script function and purpose: saberctl(1) -- the command-line half of the
panel's control socket, and its own binary.

hikari.conf binds keys to `exec` strings, so a keybinding cannot reach the
resident panel except by launching something that talks to it (D-008). That
makes this program run on every bound keypress, which is the whole reason it
links nothing but libc: no Wayland, no cairo, no pango, no gdk-pixbuf, no
librsvg, and no glib either. It sends one line and prints one response. */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <saber/ipc.h>
#include <saber/saber.h>

static void
usage(FILE *stream)
{
  fprintf(stream,
      "usage: saberctl [-h] [-v] <command> [argument]\n"
      "\n"
      "  -h, --help      this message\n"
      "  -v, --version   version only\n"
      "\n"
      "commands:\n"
      "  dash                  toggle the application grid\n"
      "  spread [app_id]       toggle the window spread, optionally filtered\n"
      "  launch <1-10>         launch or focus favourite N\n"
      "  overlay <on|off>      the hold-Super number overlay\n"
      "  show                  reveal the panel\n"
      "  hide                  conceal the panel\n"
      "  toggle                toggle panel visibility\n"
      "  sheet <0-9>           switch to sheet N\n"
      "  pin <0-9>             send the focused window to sheet N\n"
      "  reload                re-read the configuration\n"
      "  status                report panel state, terminated by END\n"
      "  quit                  exit the panel\n"
      "\n"
      "Exits 0 when the panel answers `ok`, 1 otherwise.\n");
}

/* Function purpose: Join the argument vector into the single request line the
protocol expects, refusing over-length input here rather than letting the panel
close the connection on it -- the diagnostic is far clearer on this side. */
static bool
build_request(int argc, char **argv, char *request, size_t size)
{
  size_t len = 0;

  for (int i = 1; i < argc; i++) {
    int n = snprintf(
        request + len, size - len, "%s%s", i > 1 ? " " : "", argv[i]);

    if (n < 0 || (size_t)n >= size - len) {
      return false;
    }
    len += (size_t)n;
  }

  /* One byte for the newline; the panel's cap counts it. */
  if (len + 1 >= size) {
    return false;
  }

  request[len++] = '\n';
  request[len] = '\0';

  return true;
}

static bool
write_all(int fd, const char *buf, size_t len)
{
  size_t off = 0;

  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);

    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }

  return true;
}

/* Function purpose: Resolve the socket and connect, translating the two errnos
that mean "there is no panel" into one plain sentence. A raw errno is no help to
someone who has just pressed a key and seen nothing happen. */
static int
connect_to_panel(void)
{
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");

  if (runtime_dir == NULL || runtime_dir[0] == '\0') {
    fprintf(stderr,
        "saberctl: XDG_RUNTIME_DIR is not set, so there is no panel socket to "
        "find.\n");
    return -1;
  }

  struct sockaddr_un addr = { 0 };

  addr.sun_family = AF_UNIX;

  int n = snprintf(addr.sun_path,
      sizeof(addr.sun_path),
      "%s/%s",
      runtime_dir,
      SABER_IPC_SOCKET_NAME);

  if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
    fprintf(stderr,
        "saberctl: XDG_RUNTIME_DIR is too long for a unix socket path.\n");
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

  if (fd < 0) {
    fprintf(stderr, "saberctl: cannot create a socket: %s\n", strerror(errno));
    return -1;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    return fd;
  }

  if (errno == ENOENT) {
    fprintf(stderr, "saberctl: no panel running (%s does not exist).\n",
        addr.sun_path);
  } else if (errno == ECONNREFUSED) {
    fprintf(stderr,
        "saberctl: no panel running (%s is a stale socket left by an unclean "
        "exit).\n",
        addr.sun_path);
  } else {
    fprintf(stderr,
        "saberctl: cannot reach the panel at %s: %s\n",
        addr.sun_path,
        strerror(errno));
  }

  close(fd);

  return -1;
}

int
main(int argc, char **argv)
{
  if (argc > 1 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    usage(stdout);
    return EXIT_SUCCESS;
  }

  if (argc > 1 &&
      (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
    printf("saberctl %s\n", SABER_VERSION);
    return EXIT_SUCCESS;
  }

  if (argc < 2) {
    usage(stderr);
    return EXIT_FAILURE;
  }

  if (argv[1][0] == '-') {
    fprintf(stderr, "saberctl: unknown option: %s\n", argv[1]);
    usage(stderr);
    return EXIT_FAILURE;
  }

  /* One byte over the wire limit: the panel counts the newline within its 512,
  so the longest legal request is 511 characters plus it, and the terminator
  this buffer also has to hold is not sent. */
  char request[SABER_IPC_MAX_REQUEST + 1];

  if (!build_request(argc, argv, request, sizeof(request))) {
    fprintf(stderr,
        "saberctl: request is longer than the %d byte limit.\n",
        SABER_IPC_MAX_REQUEST);
    return EXIT_FAILURE;
  }

  int fd = connect_to_panel();

  if (fd < 0) {
    return EXIT_FAILURE;
  }

  if (!write_all(fd, request, strlen(request))) {
    fprintf(stderr, "saberctl: could not send the request: %s\n",
        strerror(errno));
    close(fd);
    return EXIT_FAILURE;
  }

  char response[SABER_IPC_MAX_RESPONSE + 16];
  size_t len = 0;

  while (len < sizeof(response) - 1) {
    ssize_t n = read(fd, response + len, sizeof(response) - 1 - len);

    if (n > 0) {
      len += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      fprintf(stderr, "saberctl: could not read the response: %s\n",
          strerror(errno));
      close(fd);
      return EXIT_FAILURE;
    }
    break;
  }

  close(fd);
  response[len] = '\0';

  if (len == 0) {
    fprintf(stderr,
        "saberctl: the panel closed the connection without answering.\n");
    return EXIT_FAILURE;
  }

  fputs(response, stdout);

  /* The grammar is one word: a reply that opens with `error` failed, and
  everything else -- `ok`, or a status report -- succeeded. */
  return strncmp(response, "error ", 6) == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
