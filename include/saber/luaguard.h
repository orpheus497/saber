/* Script function and purpose: The one place a Lua interpreter is built for
reading a configuration file, and the ceilings it is built with. Saber reads
two Lua files -- its own saber.lua and the compositor's hikari.lua, for the
palette and the animation duration -- and the two readers are in different
translation units with nothing in common but this.

They share this and not their sandboxes because the sandboxes legitimately
differ: saber.lua gets the undefined-name diagnostic a user migrating a file by
hand needs, and somebody else's hikari.lua does not. What is shared is the
part that has to be identical, and the part where a second copy would be a
second place to get it wrong: a memory ceiling, the string library's two
C-side loops bounded, and a setup that runs protected. */

#if !defined(SABER_LUAGUARD_H)
#define SABER_LUAGUARD_H

#include <stdbool.h>
#include <stddef.h>

#include <lua.h>

/* Action purpose: what a configuration may allocate, declared here because a
reader that refuses a file for exceeding it has to be able to say so. Measured:
a bare interpreter with its four libraries and no file read costs 14636 bytes, the
shipped saber.lua peaks at 19895, and hikari-sakura's own 51 KB hikari.lua at
62000 -- so most of what a real configuration costs is the interpreter and not
the file, and this is three orders of magnitude of headroom. Against it, a
single string.rep reached 2047 MiB here before the ceiling existed. */
#define SABER_LUA_MEMORY_CAP ((size_t)64 * 1024 * 1024)

/* Action purpose: how long a configuration chunk may run, and how often the
count hook looks at the clock to find out.

WHY A CLOCK AND NOT MORE COUNTERS. The wrappers in src/luaguard.c charge the
two string operations whose work happens inside one C call, and that bounds
those two. It does not bound the operations whose work happens inside one VM
INSTRUCTION, which is a different set and a larger one: the `..` operator copies
both operands inside luaV_concat, and string.upper, string.lower,
string.reverse, table.concat and the output side of string.gsub each copy a
whole string in one C call the hook does not stand between. Measured against
this tree, a chunk of

  local s = string.rep('x', 1048576)
  for i = 1, 1e9 do local _ = s .. s end

ran 77.7 seconds before the instruction ceiling stopped it, because a handful of
VM instructions per iteration copied two mebibytes and freed them again, so
neither the instruction hook nor the memory ceiling was anywhere near firing.
string.upper on the same string passed five minutes. For a panel that is not
appearing at startup and a `saberctl reload` that hangs.

Charging each of those operations separately does not close the class -- it
closes the members somebody thought of, which is how the two ceilings above came
to be necessary in the first place. Elapsed time closes the class, because the
question a configuration reader actually has is whether the file finished, not
how it spent itself.

SABER_LUA_SAMPLE is what makes the ceiling tight rather than nominal, and it is
measured rather than guessed. The deadline can only be noticed when the hook
runs, so the overshoot is the sample count times the most expensive single
instruction the memory ceiling allows -- and that ceiling is what bounds it,
since a table.concat wants its operands and its result live at once: at four
mebibytes a side the memory ceiling refuses the chunk outright, and the worst
instruction that survives copies sixteen. A sample of a thousand let that chunk
run 3.3 seconds against a one-second ceiling, measured over six runs -- a
nominal ceiling rather than a real one, and the first shape tried gave 1.4
seconds, so the number to design against was not the one that showed up first.
A sample of a hundred holds the same chunk to 1.0-1.3 seconds. Tightening past
the first sample cannot help, because the first sample is already the
overshoot.

The cost is paid per instruction and is worth stating honestly rather than
waved at. Measured as the minimum of two hundred runs of a tight arithmetic
loop of about 1.8 million instructions: 2.27 ms with no hook, 5.23 ms armed, so
the hook costs 3.0 ms there and slightly more than doubles that loop. It is not
free and it is not noise. What makes it affordable is the comparison it belongs
in: 3.0 ms is three parts in a thousand of the ceiling it enforces, and the
only file that pays it in full is one doing arithmetic no configuration does.
The shipped file runs a few hundred instructions, hikari-sakura's own 51 KB file
well under a millisecond. Shrinking the sample further buys overshoot nobody
would notice at a cost that is already visible. */
#define SABER_LUA_TIME_CAP_MS 1000
#define SABER_LUA_SAMPLE 100

/* Function purpose: Build an interpreter holding only the four libraries a
configuration has a use for, with the restricted environment `names` describes
stored ready for saber_lua_enter(). `undefined` is installed as that
environment's __index, so a read of a name it does not hold is an error rather
than nil; pass NULL to leave an absent name reading as nil.

`environment` adds hikari's `env`, the read-only table a configuration reads
environment variables through. It is a parameter and not a name in `names`
because there is no global of that name to copy -- it is constructed -- and
because only the reader of hikari's own file owes it: hikari provides one, its
shipped hikari.lua uses it, and a sandbox without one fails that file outright
rather than merely losing a value. saber.lua has never had one.

NULL is returned when the interpreter could not be built, which is out of
memory and nothing else. The state must be released with saber_lua_close() and
not with lua_close(), which would leak its accounting. */
lua_State *saber_lua_open(const char *const names[], lua_CFunction undefined,
    bool environment);

/* Function purpose: Give the chunk on top of the stack the restricted
environment in place of Lua's own globals. _ENV is a chunk's first upvalue, so
this one substitution covers the whole file rather than a list of deletions
applying only where they were made. The chunk stays on top either way; false
means it was not a chunk, and the caller owes it a pop. */
bool saber_lua_enter(lua_State *L);

/* Function purpose: Install `hook` and start the clock and the instruction
count for one chunk, called immediately before the lua_pcall that runs it.
`step_cap` is the reader's own instruction ceiling, which the hook now reaches
in SABER_LUA_SAMPLE-sized pieces rather than in one, so that the clock can be
looked at on the way.

The hook is installed here rather than by the reader because the sample count
and the charging have to agree, and a reader that armed its own count would be
a second place to get that wrong. The two ceilings themselves live in the budget
the allocator already carries -- the one thing both readers share, and the only
thing a hook can find from the state it is handed. */
void saber_lua_arm(lua_State *L, lua_Hook hook, unsigned long step_cap);

/* Function purpose: Charge one hook sample and say which ceiling has been
passed, or NULL while both still hold. The return is a string literal, so there
is nothing to free and nothing to outlive; a reader puts it in the diagnostic
Lua raises, so a user who has to fix the file is told which of the two ran out
rather than only that something did.

This is the whole body of both readers' hooks. It charges, so it is called once
per hook call and not as a question asked twice. */
const char *saber_lua_tick(lua_State *L);

/* Function purpose: Stop counting allocations, once the configuration chunk
has finished and the only code left to run is this tree's own reader. The time
and instruction ceilings are lifted with it, for the same reason: they are for
the file, not for the reading of it. */
void saber_lua_unbound(lua_State *L);

void saber_lua_close(lua_State *L);

#endif
