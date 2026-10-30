/* Script function and purpose: The regression test for the ceilings a
configuration file is read under. It drives the public luaguard interface
directly -- open a state, arm it, run a chunk -- which is exactly what
config_open() and hikari_open() do, so what it measures is what the panel does
at startup and on `saberctl reload`. It links luaguard.o, Lua and libc: no
compositor, no configuration parser, no glib, so it runs on any host.

WHY THE TIMES ARE ASSERTED AND NOT JUST THE REFUSALS. Every one of these chunks
was refused before this test existed. What was wrong was how long the refusal
took: the concatenation case ran 77.7 seconds and string.upper past five
minutes, because the instruction ceiling bounds instructions and says nothing
about what one instruction costs. A test that only asked "was it refused" passed
throughout. So each case asserts a bound on the elapsed time, set far enough
above the ceiling to survive a loaded machine and far enough below the old
numbers to fail if the deadline is ever lost. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <saber/luaguard.h>

/* The reader's own instruction ceiling. Duplicated from src/config.c rather
than exported, because what this test pins is the behaviour a reader gets when
it arms with its own number, and src/theme.c arms with the same one. */
#define BUDGET_TEST_STEPS 2000000

/* How long a refused chunk may take. The ceiling is SABER_LUA_TIME_CAP_MS and
the overshoot is bounded by one sample of instructions, measured at 0.0-0.3 s
for the most expensive instruction the memory ceiling allows. Ten seconds is
thirty times that overshoot, so a slow or contended machine cannot fail this,
while the 77.7 s and 300 s+ the escape used to take cannot pass it. */
#define BUDGET_TEST_LIMIT_MS 10000

static const char *const budget_environment[] = {
  "assert",
  "error",
  "ipairs",
  "math",
  "pairs",
  "select",
  "string",
  "table",
  "tonumber",
  "tostring",
  "type",
  NULL,
};

static int failures;

static long long
budget_now_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

/* Function purpose: End a chunk that has passed a ceiling, which is the whole
body of both readers' hooks. */
static void
budget_runaway(lua_State *L, lua_Debug *activation)
{
  const char *reason = saber_lua_tick(L);

  (void)activation;

  if (reason == NULL) {
    return;
  }

  luaL_error(L, "%s", reason);
}

/* Function purpose: Run `chunk` under the real ceilings and report how it
ended. `refused` says whether Lua raised; the return is the elapsed
milliseconds. */
static long long
budget_run(const char *chunk, bool *refused, char *message, size_t size)
{
  lua_State *L = saber_lua_open(budget_environment, NULL, false);
  long long start;
  long long elapsed;
  int status;

  if (L == NULL) {
    fprintf(stderr, "FAIL: the interpreter could not be built\n");
    failures++;
    *refused = false;

    return 0;
  }

  if (luaL_loadbufferx(L, chunk, strlen(chunk), "budget", "t") != LUA_OK) {
    snprintf(message, size, "%s", lua_tostring(L, -1));
    saber_lua_close(L);
    *refused = true;

    return 0;
  }

  if (!saber_lua_enter(L)) {
    saber_lua_close(L);
    fprintf(stderr, "FAIL: the chunk would not take the sandbox\n");
    failures++;
    *refused = false;

    return 0;
  }

  saber_lua_arm(L, budget_runaway, BUDGET_TEST_STEPS);

  start = budget_now_ms();
  status = lua_pcall(L, 0, 1, 0);
  elapsed = budget_now_ms() - start;

  saber_lua_unbound(L);

  *refused = status != LUA_OK;
  snprintf(message, size, "%s",
      status == LUA_OK ? "finished" : lua_tostring(L, -1));

  saber_lua_close(L);

  return elapsed;
}

