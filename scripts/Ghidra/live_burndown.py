#!/usr/bin/env python3
"""
LIVE RE burndown for SW_RACER_RE -- no machine / no GUI / no stale export needed.

The GhidraMCP plugin's /list_functions HTTP endpoint returns every function as
"name at ADDRESS" straight from the live DB, so this reflects the current DB
(import + all in-session renames) -- unlike scripts/Ghidra/master_functions.h,
which is a static ExportFunctionList.py snapshot that goes stale the moment any
rename/import happens.

USAGE (fully remote, Ghidra just has to be running with the plugin on :8080):
    # 1. pull the live list (PowerShell):
    #    Invoke-RestMethod -Uri http://127.0.0.1:8080/list_functions -TimeoutSec 30 |
    #      Out-File -Encoding ascii scripts/Ghidra/live_functions.txt
    #    (or: curl -s http://127.0.0.1:8080/list_functions > scripts/Ghidra/live_functions.txt)
    # 2. analyze:
    python scripts/Ghidra/live_burndown.py
    python scripts/Ghidra/live_burndown.py --min-cluster 4 --top 30

live_functions.txt is .gitignore'd local data (like master_functions.h).
"""
import re, os, sys, argparse

HERE = os.path.dirname(os.path.realpath(__file__))
DEFAULT = os.path.join(HERE, "live_functions.txt")
line_re = re.compile(r'^(.*?)\s+at\s+([0-9a-fA-F]{6,8})\s*$')
unnamed = lambda n: n.startswith("FUN_") or n.startswith("thunk_FUN_")

def parse(path):
    rows = []
    for l in open(path, encoding="ascii", errors="replace"):
        m = line_re.match(l.strip())
        if m:
            rows.append((int(m.group(2), 16), m.group(1).strip()))
    rows.sort()
    return rows

def cluster(rows, gap=0x1000):
    clusters, cur, last = [], None, None
    for a, n in rows:
        if not unnamed(n):
            last = n
            if cur and a - cur["end"] >= gap:
                clusters.append(cur); cur = None
            continue
        if cur is None:
            cur = {"start": a, "end": a, "count": 1, "hint": last}
        elif a - cur["end"] < gap:
            cur["end"] = a; cur["count"] += 1
        else:
            clusters.append(cur); cur = {"start": a, "end": a, "count": 1, "hint": last}
    if cur: clusters.append(cur)
    return clusters

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT)
    ap.add_argument("--min-cluster", type=int, default=4)
    ap.add_argument("--gap", type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument("--top", type=int, default=24)
    args = ap.parse_args()
    if not os.path.exists(args.src):
        sys.exit("missing %s -- pull it first: Invoke-RestMethod :8080/list_functions > %s"
                 % (args.src, os.path.basename(args.src)))
    rows = parse(args.src)
    total = len(rows)
    fun = [r for r in rows if unnamed(r[1])]
    nN = total - len(fun)
    print("=== LIVE burndown (%s) ===" % os.path.basename(args.src))
    print("total %d | named %d (%.1f%%) | unnamed FUN_ %d (%.1f%%)"
          % (total, nN, 100*nN/total, len(fun), 100*len(fun)/total))
    cl = [c for c in cluster(rows, args.gap) if c["count"] >= args.min_cluster]
    cl.sort(key=lambda c: c["count"], reverse=True)
    print("\ntop unnamed clusters (>=%d, gap<%s), by size:" % (args.min_cluster, hex(args.gap)))
    print("  %5s  %-23s %s" % ("count", "range", "near (subsystem hint)"))
    for c in cl[:args.top]:
        print("  %5d  0x%06x-0x%06x  %s" % (c["count"], c["start"], c["end"], c["hint"] or "(top)"))
    print("\n%d clusters >= %d; %d of %d unnamed shown"
          % (len(cl), args.min_cluster, sum(c["count"] for c in cl[:args.top]), len(fun)))

if __name__ == "__main__":
    main()
