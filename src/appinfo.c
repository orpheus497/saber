/* Script function and purpose: XDG desktop-entry index -- scanning, visibility
filtering, Exec field-code expansion and launching. Ported from sofi's
source/modes/drun.c (MIT, same author); the scan order, the "first directory to
supply an ID wins" masking and the TryExec test come from there. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/appinfo.h>

static const char DESKTOP_GROUP[] = "Desktop Entry";

/* Action purpose: A package install rewrites a whole directory, so the monitor
fires many times for one logical change. Re-scanning per event would walk every
data directory dozens of times; the timer restarts on each event and the scan
happens once the burst is over. */
#define SABER_APPINFO_DEBOUNCE_MS 400

/* Bind-mount loops report as directories, so the walk needs its own bound. */
#define SABER_APPINFO_MAX_DEPTH 32

/* The grandchild reports a failed exec on this descriptor; fixing it lets the
rest of the inherited table be closed in one call. */
#define SABER_SPAWN_REPORT_FD 3

struct saber_appinfo_index {
  GHashTable *by_id; /* char * -> struct saber_appinfo *, owns a ref */
  GPtrArray *entries; /* borrowed, in scan order */
  GPtrArray *monitors;
  guint debounce;
  unsigned int generation;
  saber_appinfo_changed_func changed;
  void *changed_data;
};

struct saber_appinfo_scan {
  GHashTable *by_id;
  GPtrArray *entries;
  GHashTable *seen; /* IDs already resolved, including suppressed ones */
  GPtrArray *dirs; /* every directory walked, so each can be monitored */
  gchar **desktops; /* XDG_CURRENT_DESKTOP, split */
};

/*
 * Entries
 */

struct saber_appinfo *
saber_appinfo_ref(struct saber_appinfo *app)
{
  if (app != NULL) {
    app->refs++;
  }
  return app;
}

static void
action_clear(struct saber_appinfo_action *action)
{
  g_free(action->id);
  g_free(action->name);
  g_free(action->icon);
  g_free(action->exec);
}

void
saber_appinfo_unref(struct saber_appinfo *app)
{
  if (app == NULL || --app->refs > 0) {
    return;
  }

  for (size_t i = 0; i < app->actions_len; i++) {
    action_clear(&app->actions[i]);
  }
  g_free(app->actions);

  g_free(app->id);
  g_free(app->path);
  g_free(app->name);
  g_free(app->generic_name);
  g_free(app->comment);
  g_free(app->icon);
  g_free(app->exec);
  g_free(app->startup_wm_class);
  g_free(app->work_dir);
  g_strfreev(app->categories);
  g_free(app);
}

const struct saber_appinfo_action *
saber_appinfo_find_action(const struct saber_appinfo *app, const char *id)
{
  if (app == NULL || id == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < app->actions_len; i++) {
    if (g_strcmp0(app->actions[i].id, id) == 0) {
      return &app->actions[i];
    }
  }
  return NULL;
}

/*
 * Exec field codes
 */

enum field_kind {
  FIELD_INVALID,
  FIELD_FILE, /* %f */
  FIELD_FILES, /* %F */
  FIELD_URI, /* %u */
  FIELD_URIS, /* %U */
  FIELD_ICON, /* %i -- two arguments when it stands alone */
  FIELD_NAME, /* %c */
  FIELD_LOCATION, /* %k */
  FIELD_DROP, /* deprecated: expands to nothing */
  FIELD_PERCENT, /* %% */
};

static const struct {
  char code;
  enum field_kind kind;
} field_codes[] = {
  { 'f', FIELD_FILE },
  { 'F', FIELD_FILES },
  { 'u', FIELD_URI },
  { 'U', FIELD_URIS },
  { 'i', FIELD_ICON },
  { 'c', FIELD_NAME },
  { 'k', FIELD_LOCATION },
  { 'd', FIELD_DROP },
  { 'D', FIELD_DROP },
  { 'n', FIELD_DROP },
  { 'N', FIELD_DROP },
  { 'v', FIELD_DROP },
  { 'm', FIELD_DROP },
  { '%', FIELD_PERCENT },
};

