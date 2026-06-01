# 03 — AI-Assisted Reverse Engineering with Claude

The per-function loop in [01-workflow.md](./01-workflow.md) is mostly tedium: reading
pseudocode, recognising an engine function, hand-writing a C body, and keeping the
triple-entry in sync. Almost all of that is exactly the kind of work an LLM like Claude
is good at — *proposing*, while you *verify*. This guide shows where Claude plugs into
the loop and how to prompt it.

> **Golden rule: AI proposes, the human verifies.** Every name, type, and body Claude
> suggests must be checked against the disassembly and (ideally) confirmed in-game.
> Treat its output like a confident junior contributor's PR.

## A. Read Ghidra directly with GhidraMCP

The biggest single lever is connecting Claude to Ghidra through a **Model Context
Protocol (MCP) server** such as [GhidraMCP](https://lobehub.com/mcp/und3rf10w-ai-ghidra-tools).
With it, Claude can:

- request the decompiled C pseudocode of any function,
- analyse the logic and **propose a name + signature**,
- infer data-structure layouts from access patterns,
- and **rename symbols / set types programmatically** back in Ghidra.

That collapses steps 1–2 of the workflow (pick + identify) into a conversation. Set it
up once, point it at your Ghidra project, and you can ask things like *"summarise what
`FUN_0042f6e0` does and suggest a name and prototype."*

Related research/tools, if you want to go deeper:
[LLM4Decompile](https://github.com/albertan017/LLM4Decompile) (models trained to refine
Ghidra pseudocode) and [ReCopilot](https://arxiv.org/html/2505.16366v1) (function-name
and variable-type recovery).

## B. Pseudocode → matching C in the repo's idiom

Paste a Ghidra decompilation (and the raw asm if matching matters) and ask Claude to
write the body **in this repo's exact conventions**. A good prompt includes the
conventions so the output drops straight in:

> "Rewrite this Ghidra output as a clean C function for the SWE1R decomp. Conventions:
> name it `Module_FunctionName`; put `// 0xADDR` as the line above the function; use the
> types from `src/types.h` (e.g. `rdVector2`, `rdVector3`); prefer the style of
> `src/Primitives/rdVector.c`. Tell me the calling convention you inferred and why."

Claude is good at recovering the **signature** and spotting `__thiscall`/`__fastcall`
patterns, which matter for both correctness and matching.

## C. Automate the OpenJKDF2 cross-reference

Instead of hand-pasting GitHub URLs into `NOTES.md`, point Claude at the
`modules/OpenJKDF2` submodule and have it find the twin:

> "This is a Sith-engine function. Search `modules/OpenJKDF2` for the equivalent of
> this function. If you find it, give me its name, prototype, calling convention, and
> any relevant struct definitions, and note how confident you are that it's the same
> function."

Because OpenJKDF2 is a near-complete decomp of the same engine family, this transfers
**names, signatures, struct shapes, and semantics** cheaply. **Addresses never
transfer** — they differ per game; only the meaning does.

## D. Scaffold the triple-entry (kill the sync errors)

Once a function is identified, ask Claude to emit all three edits at once so you don't
typo an address across files:

> "For `rdVector_Add2` at `0x0042f6e0` with signature `rdVector2* rdVector_Add2(...)`,
> give me: (1) the `#define _ADDR` + prototype for `src/Primitives/rdVector.h`, (2) the
> `.c` implementation, and (3) the `hook_function(...)` line for `hook_init()` in
> `src/hook.c`. Remind me to add the file to `SOURCES=` in `compile.bat` if it's new."

This is the manual chore from [01-workflow.md](./01-workflow.md) §3 done in one shot,
consistently.

## E. The matching inner loop

When you're chasing a byte-exact match and the diff (from `objdiff`/`asm-differ`) isn't
clean, feed Claude both sides:

> "Here is my C, the target asm, and the asm my code currently produces. Suggest source
> changes that would close the diff (operand order, temporaries, signedness, loop
> shape) without changing behaviour."

Matching is largely a register-allocation/instruction-ordering puzzle; an LLM is a fast
source of "try this variation" ideas — but the **real VC++ 5.0 compiler is the only
judge**.

## F. Batch and background work

For bulk progress, use Claude (e.g. Claude Code, including on the web) to:

- name large numbers of trivial **leaf functions** from their pseudocode,
- fill in **types** for entries in `data_symbols.syms`,
- **review PRs** for address typos, missing `hook_function` lines, or a `.c` file not
  added to `compile.bat`'s `SOURCES=`,
- draft documentation and keep `NOTES.md` tidy.

## Cautions

- **Verify everything in-game.** A plausible-looking body that's subtly wrong will pass
  review and crash later.
- **Clean-room hygiene.** Analysing a binary you legally own via Ghidra is standard
  decompilation practice. Do **not** feed Claude leaked/original LucasArts source code —
  keep the project clean-room.
- **Addresses are per-binary.** Anything Claude pulls from OpenJKDF2 or elsewhere is a
  *name/shape* hint only; confirm the address in *this* binary.
- **Matching still needs the real compiler.** Claude can propose source variations but
  cannot certify a byte match — only VC++ 5.0 + a diff tool can.

Back to: [the workflow](./01-workflow.md) · [setup](./02-setup.md) · [handbook index](./README.md)
