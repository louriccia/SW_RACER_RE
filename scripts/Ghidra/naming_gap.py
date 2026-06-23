#!/usr/bin/env python3
"""
Naming-gap report: functions NAMED in the live Ghidra DB but NOT yet declared in any
src/ header. These are the "named locally, not upstream" backlog -- candidates to write
up as `_ADDR` + prototype declarations and PR.

Needs a fresh DB dump:
    curl http://127.0.0.1:8080/list_functions > scripts/Ghidra/live_functions.txt

Then:
    python scripts/Ghidra/naming_gap.py            # grouped by subsystem -> target header
    python scripts/Ghidra/naming_gap.py --noise     # also list CRT/runtime (out of scope)

Caveats this script flags:
  - DUP names (same name at >1 address) -- usually a thunk named like its target; resolve
    before declaring (tim's dup-scan rejects them).
  - The DB has names but often NOT real prototypes (signature shows `undefined NAME(void)`),
    so each function must be decompiled to determine its true return type/params.
"""
import re, os, glob, argparse

HERE = os.path.dirname(os.path.realpath(__file__))
is_fun = lambda n: n.startswith("FUN_") or n.startswith("thunk_FUN_")


def db_named():
    lf = os.path.join(HERE, "live_functions.txt")
    line = re.compile(r'^(.*?)\s+at\s+([0-9a-fA-F]{6,8})\s*$')
    out = []
    for l in open(lf, encoding="ascii", errors="replace"):
        m = line.match(l.strip())
        if m and not is_fun(m.group(1).strip()):
            out.append((m.group(2).lower().zfill(8), m.group(1).strip()))
    return out


def named_in_source():
    defn = re.compile(r'#define\s+(\S+?)(?:_ADDR)?\s+\(0x([0-9a-fA-F]{8})\)')
    s = set()
    for dp, _, fns in os.walk("src"):
        for f in fns:
            if f.endswith(".h"):
                for l in open(os.path.join(dp, f), encoding="ascii", errors="replace"):
                    m = defn.search(l)
                    if m and not is_fun(m.group(1)):
                        s.add(m.group(2).lower())
    return s


def is_noise(name, addr):
    a = int(addr, 16)
    if name.startswith(("stdlib_", "__")) or name == "entry":
        return True
    if name.startswith(("nullsub", "thunk")) or re.match(r'^_[a-z]', name):
        return True
    if 0x0049e000 <= a <= 0x0049f000:   # IAT / winapi imports
        return True
    if a >= 0x004a0000:                 # CRT/startup tail
        return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--noise", action="store_true", help="also list CRT/runtime functions")
    args = ap.parse_args()

    src = named_in_source()
    gap = sorted((a, n) for a, n in db_named() if a not in src)
    game = [(a, n) for a, n in gap if not is_noise(n, a)]
    noise = [(a, n) for a, n in gap if is_noise(n, a)]

    name_count = {}
    for a, n in game:
        name_count[n] = name_count.get(n, 0) + 1

    hdr = {}
    for p in glob.glob("src/**/*.h", recursive=True):
        hdr.setdefault(os.path.basename(p)[:-2], p.replace("\\", "/"))

    groups = {}
    for a, n in game:
        pre = n.split("_")[0] if "_" in n else "(unprefixed)"
        groups.setdefault(pre, []).append((a, n))

    print("naming gap (DB named, not in source): %d  [game %d | CRT/runtime %d]"
          % (len(gap), len(game), len(noise)))
    dups = {n: c for n, c in name_count.items() if c > 1}
    if dups:
        print("DUP names (resolve before PR -- likely thunks): %s" % ", ".join(sorted(dups)))
    print("\n=== game functions by prefix -> target header ===")
    for pre in sorted(groups, key=lambda k: -len(groups[k])):
        fns = sorted(groups[pre])
        h = hdr.get(pre)
        if not h and pre.startswith("swrObj"):
            h = hdr.get("swrObj")
        if not h and pre == "swrScene":
            h = hdr.get("swrObj")
        print("\n[%s]  %d fn  -> %s" % (pre, len(fns), h or "??? assign by address"))
        for a, n in fns:
            tag = "  <DUP>" if name_count[n] > 1 else ""
            print("   0x%s  %s%s" % (a, n, tag))

    if args.noise:
        print("\n=== CRT / runtime / imports (out of scope) ===")
        for a, n in noise:
            print("   0x%s  %s" % (a, n))


if __name__ == "__main__":
    main()