static enum field_kind
field_lookup(char code)
{
  for (size_t i = 0; i < G_N_ELEMENTS(field_codes); i++) {
    if (field_codes[i].code == code) {
      return field_codes[i].kind;
    }
  }
  return FIELD_INVALID;
}

/* Returns NULL for a URI that has no local path: %f cannot represent one. */
static char *
as_path(const char *spec)
{
  char *scheme = g_uri_parse_scheme(spec);
  if (scheme == NULL) {
    return g_strdup(spec);
  }

  char *path = NULL;
  if (g_ascii_strcasecmp(scheme, "file") == 0) {
    path = g_filename_from_uri(spec, NULL, NULL);
  }
  g_free(scheme);
  return path;
}

static char *
as_uri(const char *spec)
{
  char *scheme = g_uri_parse_scheme(spec);
  if (scheme != NULL) {
    g_free(scheme);
    return g_strdup(spec);
  }

  char *absolute = g_canonicalize_filename(spec, NULL);
  char *uri = g_filename_to_uri(absolute, NULL, NULL);
  g_free(absolute);
  return uri;
}

static void
append_value(GPtrArray *out, char *value)
{
  if (value != NULL) {
    g_ptr_array_add(out, value);
  }
}

/* Function purpose: Turn one Exec token into zero or more argv entries. The
token has already been split off by the quoting rules, so a field code can only
ever produce whole arguments -- an expanded file name is never re-parsed, which
is what keeps a hostile name from becoming a command. */
static bool
expand_token(const struct saber_appinfo *app,
    const char *token,
    const char *const *uris,
    GPtrArray *out)
{
  /* Action purpose: The codes that may yield a count other than one argument
  are only honoured when they are the entire token, as the spec requires.
  Embedded, they fall through to the textual pass and behave as their
  single-value form, which is what "--files=%F" in the wild means. */
  if (token[0] == '%' && token[1] != '\0' && token[2] == '\0') {
    enum field_kind kind = field_lookup(token[1]);
    switch (kind) {
      case FIELD_FILES:
      case FIELD_URIS:
        for (size_t i = 0; uris != NULL && uris[i] != NULL; i++) {
          append_value(out,
              kind == FIELD_FILES ? as_path(uris[i]) : as_uri(uris[i]));
        }
        return true;
      case FIELD_FILE:
      case FIELD_URI:
        if (uris != NULL && uris[0] != NULL) {
          append_value(out,
              kind == FIELD_FILE ? as_path(uris[0]) : as_uri(uris[0]));
        }
        return true;
      case FIELD_ICON:
        if (app->icon != NULL) {
          g_ptr_array_add(out, g_strdup("--icon"));
          g_ptr_array_add(out, g_strdup(app->icon));
        }
        return true;
      case FIELD_INVALID:
        return false;
      default:
        break;
    }
  }

  GString *buffer = g_string_new(NULL);
  bool expanded = false;

  for (const char *p = token; *p != '\0'; p++) {
    if (*p != '%') {
      g_string_append_c(buffer, *p);
      continue;
    }

    p++;
    if (*p == '\0') {
      g_string_free(buffer, TRUE);
      return false;
    }
    expanded = true;

    char *value = NULL;
    switch (field_lookup(*p)) {
      case FIELD_PERCENT:
        g_string_append_c(buffer, '%');
        break;
      case FIELD_DROP:
        break;
      case FIELD_NAME:
        if (app->name != NULL) {
          g_string_append(buffer, app->name);
        }
        break;
      case FIELD_LOCATION:
        if (app->path != NULL) {
          g_string_append(buffer, app->path);
        }
        break;
      case FIELD_ICON:
        if (app->icon != NULL) {
          g_string_append(buffer, app->icon);
        }
        break;
      case FIELD_FILE:
      case FIELD_FILES:
        value = (uris != NULL && uris[0] != NULL) ? as_path(uris[0]) : NULL;
        break;
      case FIELD_URI:
      case FIELD_URIS:
        value = (uris != NULL && uris[0] != NULL) ? as_uri(uris[0]) : NULL;
        break;
      default:
        g_string_free(buffer, TRUE);
        return false;
    }

    if (value != NULL) {
      g_string_append(buffer, value);
      g_free(value);
    }
  }

  /* A token that was nothing but a code with no value to supply disappears
  rather than becoming an empty argument; a literal "" stays. */
  if (expanded && buffer->len == 0) {
    g_string_free(buffer, TRUE);
    return true;
  }

  g_ptr_array_add(out, g_string_free(buffer, FALSE));
  return true;
}

