/* Script function and purpose: Build the interpreter a configuration file is
read in, with the three ceilings that make running somebody's file safe: a
memory cap, a bound on the two string operations whose work happens inside a
single C call, and a setup that cannot abort the panel.

WHY THE INSTRUCTION HOOK IS NOT ENOUGH, which is the whole reason this file
exists. Both readers arm a count hook (lua_sethook with LUA_MASKCOUNT). That
hook bounds Lua VM instructions and cannot interrupt a single C function, and
string.rep's copy loop is one C function. Lua's own guard rejects a repeat
whose RESULT would exceed its maximum string size, which sounds sufficient and
is not: string.rep("", math.maxinteger) produces a ZERO-LENGTH result, passes
that guard, and then runs its copy loop nearly maxinteger times. It allocates
nothing, so a memory cap does not catch it either. Measured with the hook armed
at 2000000 instructions, a saber.lua containing that one line ran past 20
seconds without the hook firing once -- at startup and again on `saberctl
reload`, which is a hung panel rather than a refused file. */

#include <saber/luaguard.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lualib.h>

/* [COMMENT] Action purpose: Refuse to compile against a Lua whose C API this
file is not written against. luaL_requiref and lua_rawgetp arrived in 5.2, and
5.5 gave lua_newstate a third seed argument that saber_lua_open() does not pass
-- so on 5.5 the guard's absence produces a wrong-argument-count error pointing
at a call site that looks correct, instead of a sentence naming the real
problem. Both makefiles probe for 5.4 and accept 5.3, but LUA_CFLAGS can be set
by hand and a distribution's unversioned `lua` module is whatever that
distribution ships. */
#if LUA_VERSION_NUM < 502 || LUA_VERSION_NUM > 504
#error "saber's configuration readers require Lua 5.2, 5.3 or 5.4"
#endif

/* Action purpose: Ceilings for the two string operations the instruction hook
cannot reach, because all of their work happens inside one C call.

SABER_LUA_REP_MAX bounds string.rep's repeat COUNT and not its result size. The
distinction is the defect: the zero-length result above passes Lua's own
size guard and then loops anyway, so bounding what comes out bounds nothing.
The count is what the loop runs on.

SABER_LUA_SUBJECT_MAX bounds the subject handed to the pattern functions.
Lua's matcher backtracks and its work is superlinear in subject length, so an
unbounded subject is the same hazard with a less obvious trigger. Bounding the
subject does not make pathological backtracking impossible -- a pattern with
several non-greedy quantifiers can still be slow on a short string -- and that
residual is accepted rather than hidden: it is bounded work on bounded input,
where the rep case was unbounded work outright.

Both sit far above any configuration anybody writes. A separator built with
string.rep("-", 80) uses 80 of a million, and the longest string in the shipped
saber.lua is 14 bytes of a possible 65536. */
#define SABER_LUA_REP_MAX (1 << 20)
#define SABER_LUA_SUBJECT_MAX (1 << 16)

/* Action purpose: SABER_LUA_WORK_MAX bounds the TOTAL byte-work the two
wrappers above may be asked for, across every call a configuration makes.

The two ceilings above each bound ONE call, and that is a different thing. A
configuration whose body is

  for i = 1, 1e9 do local _ = string.rep("x", 1048575) end

satisfies the count ceiling, costs a handful of VM instructions per iteration so
the instruction hook is nowhere near firing, and frees each megabyte before the
next one so the allocation ceiling never sees it. The same shape was measured
against sofi, which shares this design: it ran past ninety seconds with no
diagnostic, which for a panel means not appearing at startup and hanging on
`saberctl reload`. guard_pattern has the same shape with a less obvious trigger:
repeated 64 KiB subjects, each scanned in one C call.

A quarter of a gibibyte of copying takes a fraction of a second and is far more
than any configuration deriving a theme from sixteen colours can want, so a file
that reaches this ceiling is not doing work of this kind. */
#define SABER_LUA_WORK_MAX ((size_t)256 << 20)