/* Function purpose: A chunk that must be refused, and must be refused
promptly. The name is printed either way so a failure says which shape. */
static void
check_refused(const char *name, const char *chunk)
{
  char message[256];
  bool refused = false;
  long long took = budget_run(chunk, &refused, message, sizeof(message));

  if (!refused) {
    printf("FAIL  %-22s ran to completion in %lldms; it must be refused\n",
        name, took);
    failures++;

    return;
  }

  if (took > BUDGET_TEST_LIMIT_MS) {
    printf("FAIL  %-22s refused, but only after %lldms (limit %dms): %s\n",
        name, took, BUDGET_TEST_LIMIT_MS, message);
    failures++;

    return;
  }

  printf("ok    %-22s refused in %4lldms: %s\n", name, took, message);
}

/* Function purpose: A chunk a real configuration could contain, which must run
to completion. The ceilings are worth nothing if they refuse ordinary files. */
static void
check_allowed(const char *name, const char *chunk)
{
  char message[256];
  bool refused = false;
  long long took = budget_run(chunk, &refused, message, sizeof(message));

  if (refused) {
    printf("FAIL  %-22s was refused: %s\n", name, message);
    failures++;

    return;
  }

  printf("ok    %-22s finished in %4lldms\n", name, took);
}

int
main(void)
{
  /* The shapes whose work happens inside one VM instruction or one C call, and
  which no byte counter in luaguard.c charges. Each of these ran for tens of
  seconds to minutes before the deadline existed. */
  check_refused("concat",
      "local s = string.rep('x', 1048576)\n"
      "for i = 1, 1e9 do local _ = s .. s end\n"
      "return {}\n");

  check_refused("string.upper",
      "local s = string.rep('x', 1048576)\n"
      "for i = 1, 1e9 do local _ = s:upper() end\n"
      "return {}\n");

  check_refused("string.reverse",
      "local s = string.rep('x', 1048576)\n"
      "for i = 1, 1e9 do local _ = s:reverse() end\n"
      "return {}\n");

  check_refused("table.concat",
      "local s = string.rep('x', 1048576)\n"
      "local t = {s, s, s, s}\n"
      "for i = 1, 1e9 do local _ = table.concat(t) end\n"
      "return {}\n");

  check_refused("gsub output",
      "local s = string.rep('x', 65536)\n"
      "for i = 1, 1e9 do local _ = s:gsub('x', 'yy') end\n"
      "return {}\n");

  /* The worst single instruction the memory ceiling leaves available: the
  operands and the result of a sixteen-mebibyte table.concat all live at once.
  This is what sets the overshoot, so it is the case that would fail first if
  SABER_LUA_SAMPLE were ever loosened. */
  check_refused("widest instruction",
      "local s = string.rep('x', 1048576)\n"
      "s = s .. s\n"
      "s = s .. s\n"
      "s = s .. s\n"
      "local t = {s, s}\n"
      "for i = 1, 1e9 do local _ = table.concat(t) end\n"
      "return {}\n");

  /* The ceilings that already existed, which the deadline is added beside and
  does not replace. A tight loop reaches the instruction ceiling long before a
  second has passed, and the two string wrappers reach the byte ceiling -- so a
  regression that left only the deadline standing would show up here as a
  refusal arriving later and under a different reason. */
  check_refused("tight loop", "while true do end\nreturn {}\n");

  check_refused("string.rep loop",
      "for i = 1, 1e9 do local _ = string.rep('x', 1048575) end\n"
      "return {}\n");

  check_refused("string.rep count",
      "return {s = string.rep('', math.maxinteger)}\n");

  /* And the files that must still be read. The second is the shape the shipped
  configuration actually has: sixteen colours turned into strings. */
  check_allowed("empty table", "return {}\n");

  check_allowed("sixteen colours",
      "local t = {}\n"
      "for i = 1, 16 do t[i] = string.format('#%02x%02x%02x', i, i, i) end\n"
      "return {palette = t}\n");

  check_allowed("a real separator",
      "return {sep = string.rep('-', 80), name = ('saber'):upper()}\n");

  if (failures != 0) {
    printf("\n%d of the configuration budget checks failed\n", failures);

    return 1;
  }

  printf("\nall configuration budget checks passed\n");

  return 0;
}