char **
saber_appinfo_build_argv(const struct saber_appinfo *app,
    const char *action_id,
    const char *const *uris)
{
  if (app == NULL) {
    return NULL;
  }

  const char *exec = app->exec;
  if (action_id != NULL) {
    const struct saber_appinfo_action *action =
        saber_appinfo_find_action(app, action_id);
    if (action == NULL) {
      g_warning("[%s] no such action \"%s\".", app->id, action_id);
      return NULL;
    }
    exec = action->exec;
  }
  if (exec == NULL) {
    return NULL;
  }

  /* Action purpose: The Exec value is split by the desktop-entry quoting rules
  and then handed to execvp. It is never passed to a shell, so none of the
  reserved characters can act. */
  int count = 0;
  char **tokens = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(exec, &count, &tokens, &error)) {
    g_warning("[%s] unparsable Exec \"%s\": %s.", app->id, exec,
        error->message);
    g_error_free(error);
    return NULL;
  }

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  bool ok = true;
  for (int i = 0; ok && i < count; i++) {
    ok = expand_token(app, tokens[i], uris, out);
  }
  g_strfreev(tokens);

  if (!ok || out->len == 0) {
    if (!ok) {
      g_warning("[%s] invalid field code in Exec \"%s\".", app->id, exec);
    }
    g_ptr_array_free(out, TRUE);
    return NULL;
  }

  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

void
saber_appinfo_free_argv(char **argv)
{
  g_strfreev(argv);
}

/*
 * Launching
 */

static char **
wrap_in_terminal(char *const *argv)
{
  const char *terminal = g_getenv("TERMINAL");
  if (terminal == NULL || *terminal == '\0') {
    terminal = "xterm";
  }

  int count = 0;
  char **parts = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(terminal, &count, &parts, &error)) {
    g_warning("unparsable TERMINAL \"%s\": %s.", terminal, error->message);
    g_error_free(error);
    return NULL;
  }

  GPtrArray *out = g_ptr_array_new();
  for (int i = 0; i < count; i++) {
    g_ptr_array_add(out, parts[i]);
  }
  g_free(parts); /* elements were stolen above */

  g_ptr_array_add(out, g_strdup("-e"));
  for (char *const *p = argv; *p != NULL; p++) {
    g_ptr_array_add(out, g_strdup(*p));
  }
  g_ptr_array_add(out, NULL);

  return (char **)g_ptr_array_free(out, FALSE);
}

static void
report_errno(int fd, int value)
{
  ssize_t written;
  do {
    written = write(fd, &value, sizeof(value));
  } while (written < 0 && errno == EINTR);
  (void)written;
}

/* Function purpose: Build the child's environment in the PARENT. Everything
between fork and exec must be async-signal-safe, and GLib's environment calls
are not; assigning `environ` in the child is. */
static char **
child_environ(const char *token)
{
  char **env = g_get_environ();
  if (token != NULL) {
    env = g_environ_setenv(env, "XDG_ACTIVATION_TOKEN", token, TRUE);
    env = g_environ_setenv(env, "DESKTOP_STARTUP_ID", token, TRUE);
  } else {
    /* An inherited token belongs to some earlier launch and would misdirect
    focus if it were passed on. */
    env = g_environ_unsetenv(env, "XDG_ACTIVATION_TOKEN");
    env = g_environ_unsetenv(env, "DESKTOP_STARTUP_ID");
  }
  return env;
}

static void
close_inherited_fds(void)
{
#if defined(__FreeBSD__)
  closefrom(SABER_SPAWN_REPORT_FD + 1);
#else
  /* Action purpose: closefrom(2) is the FreeBSD interface and the only one
  used on the target; the loop exists so these sources still compile on the
  host used for syntax checking. */
  long limit = sysconf(_SC_OPEN_MAX);
  if (limit < 0 || limit > 4096) {
    limit = 4096;
  }
  for (int fd = SABER_SPAWN_REPORT_FD + 1; fd < (int)limit; fd++) {
    close(fd);
  }
#endif
}