struct guard_budget {
  size_t used;
  size_t cap;
  /* Bytes of C-side work the wrappers have been asked for, in total, and the
  ceiling on it. Separate from `used` because this work allocates nothing that
  lasts: see the note on SABER_LUA_WORK_MAX. A zero ceiling means unbounded,
  which is what saber_lua_unbound leaves behind. */
  size_t work;
  size_t work_cap;
};

struct guard_setup {
  const char *const *names;
  lua_CFunction undefined;
  bool environment;
};

/* Action purpose: the restricted environment is stored under
&guard_environment_key, so saber_lua_enter() can find it with no string key to
collide with anything else in the registry. */
static const char guard_environment_key = 0;

/* Function purpose: Lua's allocator with a ceiling. Returning NULL for a
growing request makes the core raise LUA_ERRMEM, which the reader's lua_pcall
turns into an ordinary diagnostic -- so a configuration that asks for a
terabyte is refused on the same terms as one that will not parse.

Action purpose: when ptr is NULL the osize argument is a type tag and not a
size, so the old size has to be taken as zero. That is the one part of the
lua_Alloc contract that is easy to get wrong, and getting it wrong corrupts the
running total silently rather than failing. */
static void *
guard_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  struct guard_budget *budget = ud;
  size_t old = ptr == NULL ? 0 : osize;
  void *block;

  if (nsize == 0) {
    free(ptr);
    budget->used -= old;

    return NULL;
  }

  if (nsize > old && nsize - old > budget->cap - budget->used) {
    return NULL;
  }

  block = realloc(ptr, nsize);

  if (block == NULL) {
    return NULL;
  }

  budget->used = budget->used - old + nsize;

  return block;
}

/* Function purpose: Call the real string function held as upvalue 1, the
caller's ceiling having already been checked. Shared by both wrappers because
the delegation is the fiddly half: the original has to be moved BELOW the
arguments it is being given, which is what the lua_insert does. */
static int
guard_delegate(lua_State *L)
{
  int args = lua_gettop(L);

  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, args, LUA_MULTRET);

  return lua_gettop(L);
}

/* Function purpose: Charge bytes of C-side work against the running total and
say whether the configuration has now had all it may have.

Action purpose: the budget is reached through lua_getallocf, which hands back
the pointer lua_setallocf was given, so a wrapper finds its own budget without a
second upvalue and without a global. A state this file did not create has a
different allocator and so no budget to charge, which is not a failure -- it
simply has no ceiling of this kind.

The addition saturates rather than wrapping, because a wrap would reset the
total to nearly zero and hand the configuration its whole allowance again. */
static bool
guard_charge(lua_State *L, size_t bytes)
{
  void *ud = NULL;
  struct guard_budget *budget;

  if (lua_getallocf(L, &ud) != guard_alloc || ud == NULL) {
    return true;
  }

  budget = ud;

  if (budget->work_cap == 0) {
    return true;
  }

  if (bytes > SIZE_MAX - budget->work) {
    budget->work = SIZE_MAX;
  } else {
    budget->work += bytes;
  }

  return budget->work <= budget->work_cap;
}

/* Function purpose: string.rep with its repeat count and its share of the
total work both bounded. */
static int
guard_rep(lua_State *L)
{
  lua_Integer count = luaL_optinteger(L, 2, 0);
  size_t len = 0;

  if (count > SABER_LUA_REP_MAX) {
    /* Action purpose: The count is rendered here rather than handed to
    luaL_error, because luaL_error formats through Lua's own limited formatter
    whose %d is an int -- so a count above INT_MAX would print as a negative
    number. The buffer is automatic because luaL_error does not return: nothing
    on the heap may be live across it. */
    char seen[32];

    snprintf(seen, sizeof(seen), "%lld", (long long)count);

    return luaL_error(L,
        "string.rep was asked for %s copies, and at most %d may be asked for",
        seen, SABER_LUA_REP_MAX);
  }

  /* The work is the count TIMES the length, because that is what the call
  copies. Bounding the count alone leaves a loop of large-but-legal calls
  unbounded. */
  if (lua_type(L, 1) == LUA_TSTRING) {
    lua_tolstring(L, 1, &len);
  }

  if (!guard_charge(L, count > 0 ? (size_t)count * len : 0)) {
    return luaL_error(L,
        "this configuration has asked for more than the %d MiB of string work "
        "saber allows it in total",
        (int)(SABER_LUA_WORK_MAX >> 20));
  }

  return guard_delegate(L);
}

