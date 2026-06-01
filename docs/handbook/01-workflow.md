# 01 — The Contributor Workflow

This is the day-to-day loop for taking one unknown function from raw disassembly to a
named, re-implemented, hooked function. The running example is
`rdVector_Add2` (see `src/Primitives/rdVector.c` and `rdVector.h`).

## The loop at a glance

```
pick FUN_xxxxxxxx  ->  identify it  ->  triple-entry edit  ->  build  ->  inject & observe  ->  PR
   (Ghidra)          (OpenJKDF2)      (.h / .c / hook.c)    (compile.bat)   (loader.exe)
```

## 1. Pick a function

In Ghidra, open the Symbol Tree / Functions list and find an unnamed `FUN_xxxxxxxx`.
Good first targets are **leaf functions** (call few or no other functions) in modules
that are already partly done — the math and primitive modules are easiest. Read the
decompiled pseudocode and the disassembly side by side.

> Tip: the largest, least-structured asset in this repo is the function
> name↔address↔signature mapping. Every function you name removes a `FUN_` and nudges
> the progress metric.

## 2. Identify it (Sith-engine cross-reference)

SWE1R is the **Sith engine**, so most non-game-specific functions have a twin in
[OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) (checked out under
`modules/OpenJKDF2`). Module prefixes you will see everywhere — `rd*` (renderer),
`std*` (standard library / platform), `sith*` (engine core) — match OpenJKDF2's naming.
Look up the homonym there to recover the **name, signature, calling convention, and
struct field meanings**. `NOTES.md` records several confirmed matches (e.g. the
`stdDisplay` error string, the `stdPlatform` abstraction). Game-specific logic lives
under the `swr*` prefix (`src/Swr/`, `src/Main/`) and has no OpenJKDF2 twin — those you
reverse from scratch.

> The fast path for both steps 1 and 2 is in [03-ai-assisted.md](./03-ai-assisted.md).

## 3. The triple-entry edit

A re-implemented function lives in **three places that must stay in sync**. Using
`rdVector_Add2` (address `0x0042f6e0`) as the template:

**(a) Module header** — `src/Primitives/rdVector.h`: the address macro **and** the
prototype.

```c
#define rdVector_Add2_ADDR (0x0042f6e0)
...
rdVector2* rdVector_Add2(rdVector2* v1, const rdVector2* v2, const rdVector2* v3);
```

**(b) Implementation** — `src/Primitives/rdVector.c`: the body, with the address as a
leading comment (this is the repo convention — it lets a reader jump back to Ghidra).

```c
// 0x0042f6e0
rdVector2* rdVector_Add2(rdVector2* v1, const rdVector2* v2, const rdVector2* v3)
{
    v1->x = v2->x + v3->x;
    v1->y = v2->y + v3->y;
    return v1;
}
```

**(c) Hook registration** — `src/hook.c`, inside `hook_init()`: one line that patches
the original function's prologue with a `jmp` to your implementation.

```c
hook_function(rdVector_Add2_ADDR, (uint8_t*)rdVector_Add2);
```

`hook_function()` (in `src/hook.c`) ASLR-rebases the address, then overwrites the first
5 bytes of the original function with `0xE9 rel32` (a near jump) so the game calls your
code instead. Functions left commented-out in `hook_init()` are the TODO backlog.

### Conventions

- **Naming**: `Module_FunctionName` (`rdVector_Add2`, `stdMath_Sqrt`). Match OpenJKDF2
  spelling where a twin exists.
- **Addresses**: lowercase hex, the full `0x004xxxxx` form, in both the `_ADDR` macro
  and the `// ` comment above the body.
- **Types / globals**: new structs go in `src/types.h`; new global variables go in
  `data_symbols.syms` (format: `name 0xADDR Type [= value]`) and are regenerated into
  `globals.c/.h` via `scripts/GenerateGlobalHeaderFromSymbols.py`.
- **Calling conventions**: note `__thiscall` / `__fastcall` where they apply — they
  matter for both correctness and (eventually) matching. See `NOTES.md` for the x86
  calling-convention notes.

## 4. Build

`compile.bat` builds both artifacts with mingw:

- `loader.exe` (from `loader/loader.cpp` + `loader/md5.c`)
- `swr_reimpl.dll` (your re-implemented functions)

into `./build`. The DLL's source list is the `SOURCES=` line in `compile.bat` — **if you
add a new `.c` file, add it there**.

## 5. Inject and observe

Copy **both** `build/loader.exe` and `build/swr_reimpl.dll` — *without renaming them* —
next to `SWEP1RCR.EXE`, then run `loader.exe`. The loader starts the game suspended,
injects the DLL (`CreateProcessA` + `CreateRemoteThread` + `LoadLibraryA`), and resumes
it. `hook_init()` installs your hooks on load. The loader MD5-checks the executable, so
use a supported build:

| Version | MD5 |
|---------|-----|
| GOG | `e1fcf50c8de2dbef70e6ad8e09371322` |
| Steam | `adbef6bc9747c087485fce8a48f5eca4` |

**Today the only behavioural test is launching the game and watching it.** There is no
automated match/regression check yet — the `Makefile compare:` target is an
unimplemented `TODO`. (Closing that gap with `objdiff`/`asm-differ` is tracked as future
work; see [02-setup.md](./02-setup.md).)

## 6. What a pull request looks like

A typical PR diff is small and shaped like this:

- `src/<Module>/<file>.h` — new `_ADDR` macro(s) + prototype(s)
- `src/<Module>/<file>.c` — the implementation(s) with `// 0xADDR` comments
- `src/hook.c` — new `hook_function(...)` line(s)
- *(optional)* `src/types.h` — struct/field additions
- *(optional)* `data_symbols.syms` — new globals
- *(optional)* `compile.bat` — new source file added to `SOURCES=`

Include in the description: what the function does, how you identified it (OpenJKDF2
twin? reversed from scratch?), and any uncertainty (guessed field names, unknown
calling convention). Attribute OpenJKDF2-derived names.

### Sharp edges to expect (so you aren't surprised)

- The three entries (header / `.c` / `hook.c`) are kept in sync **by hand** — a typo'd
  address fails silently or crashes the game.
- No fast feedback: every test is a full game launch.
- The matching (VC++ 5.0) toolchain is optional and fiddly — see the setup guide.

The AI-assisted workflow ([03-ai-assisted.md](./03-ai-assisted.md)) exists specifically
to take the tedium out of steps 1–3.