extern char **environ;

static void
child_exec(char *const *argv,
    const char *work_dir,
    char **envp,
    int report_fd)
{
  /* Action purpose: The intermediate child exits at once so the grandchild is
  reparented to init and reaped there. The panel therefore never needs a
  SIGCHLD handler, and a launched application cannot become a zombie under a
  GLib main loop that knows nothing about it. */
  setsid();

  pid_t inner = fork();
  if (inner > 0) {
    _exit(0);
  }
  if (inner < 0) {
    report_errno(report_fd, errno);
    _exit(127);
  }

  if (report_fd != SABER_SPAWN_REPORT_FD) {
    if (dup2(report_fd, SABER_SPAWN_REPORT_FD) < 0) {
      _exit(127);
    }
    close(report_fd);
  }
  fcntl(SABER_SPAWN_REPORT_FD, F_SETFD, FD_CLOEXEC);

  int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    if (null > STDERR_FILENO) {
      close(null);
    }
  }
  close_inherited_fds();

  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);
  signal(SIGPIPE, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);

  if (work_dir != NULL && *work_dir != '\0' && chdir(work_dir) != 0) {
    /* Path= naming a directory that has since gone is not worth refusing the
    launch over; the application simply starts in the panel's directory. */
  }

  environ = envp;

  execvp(argv[0], argv);
  report_errno(SABER_SPAWN_REPORT_FD, errno);
  _exit(127);
}

static bool
spawn_detached(char *const *argv, const char *work_dir, char **envp)
{
  int report[2];
  if (pipe(report) != 0) {
    g_warning("pipe: %s.", g_strerror(errno));
    return false;
  }

  pid_t outer = fork();
  if (outer < 0) {
    g_warning("fork: %s.", g_strerror(errno));
    close(report[0]);
    close(report[1]);
    return false;
  }

  if (outer == 0) {
    close(report[0]);
    child_exec(argv, work_dir, envp, report[1]);
    _exit(127);
  }

  close(report[1]);

  int status = 0;
  while (waitpid(outer, &status, 0) < 0 && errno == EINTR) {
    continue;
  }

  int failure = 0;
  ssize_t got;
  do {
    got = read(report[0], &failure, sizeof(failure));
  } while (got < 0 && errno == EINTR);
  close(report[0]);

  if (got == (ssize_t)sizeof(failure)) {
    g_warning("failed to execute \"%s\": %s.", argv[0], g_strerror(failure));
    return false;
  }
  return true;
}

bool
saber_appinfo_launch(const struct saber_appinfo *app,
    const char *action_id,
    const char *const *uris,
    const char *activation_token)
{
  char **argv = saber_appinfo_build_argv(app, action_id, uris);
  if (argv == NULL) {
    return false;
  }

  char **wrapped = NULL;
  if (app->terminal) {
    wrapped = wrap_in_terminal(argv);
    if (wrapped == NULL) {
      g_strfreev(argv);
      return false;
    }
  }

  char **envp = child_environ(activation_token);
  bool ok =
      spawn_detached(wrapped != NULL ? wrapped : argv, app->work_dir, envp);

  g_strfreev(envp);
  g_strfreev(wrapped);
  g_strfreev(argv);
  return ok;
}

/*
 * Scanning
 */

static char *
desktop_id_for(const char *root, const char *path)
{
  size_t root_len = strlen(root);
  if (strncmp(path, root, root_len) != 0) {
    return NULL;
  }

  const char *relative = path + root_len;
  while (*relative == G_DIR_SEPARATOR) {
    relative++;
  }
  if (*relative == '\0') {
    return NULL;
  }

  char *id = g_strdup(relative);
  for (char *p = id; *p != '\0'; p++) {
    if (*p == G_DIR_SEPARATOR) {
      *p = '-';
    }
  }
  return id;
}

