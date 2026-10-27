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

/* What a configuration may allocate, declared here because a reader that
refuses a file for exceeding it has to be able to say so. Measured: a bare
interpreter with its four libraries and no file read costs 14636 bytes, the
shipped saber.lua peaks at 19895, and hikari-sakura's own 51 KB hikari.lua at
62000 -- so most of what a real configuration costs is the interpreter and not
the file, and this is three orders of magnitude of headroom. Against it, a
single string.rep reached 2047 MiB here before the ceiling existed. */
#define SABER_LUA_MEMORY_CAP ((size_t)64 * 1024 * 1024)

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

/* Function purpose: Stop counting allocations, once the configuration chunk
has finished and the only code left to run is this tree's own reader. */
void saber_lua_unbound(lua_State *L);

void saber_lua_close(lua_State *L);

#endif
