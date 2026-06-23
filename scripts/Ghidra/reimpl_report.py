#!/usr/bin/env python3
"""
Reimplementation completeness report for SW_RACER_RE.

Unlike coverage_report.py / live_burndown.py (which measure how many functions are
NAMED), this measures how many are actually REIMPLEMENTED in C vs. carved-and-stubbed.

A function is a "reimpl slot" if a `// 0xADDR` reverse-hook comment sits directly above
its signature in a non-generated src/ .c/.cpp file. Each slot is classified by body:
  - full      : real body, no HANG()
  - pure stub : body is essentially just `HANG("TODO"); return ...;`  (<= 3 statements)
  - partial   : mostly implemented but at least one branch still HANG()s

GOTCHA this script exists to avoid: the stub marker is the HANG() macro, NOT the word
"stub" -- grepping for "stub" reports zero and is misleading.

Usage (from repo root):
    python scripts/Ghidra/reimpl_report.py                # summary + per-subsystem table
    python scripts/Ghidra/reimpl_report.py --list-stubs   # also dump every pure-stub name
    python scripts/Ghidra/reimpl_report.py --by-file      # per-file instead of per-top-dir

If scripts/Ghidra/live_functions.txt exists (pull it with
`curl http://127.0.0.1:8080/list_functions > scripts/Ghidra/live_functions.txt`),
the overall named-vs-reimplemented funnel is printed too.
"""
import re, os, sys, argparse

HERE = os.path.dirname(os.path.realpath(__file__))
SRC = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src") \
    if os.path.basename(os.path.dirname(HERE)) else "src"
if not os.path.isdir(SRC):
    SRC = "src"

ADDR_CMT = re.compile(r'^\s*//\s*0x[0-9a-fA-F]{6,8}(\s+HOOK)?\s*$')
SIG_NAME = re.compile(r'(\w+)\s*\(')


def scan_slots():
    """Yield (filepath, name, signature, kind) for every reimpl slot."""
    for dp, _, fns in os.walk(SRC):
        if "generated" in dp.replace("\\", "/").split("/"):
            continue
        for f in sorted(fns):
            if not (f.endswith(".c") or f.endswith(".cpp")):
                continue
            path = os.path.join(dp, f)
            lines = open(path, encoding="ascii", errors="replace").read().splitlines()
            i = 0
            while i < len(lines):
                if not ADDR_CMT.match(lines[i]):
                    i += 1
                    continue
                j = i + 1
                sig = ""
                while j < len(lines) and "{" not in lines[j] and j < i + 6:
                    sig += lines[j] + " "
                    j += 1
                if j >= len(lines) or "{" not in lines[j]:
                    i += 1
                    continue
                sig += lines[j]
                depth, started, body, k = 0, False, [], j
                while k < len(lines):
                    for ch in lines[k]:
                        if ch == "{":
                            depth += 1; started = True
                        elif ch == "}":
                            depth -= 1
                    body.append(lines[k])
                    if started and depth == 0:
                        break
                    k += 1
                btxt = "\n".join(body)
                inner = btxt[btxt.find("{") + 1: btxt.rfind("}")]
                code = [l.strip() for l in inner.splitlines()
                        if l.strip() and not l.strip().startswith("//")]
                if "HANG(" not in btxt:
                    kind = "full"
                elif len(code) <= 3:
                    kind = "pure_stub"
                else:
                    kind = "partial"
                nm = SIG_NAME.search(sig)
                yield (path, nm.group(1) if nm else "?", sig.strip(), kind)
                i = k + 1


def funnel():
    lf = os.path.join(HERE, "live_functions.txt")
    if not os.path.exists(lf):
        return None
    is_fun = lambda n: n.startswith("FUN_") or n.startswith("thunk_FUN_")
    line = re.compile(r'^(.*?)\s+at\s+([0-9a-fA-F]{6,8})\s*$')
    total = named = 0
    for l in open(lf, encoding="ascii", errors="replace"):
        m = line.match(l.strip())
        if not m:
            continue
        total += 1
        if not is_fun(m.group(1).strip()):
            named += 1
    return total, named


def reg_count():
    g = os.path.join(SRC, "generated", "hook_generated.c")
    if not os.path.exists(g):
        return None
    return sum(l.count("hook_function(") for l in open(g, encoding="ascii", errors="replace"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list-stubs", action="store_true")
    ap.add_argument("--by-file", action="store_true")
    args = ap.parse_args()

    slots = list(scan_slots())
    tot = len(slots)
    if not tot:
        sys.exit("no reimpl slots found -- run from repo root?")
    kinds = {"full": 0, "pure_stub": 0, "partial": 0}
    groups = {}  # group -> {kind: n}
    for path, name, sig, kind in slots:
        kinds[kind] += 1
        rel = os.path.relpath(path, SRC).replace("\\", "/")
        key = rel if args.by_file else rel.split("/")[0]
        groups.setdefault(key, {"full": 0, "pure_stub": 0, "partial": 0})[kind] += 1

    print("=== SW_RACER_RE reimplementation report ===")
    fn = funnel()
    if fn:
        t, n = fn
        print("functions in DB : %d | named %d (%.1f%%)" % (t, n, 100 * n / t))
        print("reimpl slots    : %d (%.1f%% of DB)" % (tot, 100 * tot / t))
        print("FULLY reimpl    : %d (%.1f%% of DB)" % (kinds["full"], 100 * kinds["full"] / t))
    rc = reg_count()
    if rc is not None:
        print("hook_generated.c registrations : %d" % rc)
    print()
    print("reimpl slots           : %d" % tot)
    print("  full (real body)     : %d  (%.1f%%)" % (kinds["full"], 100 * kinds["full"] / tot))
    print("  pure stub (HANG)     : %d  (%.1f%%)" % (kinds["pure_stub"], 100 * kinds["pure_stub"] / tot))
    print("  partial (HANG branch): %d  (%.1f%%)" % (kinds["partial"], 100 * kinds["partial"] / tot))

    print("\n%-26s %6s %6s %6s %6s" % ("group", "full", "stub", "part", "total"))
    for key in sorted(groups, key=lambda k: -(groups[k]["pure_stub"])):
        g = groups[key]
        t = g["full"] + g["pure_stub"] + g["partial"]
        print("%-26s %6d %6d %6d %6d" % (key[:26], g["full"], g["pure_stub"], g["partial"], t))

    if args.list_stubs:
        print("\n=== pure stubs (need bodies) ===")
        cur = None
        for path, name, sig, kind in slots:
            if kind != "pure_stub":
                continue
            rel = os.path.relpath(path, SRC).replace("\\", "/")
            if rel != cur:
                cur = rel
                print("\n%s:" % rel)
            print("  %s" % name)


if __name__ == "__main__":
    main()