/* Function purpose: The desktop is a colon-separated list -- hikari sets
XDG_CURRENT_DESKTOP to "Hikari Sakura:wlroots" -- and OnlyShowIn/NotShowIn
match if any one element matches. */
static bool
shown_on_desktop(GKeyFile *file, gchar **desktops)
{
  if (desktops == NULL) {
    return true;
  }

  bool show = true;
  if (g_key_file_has_key(file, DESKTOP_GROUP, "OnlyShowIn", NULL)) {
    show = false;
    gchar **only = g_key_file_get_string_list(file, DESKTOP_GROUP, "OnlyShowIn",
        NULL, NULL);
    for (size_t i = 0; !show && only != NULL && only[i] != NULL; i++) {
      show = g_strv_contains((const gchar *const *)desktops, only[i]);
    }
    g_strfreev(only);
  }

  if (show && g_key_file_has_key(file, DESKTOP_GROUP, "NotShowIn", NULL)) {
    gchar **denied = g_key_file_get_string_list(file, DESKTOP_GROUP,
        "NotShowIn", NULL, NULL);
    for (size_t i = 0; show && denied != NULL && denied[i] != NULL; i++) {
      show = !g_strv_contains((const gchar *const *)desktops, denied[i]);
    }
    g_strfreev(denied);
  }

  return show;
}

/* Action purpose: TryExec names the binary the entry needs; an entry whose
binary is absent is an installed-but-unusable leftover and is skipped rather
than offered as a tile that fails on click. */
static bool
try_exec_ok(GKeyFile *file)
{
  char *value = g_key_file_get_string(file, DESKTOP_GROUP, "TryExec", NULL);
  if (value == NULL || *value == '\0') {
    g_free(value);
    return true;
  }

  bool ok;
  if (g_path_is_absolute(value)) {
    ok = g_file_test(value, G_FILE_TEST_IS_EXECUTABLE);
  } else {
    char *found = g_find_program_in_path(value);
    ok = found != NULL;
    g_free(found);
  }
  g_free(value);
  return ok;
}

/* Action purpose: GKeyFile rejects a value carrying an escape sequence it does
not know, which some real Exec lines do carry; the raw value is still the right
thing to parse in that case. */
static char *
get_raw_string(GKeyFile *file, const char *group, const char *key)
{
  char *value = g_key_file_get_string(file, group, key, NULL);
  if (value == NULL) {
    value = g_key_file_get_value(file, group, key, NULL);
  }
  return value;
}

static void
read_actions(struct saber_appinfo *app, GKeyFile *file)
{
  gsize count = 0;
  gchar **names =
      g_key_file_get_string_list(file, DESKTOP_GROUP, "Actions", &count, NULL);
  if (names == NULL || count == 0) {
    g_strfreev(names);
    return;
  }

  app->actions = g_new0(struct saber_appinfo_action, count);
  for (gsize i = 0; i < count; i++) {
    if (names[i][0] == '\0') {
      continue;
    }

    char *group = g_strdup_printf("Desktop Action %s", names[i]);
    if (!g_key_file_has_group(file, group)) {
      g_free(group);
      continue;
    }

    struct saber_appinfo_action *action = &app->actions[app->actions_len];
    action->id = g_strdup(names[i]);
    action->name =
        g_key_file_get_locale_string(file, group, "Name", NULL, NULL);
    action->icon =
        g_key_file_get_locale_string(file, group, "Icon", NULL, NULL);
    action->exec = get_raw_string(file, group, "Exec");
    app->actions_len++;

    g_free(group);
  }

  g_strfreev(names);
}