/* Function purpose: A pattern function with its subject length bounded.
Installed over find, match, gmatch and gsub, each of which takes the subject as
its first argument. */
static int
guard_pattern(lua_State *L)
{
  size_t len = 0;

  /* Action purpose: the type is tested exactly, because lua_tolstring REWRITES
  the slot it is given when that slot is not already a string -- and this slot
  is about to be handed to the real function. A number subject is left to that
  function to coerce as it always has. */
  if (lua_type(L, 1) == LUA_TSTRING) {
    lua_tolstring(L, 1, &len);
  }

  if (len > SABER_LUA_SUBJECT_MAX) {
    char seen[32];

    snprintf(seen, sizeof(seen), "%llu", (unsigned long long)len);

    return luaL_error(L,
        "a %s byte string was given to a pattern function, and at most %d "
        "bytes may be matched",
        seen, SABER_LUA_SUBJECT_MAX);
  }

  /* One scan of the subject is the floor on what a match costs; a pattern that
  backtracks costs more. Charging the length bounds the NUMBER of scans, which
  is the half the per-call ceiling above leaves open. */
  if (!guard_charge(L, len)) {
    return luaL_error(L,
        "this configuration has asked for more than the %d MiB of string work "
        "saber allows it in total",
        (int)(SABER_LUA_WORK_MAX >> 20));
  }

  return guard_delegate(L);
}

/* Function purpose: Build the bounded replacement for the string library and
leave it on the stack.

Action purpose: the replacement is ALSO installed as the string metatable's
__index, which is the half of this that is easy to miss. Replacing only the
environment's `string` leaves the method-call form reaching straight past it:
(""):rep(n) does not read that name at all, it goes through the metatable Lua
installs for every string, whose __index is the real library. Reproduced --
with the table replaced and the metatable left alone, string.rep("",
math.maxinteger) was refused and (""):rep(math.maxinteger) still hung. Closing
one route and not the other is no fix.

Mutating the shared string metatable is contained because each reader builds
its own interpreter per load and closes it afterwards, so nothing outside that
load can observe it. */
static void
guard_push_string(lua_State *L)
{
  static const char *const bounded[] = {
    "find",
    "match",
    "gmatch",
    "gsub",
    NULL,
  };

  lua_getglobal(L, LUA_STRLIBNAME);
  lua_newtable(L);

  /* Action purpose: Every entry is copied and then the bounded ones are
  overridden, rather than the wanted ones being listed. Copying first means a
  function a future Lua adds still reaches the configuration instead of
  silently disappearing from the sandbox. */
  lua_pushnil(L);

  while (lua_next(L, -3) != 0) {
    lua_pushvalue(L, -2);
    lua_insert(L, -2);
    lua_settable(L, -4);
  }

  lua_getfield(L, -2, "rep");
  lua_pushcclosure(L, guard_rep, 1);
  lua_setfield(L, -2, "rep");

  for (int i = 0; bounded[i] != NULL; i++) {
    lua_getfield(L, -2, bounded[i]);
    lua_pushcclosure(L, guard_pattern, 1);
    lua_setfield(L, -2, bounded[i]);
  }

  lua_remove(L, -2);

  lua_pushliteral(L, "");

  if (lua_getmetatable(L, -1)) {
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
  }

  lua_pop(L, 1);
}

