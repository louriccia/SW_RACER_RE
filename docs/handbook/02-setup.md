# 02 — Setting Up Your Environment

This guide gets you from a fresh machine to "I can build `swr_reimpl.dll`, inject it,
and watch the game run." The matching (Visual C++ 5.0) toolchain at the end is
**optional** — you only need it if you want to chase byte-exact matches.

> Historical note: the repo's `Makefile` hardcodes one maintainer's paths
> (`/home/tim/.wine/...`) and several targets are stubs (`config`, `compile`, `compare`,
> `send`). Treat the `Makefile` as reference notes, not a turnkey build. The supported,
> reproducible build is `compile.bat`. Where this guide references the matching
> toolchain, substitute your own Wine prefix (referred to below as `$SWE1R_WINEPREFIX`).

## 1. The game

You need a legitimate copy of `SWEP1RCR.EXE`. Supported builds (the loader MD5-checks
these):

| Version | MD5 |
|---------|-----|
| GOG | `e1fcf50c8de2dbef70e6ad8e09371322` |
| Steam | `adbef6bc9747c087485fce8a48f5eca4` |

## 2. Core toolchain (required)

- **mingw-w64** — `gcc` / `g++` (32-bit target). On Windows, MSYS2 is the easy path
  (`pacman -S mingw-w64-i686-gcc`); on Linux, `sudo apt install gcc-mingw-w64`.
- **Python 3** + **Jinja2** (`pip install jinja2`) — for the symbol/header generators in
  `scripts/`.
- **git** — and pull the submodules (OpenJKDF2 is your cross-reference oracle):
  ```
  git submodule update --init --recursive
  ```

Build sanity check:
```
compile.bat
```
This should produce `build/loader.exe` and `build/swr_reimpl.dll`.

## 3. Ghidra + ret-sync (required for analysis)

- **Ghidra 10.2.2** specifically — the bundled **ret-sync** plugin is matched to this
  version (`scripts/Ghidra/ghidra_10.2.2_PUBLIC_20230910_retsync.zip`). ret-sync lets
  Ghidra follow along live with the debugger.
- **x32dbg** — the 32-bit runtime debugger that ret-sync pairs with.

### Import the existing symbol database into Ghidra

So your database starts already named (instead of a sea of `FUN_xxxxxxxx`):

1. In Ghidra: **File → Parse C Source** on `src/types.h` (this loads all the struct
   definitions the other scripts depend on).
2. Run the scripts in `scripts/Ghidra/`:
   - `ImportHeaderInfos.py` — imports function prototypes from the module headers.
   - `importDataSymbols.py` — imports the 847 global/data symbols from
     `data_symbols.syms`.
   - `GenerateMasterHeader.py` — generates a consolidated header when you need one.

### Measuring progress

Export the function list from Ghidra to CSV, then:
```
python scripts/ParseFunctionsCSV.py functions.csv
```
It reports the percentage of functions that are named (anything not starting with
`FUN_`).

## 4. Matching toolchain — Visual C++ 5.0 under Wine (optional, stretch)

Only needed if you want to verify that your C compiles to the **same bytes** as the
original. The original game was built with **Visual C++ 5.0**.

1. **Get the compiler**: the `vcpp5` ISO (e.g.
   <https://winworldpc.com/product/visual-c/5x>). During install the product key is
   `111-11111`; use project name `SW_RE` to match the existing notes.
2. **Install it into a Wine prefix** you control. Export its location once so nothing is
   hardcoded:
   ```
   export SWE1R_WINEPREFIX="$HOME/.wine"   # adjust to your prefix
   ```
3. **The match cycle** (currently manual — the `Makefile` only stubs it):
   - **send**: copy `src/*.c` and `src/*.h` into the VC++ project dir under your prefix
     (the `Makefile send:` target shows the shape; replace `/home/tim/.wine` with
     `$SWE1R_WINEPREFIX`).
   - **compile**: drive `CL.EXE` (run `VCVARS32.BAT` first) or build via `MSDEV.EXE` in
     Wine. In the IDE: *File → Open Workspace → SW_RE.DSW*, then right-click the project
     and *Add Files to Project* for any new source.
   - **get_binary**: copy the built `.exe`/`.obj` back out.
   - **compare**: diff your compiled output against the original. **This step has no
     tool wired up yet** — the `Makefile compare:` target is a literal `TODO`.

### The known gap (and the intended fix)

There is no automated match metric today; progress is tracked by naming, not bytes. The
plan is to adopt a local diff tool suited to **x86 / MSVC** binaries —
[`objdiff`](https://github.com/encounter/objdiff) (supports x86, MSVC demangling, and
COFF objects) as the primary tool, with `asm-differ` for quick terminal inspection — to
turn the `compare:` stub into a tight, measurable loop. Until that lands, "matched" is
verified by eye. This is future work, not required for a contribution.

## 5. Acceptance

You're set up when you can:
- run `compile.bat` and get both artifacts,
- copy them next to `SWEP1RCR.EXE`, run `loader.exe`, and see the game launch with the
  injected DLL,
- open the binary in Ghidra with the symbols imported (named functions, not `FUN_`).

Next: [01-workflow.md](./01-workflow.md) for the per-function loop, or
[03-ai-assisted.md](./03-ai-assisted.md) to do it faster with Claude.