static void
read_desktop_file(struct saber_appinfo_scan *scan,
    const char *root,
    const char *path)
{
  char *id = desktop_id_for(root, path);
  if (id == NULL) {
    return;
  }

  /* A directory earlier in the search order has already spoken for this ID,
  whether by supplying an entry or by suppressing one. */
  if (g_hash_table_contains(scan->seen, id)) {
    g_free(id);
    return;
  }

  GKeyFile *file = g_key_file_new();
  GError *error = NULL;
  if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error)) {
    g_debug("[%s] %s: %s.", id, path, error->message);
    g_error_free(error);
    g_key_file_free(file);
    g_free(id);
    return;
  }

  char *type = g_key_file_get_string(file, DESKTOP_GROUP, "Type", NULL);
  bool application = g_strcmp0(type, "Application") == 0;
  g_free(type);
  if (!application) {
    g_key_file_free(file);
    g_free(id);
    return;
  }

  bool hidden = g_key_file_get_boolean(file, DESKTOP_GROUP, "Hidden", NULL) ||
      g_key_file_get_boolean(file, DESKTOP_GROUP, "NoDisplay", NULL) ||
      !shown_on_desktop(file, scan->desktops);
  if (hidden) {
    /* Recorded so a lower-priority copy of the same ID stays suppressed. */
    g_hash_table_add(scan->seen, id);
    g_key_file_free(file);
    return;
  }

  char *name = g_key_file_get_locale_string(file, DESKTOP_GROUP, "Name", NULL,
      NULL);
  char *exec = get_raw_string(file, DESKTOP_GROUP, "Exec");
  if (name == NULL || exec == NULL || !try_exec_ok(file)) {
    g_free(name);
    g_free(exec);
    g_key_file_free(file);
    g_free(id);
    return;
  }

  struct saber_appinfo *app = g_new0(struct saber_appinfo, 1);
  app->refs = 1;
  app->id = id;
  app->path = g_strdup(path);
  app->name = name;
  app->exec = exec;
  app->generic_name =
      g_key_file_get_locale_string(file, DESKTOP_GROUP, "GenericName", NULL,
          NULL);
  app->comment =
      g_key_file_get_locale_string(file, DESKTOP_GROUP, "Comment", NULL, NULL);
  app->icon =
      g_key_file_get_locale_string(file, DESKTOP_GROUP, "Icon", NULL, NULL);
  app->startup_wm_class =
      g_key_file_get_string(file, DESKTOP_GROUP, "StartupWMClass", NULL);
  app->work_dir = g_key_file_get_string(file, DESKTOP_GROUP, "Path", NULL);
  app->categories =
      g_key_file_get_string_list(file, DESKTOP_GROUP, "Categories", NULL, NULL);
  app->terminal =
      g_key_file_get_boolean(file, DESKTOP_GROUP, "Terminal", NULL);
  app->dbus_activatable =
      g_key_file_get_boolean(file, DESKTOP_GROUP, "DBusActivatable", NULL);

  if (app->work_dir != NULL && *app->work_dir == '\0') {
    g_free(app->work_dir);
    app->work_dir = NULL;
  }

  read_actions(app, file);
  g_key_file_free(file);

  g_hash_table_add(scan->seen, g_strdup(app->id));
  g_hash_table_insert(scan->by_id, g_strdup(app->id), app);
  g_ptr_array_add(scan->entries, app);
}

static void
walk_dir(struct saber_appinfo_scan *scan,
    const char *root,
    const char *dirname,
    unsigned int depth)
{
  if (depth > SABER_APPINFO_MAX_DEPTH) {
    g_warning("Desktop-file scan exceeded %d directory levels at \"%s\".",
        SABER_APPINFO_MAX_DEPTH, dirname);
    return;
  }

  GDir *dir = g_dir_open(dirname, 0, NULL);
  if (dir == NULL) {
    return;
  }
  g_ptr_array_add(scan->dirs, g_strdup(dirname));

  const gchar *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (name[0] == '.') {
      continue;
    }

    char *path = g_build_filename(dirname, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      walk_dir(scan, root, path, depth + 1);
    } else if (g_str_has_suffix(name, ".desktop")) {
      read_desktop_file(scan, root, path);
    }
    g_free(path);
  }

  g_dir_close(dir);
}

static void
monitor_changed(GFileMonitor *monitor,
    GFile *file,
    GFile *other,
    GFileMonitorEvent event,
    gpointer data);