/* Function purpose: Open one library. luaL_requiref is the only supported way
to open a single one: it runs the opener, caches the module in package.loaded
and assigns the global, and leaves the module table on the stack for the caller
to drop. */
static void
guard_open_one(lua_State *L, const char *name, lua_CFunction opener)
{
  luaL_requiref(L, name, opener, 1);
  lua_pop(L, 1);
}

/* Function purpose: Answer a read of env.NAME with that environment variable,
or with nil when it is unset. Reached through the table's __index, so the table
stays empty and no copy of the environment is taken.

Action purpose: a Lua string may carry an embedded NUL and getenv(3) stops at
the first one, so `env["PATH\0anything"]` would otherwise be answered with the
value of PATH. A name whose C length is not its Lua length is not the name of a
variable, and is answered with nil rather than with its prefix's value. */
static int
guard_env_index(lua_State *L)
{
  size_t len;

  if (lua_type(L, 2) != LUA_TSTRING) {
    lua_pushnil(L);

    return 1;
  }

  const char *name = lua_tolstring(L, 2, &len);
  const char *value = (strlen(name) == len) ? getenv(name) : NULL;

  if (value == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, value);
  }

  return 1;
}

/* Function purpose: Refuse an assignment to env. The table is a view of this
process's environment and not a place to keep things, and a configuration that
believed it had set a variable would be wrong with nothing to say so. */
static int
guard_env_newindex(lua_State *L)
{
  return luaL_error(L, "env is read-only");
}

/* Function purpose: Build hikari's `env` and leave it on the stack.

Why this exists at all: hikari-sakura's own reader hands a configuration an
`env` to read the environment through, and the hikari.lua it ships uses it. A
sandbox without one does not lose that value, it loses the file -- indexing an
absent global raises, which ends the chunk before `palette` has been reached,
so the panel would fall back to its compiled-in colours against the very file
it exists to follow. Reading somebody else's file means providing what its own
reader provides.

Action purpose: __metatable = false is what makes the other two metamethods
hold. Without it a configuration reads the metatable through
getmetatable(env) and replaces __newindex, and read-only lasts exactly as long
as nobody tries; with it getmetatable answers false and setmetatable raises. */
static void
guard_push_env(lua_State *L)
{
  lua_newtable(L);

  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, guard_env_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, guard_env_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");

  lua_setmetatable(L, -2);
}

/* Function purpose: Open the four libraries a configuration has a use for,
build the restricted environment and leave it in the registry for
saber_lua_enter().

Action purpose: this runs as a protected call, because every step of it can
raise LUA_ERRMEM -- opening a library, interning a key, growing a table -- and
a raise with no protected frame below it reaches Lua's panic handler, which
calls abort(). A panel that aborts because the machine was briefly out of
memory is the same defect as one that aborts on a metamethod, which is why this
is a lua_CFunction and not a run of calls in saber_lua_open().

Four libraries OPENED, rather than all of them opened and most of them hidden.
luaL_openlibs constructs io, os, package, coroutine and debug and then relies
on the environment substitution to keep them out of reach; this constructs
neither the tables nor the functions in them, so there is nothing for a mistake
in the environment list -- or in a future change to it -- to expose. The
restricted list is what the chunk sees either way, so nothing a configuration
can do changes; what changes is what exists to be reached if it could. */
static int
guard_prepare(lua_State *L)
{
  const struct guard_setup *setup = lua_touserdata(L, 1);

  guard_open_one(L, "_G", luaopen_base);
  guard_open_one(L, LUA_STRLIBNAME, luaopen_string);
  guard_open_one(L, LUA_TABLIBNAME, luaopen_table);
  guard_open_one(L, LUA_MATHLIBNAME, luaopen_math);

  lua_newtable(L);

  for (int i = 0; setup->names[i] != NULL; i++) {
    lua_getglobal(L, setup->names[i]);
    lua_setfield(L, -2, setup->names[i]);
  }

  /* Action purpose: Replace the `string` the loop above just copied with the
  bounded one, which also points the string metatable at it. Done after the
  loop rather than instead of it, so a reader that leaves "string" out of its
  list does not silently get the real library through the metatable. */
  guard_push_string(L);
  lua_setfield(L, -2, LUA_STRLIBNAME);

  /* Action purpose: `env` is constructed rather than copied from a global,
  because there is no such global -- it is hikari's, and this is the reader that
  has to supply what hikari supplies. Only the reader of hikari's file asks for
  it; saber.lua has never had one and gaining one here would be a new feature
  in the wrong file. */
  if (setup->environment) {
    guard_push_env(L);
    lua_setfield(L, -2, "env");
  }

  if (setup->undefined != NULL) {
    lua_newtable(L);
    lua_pushcfunction(L, setup->undefined);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
  }

  lua_rawsetp(L, LUA_REGISTRYINDEX, &guard_environment_key);

  return 0;
}

