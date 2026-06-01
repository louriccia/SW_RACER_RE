# SWE1R Reverse Engineer Handbook

A practical guide for contributing to the Star Wars Episode I: Racer (`SWEP1RCR.EXE`)
decompilation. If you have never touched this repo before, start here.

## What are we working toward?

The mission is a **fully-named, well-understood engine** — so that anyone building a
mod (whether a DLL-injection mod in this repo, the [annodue](https://github.com/louriccia/annodue)
Zig plugin platform, or [swe1r-patcher](https://github.com/louriccia/swe1r-patcher))
can refer to a function, struct, or global **by name** instead of re-discovering raw
addresses from scratch. Today those addresses are re-found and hardcoded independently
in three different projects; every function we name and document here is a fact the
whole ecosystem can stop re-discovering.

SWE1R runs the **Sith engine** (the same LucasArts engine as Jedi Knight: Dark Forces 2,
Grim Fandango, and Indiana Jones and the Infernal Machine). That is why this project is
modeled on, and ships [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) as a
submodule — it is a near-complete decompilation of the same engine family and our single
best oracle for names, signatures, and struct layouts.

## The progress model

A function moves through three stages:

| Stage | Meaning | How |
|-------|---------|-----|
| **Named** | The function has a real name + signature in Ghidra and a `#define _ADDR` + prototype in a module header. | Ghidra analysis, OpenJKDF2 cross-ref |
| **Re-implemented (functional)** | A C body exists that behaves identically; built with mingw and hooked into the live game via `hook.c`. | The triple-entry loop (see workflow) |
| **Matched (stretch)** | The C compiles to byte-identical machine code with the original compiler (Visual C++ 5.0). | VC++ 5.0 + a diff tool — **stretch goal, not a gate** |

Progress today is reported as **% of functions named** (not `FUN_xxxxxxxx`) — currently
**30.54% (656 / 2148)** — measured by `scripts/ParseFunctionsCSV.py` on a Ghidra function
export. You do **not** need byte-matching for a contribution to be valuable: every
shipping mod is DLL injection, so correct **names, signatures, struct layouts, and
addresses** are what matter most. Matching is a correctness oracle and preservation
goal we pursue opportunistically.

## The three guides

1. **[01-workflow.md](./01-workflow.md)** — the per-function loop, the triple-entry
   pattern, build & test, and what a pull request looks like.
2. **[02-setup.md](./02-setup.md)** — getting a working environment (Ghidra + ret-sync,
   mingw, and the optional VC++ 5.0 matching toolchain).
3. **[03-ai-assisted.md](./03-ai-assisted.md)** — using Claude / LLM tooling
   (GhidraMCP, pseudocode→C, OpenJKDF2 cross-ref) to do all of the above far faster.

## A good first contribution

Pick a small **leaf function** (one that calls little or nothing else) in a module that
is already partly named — the math/primitive modules (`src/Primitives/rdVector.c`,
`rdMatrix.c`, `src/General/stdMath.c`) are the friendliest worked examples. Name it,
implement it, hook it, confirm the game still runs. That is a complete, mergeable PR.
See `01-workflow.md` for the step-by-step.