static void
rebuild_monitors(struct saber_appinfo_index *index, GPtrArray *dirs)
{
  g_ptr_array_set_size(index->monitors, 0);

  for (guint i = 0; i < dirs->len; i++) {
    GFile *dir = g_file_new_for_path(g_ptr_array_index(dirs, i));
    GFileMonitor *monitor = g_file_monitor_directory(dir,
        G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_object_unref(dir);

    if (monitor == NULL) {
      continue;
    }
    g_signal_connect(monitor, "changed", G_CALLBACK(monitor_changed), index);
    g_ptr_array_add(index->monitors, monitor);
  }
}

void
saber_appinfo_index_rescan(struct saber_appinfo_index *index)
{
  struct saber_appinfo_scan scan = {
    .by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)saber_appinfo_unref),
    .entries = g_ptr_array_new(),
    .seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    .dirs = g_ptr_array_new_with_free_func(g_free),
    .desktops = NULL,
  };

  const char *current = g_getenv("XDG_CURRENT_DESKTOP");
  if (current != NULL && *current != '\0') {
    scan.desktops = g_strsplit(current, ":", 0);
  }

  /* GLib already applies the XDG defaults these two resolve to, which on
  FreeBSD are /usr/local/share and /usr/share. */
  char *user = g_build_filename(g_get_user_data_dir(), "applications", NULL);
  walk_dir(&scan, user, user, 0);
  g_free(user);

  const gchar *const *system = g_get_system_data_dirs();
  for (const gchar *const *it = system; it != NULL && *it != NULL; it++) {
    if (**it == '\0') {
      continue;
    }

    bool duplicate = false;
    for (const gchar *const *seen = system; seen != it; seen++) {
      duplicate = duplicate || g_strcmp0(*seen, *it) == 0;
    }
    if (duplicate) {
      continue;
    }

    char *dir = g_build_filename(*it, "applications", NULL);
    walk_dir(&scan, dir, dir, 0);
    g_free(dir);
  }

  g_hash_table_destroy(index->by_id);
  g_ptr_array_free(index->entries, TRUE);
  index->by_id = scan.by_id;
  index->entries = scan.entries;
  index->generation++;

  rebuild_monitors(index, scan.dirs);

  g_hash_table_destroy(scan.seen);
  g_ptr_array_free(scan.dirs, TRUE);
  g_strfreev(scan.desktops);
}

static gboolean
debounce_fire(gpointer data)
{
  struct saber_appinfo_index *index = data;
  index->debounce = 0;

  saber_appinfo_index_rescan(index);
  if (index->changed != NULL) {
    index->changed(index->changed_data);
  }
  return G_SOURCE_REMOVE;
}

static void
monitor_changed(GFileMonitor *monitor,
    GFile *file,
    GFile *other,
    GFileMonitorEvent event,
    gpointer data)
{
  (void)monitor;
  (void)file;
  (void)other;
  (void)event;

  struct saber_appinfo_index *index = data;
  if (index->debounce != 0) {
    g_source_remove(index->debounce);
  }
  index->debounce =
      g_timeout_add(SABER_APPINFO_DEBOUNCE_MS, debounce_fire, index);
}

struct saber_appinfo_index *
saber_appinfo_index_create(void)
{
  struct saber_appinfo_index *index = g_new0(struct saber_appinfo_index, 1);
  index->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      (GDestroyNotify)saber_appinfo_unref);
  index->entries = g_ptr_array_new();
  index->monitors = g_ptr_array_new_with_free_func(g_object_unref);

  saber_appinfo_index_rescan(index);
  return index;
}

void
saber_appinfo_index_destroy(struct saber_appinfo_index *index)
{
  if (index == NULL) {
    return;
  }

  if (index->debounce != 0) {
    g_source_remove(index->debounce);
  }
  g_ptr_array_free(index->monitors, TRUE);
  g_ptr_array_free(index->entries, TRUE);
  g_hash_table_destroy(index->by_id);
  g_free(index);
}

void
saber_appinfo_index_set_changed(struct saber_appinfo_index *index,
    saber_appinfo_changed_func func,
    void *user_data)
{
  index->changed = func;
  index->changed_data = user_data;
}

unsigned int
saber_appinfo_index_generation(const struct saber_appinfo_index *index)
{
  return index->generation;
}

struct saber_appinfo *
saber_appinfo_index_lookup(const struct saber_appinfo_index *index,
    const char *id)
{
  if (index == NULL || id == NULL) {
    return NULL;
  }
  return g_hash_table_lookup(index->by_id, id);
}

size_t
saber_appinfo_index_size(const struct saber_appinfo_index *index)
{
  return index->entries->len;
}

struct saber_appinfo *
saber_appinfo_index_nth(const struct saber_appinfo_index *index, size_t n)
{
  if (n >= index->entries->len) {
    return NULL;
  }
  return g_ptr_array_index(index->entries, n);
}