lua_State *
saber_lua_open(const char *const names[], lua_CFunction undefined,
    bool environment)
{
  struct guard_budget *budget = malloc(sizeof(*budget));
  struct guard_setup setup = { names, undefined, environment };

  if (budget == NULL) {
    return NULL;
  }

  budget->used = 0;
  budget->cap = SABER_LUA_MEMORY_CAP;
  budget->work = 0;
  budget->work_cap = SABER_LUA_WORK_MAX;

  /* Action purpose: lua_newstate rather than luaL_newstate, because the cap is
  the allocator -- an interpreter built with Lua's own allocator and switched
  afterwards would have blocks in it that this budget never counted, and
  freeing one of those would take the running total below zero. The budget
  outlives this frame on the heap and is recovered at close through
  lua_getallocf, which is where the state keeps it. */
  lua_State *L = lua_newstate(guard_alloc, budget);

  if (L == NULL) {
    free(budget);

    return NULL;
  }

  lua_pushcfunction(L, guard_prepare);
  lua_pushlightuserdata(L, &setup);

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    saber_lua_close(L);

    return NULL;
  }

  return L;
}

bool
saber_lua_enter(lua_State *L)
{
  lua_rawgetp(L, LUA_REGISTRYINDEX, &guard_environment_key);

  const char *upvalue = lua_setupvalue(L, -2, 1);

  /* Action purpose: lua_setupvalue pops nothing when it does not take the
  value, so the environment is dropped here and not above. The name is checked
  as well, because a chunk's first upvalue being _ENV is what makes one
  substitution cover the whole file -- and a sandbox that silently attached
  itself to some other upvalue would be one that fails open. */
  if (upvalue == NULL) {
    lua_pop(L, 1);

    return false;
  }

  return strcmp(upvalue, "_ENV") == 0;
}

void
saber_lua_unbound(lua_State *L)
{
  void *budget = NULL;

  lua_getallocf(L, &budget);

  /* Action purpose: The cap exists to bound what the CONFIGURATION may ask
  for, and once the chunk has returned there is no configuration running --
  only this tree's reader, which pushes short keys and runs with no protected
  frame below it. Leaving the cap armed would let a chunk that parked 64 MiB of
  live data turn the reader's first key push into a LUA_ERRMEM, and an
  unprotected raise is abort(): the panel would have traded a hang for a
  crash. So the ceiling is lifted at exactly the point the thing it guards
  against has stopped running, and the accounting carries on so that lua_close
  still balances. */
  ((struct guard_budget *)budget)->cap = (size_t)-1;

  /* The work ceiling goes with it, for the same reason: what it bounds is the
  configuration, and the configuration has finished. Zero means no ceiling. */
  ((struct guard_budget *)budget)->work_cap = 0;
}

void
saber_lua_close(lua_State *L)
{
  void *budget = NULL;

  if (L == NULL) {
    return;
  }

  lua_getallocf(L, &budget);
  lua_close(L);
  free(budget);
}
