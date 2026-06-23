#!/usr/bin/env python3
"""
Dependency-aware reimplementation planner for SW_RACER_RE.

Joins the call graph with reimpl status to answer "what should I reimplement next,
and what exactly blocks the function I care about" using ACTUAL call edges -- unlike
coverage_report.py / live_burndown.py / reimpl_report.py, which sequence by address
proximity or directory only.

INPUTS (all local, all .gitignore'd dumps):
  scripts/Ghidra/call_edges.txt     call graph    (run ExportCallGraph.py in Ghidra)
  scripts/Ghidra/live_functions.txt addr -> name  (curl :8080/list_functions -- fresh)
                                    falls back to master_functions.h if absent
  src/**/*.{c,cpp}                  reimpl status  (reverse-hook `// 0xADDR` slots)

WHY THIS WORKS (verified architecture): src/Swr/*.c reimpls call siblings by PLAIN
NAME and are dormant reverse hooks (compiled + disasm-matched, NOT installed). So
disasm-matching a caller never needs its callee reimplemented -- you are not forced
bottom-up. The real costs this tool ranks are:
  (a) closing a subtree to fully-native C (every reachable game fn == `full`), and
  (b) live-test blockers: HANG stubs/partials reachable from a forward HOOK.

A callee is classified by its src reimpl slot body (same rule as reimpl_report.py):
  full   real body, no HANG()      stub  HANG() with <=3 stmts      partial  HANG branch
  none   no reimpl slot at all (just the original binary -- safe to call, not native)

USAGE (from repo root):
  python scripts/Ghidra/reimpl_plan.py                          # dashboard
  python scripts/Ghidra/reimpl_plan.py --blocks swrRace_Explode # closure worklist for a target
  python scripts/Ghidra/reimpl_plan.py --frontier               # un-full fns whose callees are all full
  python scripts/Ghidra/reimpl_plan.py --leverage               # un-reimpl leaves unblocking most callers
  python scripts/Ghidra/reimpl_plan.py --scc                    # recursion clusters (reimpl together)
"""
import re, os, sys, argparse

HERE = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))   # scripts/Ghidra -> repo root
SRC = os.path.join(ROOT, "src")
EDGES = os.path.join(HERE, "call_edges.txt")
LIVE = os.path.join(HERE, "live_functions.txt")
MASTER = os.path.join(HERE, "master_functions.h")

# game .text window; below 0x401000 is headers, >=0x49e000 is IAT + CRT tail.
GAME_LO, GAME_HI = 0x00401000, 0x0049e000

ADDR_CMT = re.compile(r'^\s*//\s*0x([0-9a-fA-F]{6,8})(\s+HOOK)?\s*$')
SIG_NAME = re.compile(r'(\w+)\s*\(')
TAG = {"full": "full ", "partial": "PART ", "stub": "STUB ", "none": "none "}


def load_edges():
    if not os.path.exists(EDGES):
        sys.exit("missing %s -- run ExportCallGraph.py in Ghidra first" % EDGES)
    succ = {}
    for l in open(EDGES, encoding="ascii", errors="replace"):
        l = l.rstrip("\n")
        if not l or l.startswith("#"):
            continue
        parts = l.split("\t")
        a = int(parts[0], 16)
        cs = set()
        if len(parts) > 1 and parts[1].strip():
            for c in parts[1].split(","):
                c = c.strip()
                if c:
                    cs.add(int(c, 16))
        succ.setdefault(a, set()).update(cs)
    return succ


def load_names():
    names = {}
    if os.path.exists(LIVE):
        line = re.compile(r'^(.*?)\s+at\s+([0-9a-fA-F]{6,8})\s*$')
        for l in open(LIVE, encoding="ascii", errors="replace"):
            m = line.match(l.strip())
            if m:
                names[int(m.group(2), 16)] = m.group(1).strip()
        return names, "live_functions.txt"
    if os.path.exists(MASTER):
        addr_re = re.compile(r'ADDR_0x([0-9a-fA-F]+)')
        pend = None
        for l in open(MASTER, encoding="ascii", errors="replace"):
            m = addr_re.search(l)
            if m:
                pend = int(m.group(1), 16)
                continue
            if pend is None:
                continue
            nm = SIG_NAME.search(l)
            if nm:
                names[pend] = nm.group(1)
            pend = None
        return names, "master_functions.h"
    return {}, None


def is_noise(name, addr):
    if name:
        if name.startswith(("stdlib_", "__")) or name == "entry":
            return True
        if name.startswith(("nullsub", "thunk")) or re.match(r'^_[a-z]', name):
            return True
    if 0x0049e000 <= addr <= 0x0049f000:
        return True
    if addr >= 0x004a0000:
        return True
    return False


def scan_slots():
    """addr -> reimpl kind (full / partial / stub), from reverse-hook slots in src/."""
    slots = {}
    for dp, _, fns in os.walk(SRC):
        if "generated" in dp.replace("\\", "/").split("/"):
            continue
        for f in sorted(fns):
            if not (f.endswith(".c") or f.endswith(".cpp")):
                continue
            lines = open(os.path.join(dp, f), encoding="ascii",
                         errors="replace").read().splitlines()
            i = 0
            while i < len(lines):
                m = ADDR_CMT.match(lines[i])
                if not m:
                    i += 1
                    continue
                addr = int(m.group(1), 16)
                j = i + 1
                while j < len(lines) and "{" not in lines[j] and j < i + 6:
                    j += 1
                if j >= len(lines) or "{" not in lines[j]:
                    i += 1
                    continue
                depth, started, body, k = 0, False, [], j
                while k < len(lines):
                    for ch in lines[k]:
                        if ch == "{":
                            depth += 1
                            started = True
                        elif ch == "}":
                            depth -= 1
                    body.append(lines[k])
                    if started and depth == 0:
                        break
                    k += 1
                btxt = "\n".join(body)
                inner = btxt[btxt.find("{") + 1: btxt.rfind("}")]
                code = [x.strip() for x in inner.splitlines()
                        if x.strip() and not x.strip().startswith("//")]
                if "HANG(" not in btxt:
                    kind = "full"
                elif len(code) <= 3:
                    kind = "stub"
                else:
                    kind = "partial"
                slots[addr] = kind
                i = k + 1
    return slots


class Graph:
    def __init__(self):
        self.succ_raw = load_edges()
        self.names, self.name_src = load_names()
        self.slots = scan_slots()
        self.gsucc, self.gpred, self.nodes = self._game_graph()

    def is_game(self, addr):
        return GAME_LO <= addr < GAME_HI and not is_noise(self.names.get(addr), addr)

    def _game_graph(self):
        nodes = set(a for a in self.names if self.is_game(a))
        for a, cs in self.succ_raw.items():
            if self.is_game(a):
                nodes.add(a)
            for c in cs:
                if self.is_game(c):
                    nodes.add(c)
        gsucc, gpred = {n: set() for n in nodes}, {n: set() for n in nodes}
        for a in nodes:
            for c in self.succ_raw.get(a, ()):
                if c in nodes and c != a:
                    gsucc[a].add(c)
                    gpred[c].add(a)
        return gsucc, gpred, nodes

    def status(self, addr):
        return self.slots.get(addr, "none")

    def label(self, addr):
        return self.names.get(addr) or ("FUN_%08x" % addr)

    def resolve(self, token):
        token = token.strip()
        if re.fullmatch(r'(0x)?[0-9a-fA-F]{6,8}', token):
            a = int(token, 16)
            if a in self.nodes:
                return a
        hits = sorted(a for a in self.nodes if self.names.get(a) == token)
        if not hits:
            sys.exit("not found as a game function: %s" % token)
        if len(hits) > 1:
            print("warning: %s is ambiguous (%s); using first"
                  % (token, ", ".join("0x%06x" % a for a in hits)))
        return hits[0]


def cmd_blocks(g, target, top):
    root = g.resolve(target)
    # reachable game subtree, following game->game edges only.
    reach, stack = set(), [root]
    while stack:
        n = stack.pop()
        if n in reach:
            continue
        reach.add(n)
        stack.extend(g.gsucc.get(n, ()))
    work = set(n for n in reach if g.status(n) != "full")
    counts = {"full": 0, "partial": 0, "stub": 0, "none": 0}
    for n in reach:
        counts[g.status(n)] += 1

    print("=== closure plan: %s (0x%06x) [status: %s] ==="
          % (g.label(root), root, g.status(root)))
    print("reachable game fns : %d  (full %d | partial %d | stub %d | not-started %d)"
          % (len(reach), counts["full"], counts["partial"], counts["stub"], counts["none"]))
    print("to fully close     : %d functions still need a real body" % len(work))
    if not work:
        print("\nsubtree is already fully native. Nothing to do.")
        return

    # leaf-first: a fn with fewer un-closed callees is cheaper / safer to do first.
    def blockers(n):
        return sum(1 for c in g.gsucc.get(n, ()) if c in work)

    ordered = sorted(work, key=lambda n: (blockers(n), g.status(n) != "stub", n))
    print("\nleaf-first worklist (blockers = un-closed callees still under it):")
    print("  %-5s %8s  %-10s %s" % ("kind", "blockers", "addr", "name"))
    for n in ordered[:top]:
        print("  %-5s %8d  0x%08x %s" % (TAG[g.status(n)], blockers(n), n, g.label(n)))
    if len(ordered) > top:
        print("  ... %d more (raise --top)" % (len(ordered) - top))

    live = sorted((c for c in g.gsucc.get(root, ()) if g.status(c) in ("stub", "partial")),
                  key=lambda n: n)
    if live:
        print("\nimmediate live-test blockers (HANG sibling called directly by target):")
        for n in live:
            print("  %-5s 0x%08x %s" % (TAG[g.status(n)], n, g.label(n)))


def cmd_frontier(g, top):
    rows = []
    for n in g.nodes:
        if g.status(n) == "full":
            continue
        callees = g.gsucc.get(n, ())
        if all(g.status(c) == "full" for c in callees):
            rows.append((len(g.gpred.get(n, ())), len(callees), n))
    rows.sort(reverse=True)
    print("=== frontier: un-full fns whose every game callee is already full ===")
    print("(reimplement-to-full with total downstream confidence; sorted by leverage)\n")
    print("  %-5s %7s %7s  %-10s %s" % ("kind", "callers", "callees", "addr", "name"))
    for callers, ncallees, n in rows[:top]:
        print("  %-5s %7d %7d  0x%08x %s"
              % (TAG[g.status(n)], callers, ncallees, n, g.label(n)))
    print("\n%d frontier functions (%d shown)" % (len(rows), min(top, len(rows))))


def cmd_leverage(g, top):
    rows = []
    for n in g.nodes:
        if g.status(n) == "full":
            continue
        callers = g.gpred.get(n, ())
        waiting = sum(1 for c in callers if g.status(c) in ("full", "partial"))
        if waiting:
            rows.append((waiting, len(callers), n))
    rows.sort(reverse=True)
    print("=== high-leverage leaves: un-full fns blocking the most real callers ===")
    print("(reimplementing one solidifies/unblocks many caller subtrees)\n")
    print("  %-5s %8s %7s  %-10s %s" % ("kind", "waiting", "callers", "addr", "name"))
    for waiting, callers, n in rows[:top]:
        print("  %-5s %8d %7d  0x%08x %s"
              % (TAG[g.status(n)], waiting, callers, n, g.label(n)))
    print("\n%d leaves have >=1 real caller waiting (%d shown)"
          % (len(rows), min(top, len(rows))))


def tarjan(nodes, gsucc):
    index, low, onstack, stack, idx, out = {}, {}, {}, [], [0], []
    for root in nodes:
        if root in index:
            continue
        work = [(root, iter(sorted(gsucc.get(root, ()))))]
        index[root] = low[root] = idx[0]; idx[0] += 1
        stack.append(root); onstack[root] = True
        while work:
            node, it = work[-1]
            pushed = False
            for w in it:
                if w not in index:
                    index[w] = low[w] = idx[0]; idx[0] += 1
                    stack.append(w); onstack[w] = True
                    work.append((w, iter(sorted(gsucc.get(w, ())))))
                    pushed = True
                    break
                elif onstack.get(w):
                    low[node] = min(low[node], index[w])
            if pushed:
                continue
            if low[node] == index[node]:
                comp = []
                while True:
                    m = stack.pop(); onstack[m] = False; comp.append(m)
                    if m == node:
                        break
                if len(comp) > 1:
                    out.append(comp)
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
    return out


def cmd_scc(g, top):
    comps = tarjan(g.nodes, g.gsucc)
    comps.sort(key=len, reverse=True)
    print("=== recursion clusters (SCCs > 1) -- reimplement each as one unit ===\n")
    if not comps:
        print("none: the game call graph is acyclic at function granularity.")
        return
    for comp in comps[:top]:
        comp = sorted(comp)
        print("cluster of %d:" % len(comp))
        for n in comp:
            print("  %-5s 0x%08x %s" % (TAG[g.status(n)], n, g.label(n)))
        print("")
    print("%d clusters (%d shown)" % (len(comps), min(top, len(comps))))


def cmd_dashboard(g):
    counts = {"full": 0, "partial": 0, "stub": 0, "none": 0}
    for n in g.nodes:
        counts[g.status(n)] += 1
    tot = len(g.nodes)
    print("=== reimpl plan dashboard ===")
    print("name source : %s" % (g.name_src or "(none -- names unavailable)"))
    print("game fns    : %d" % tot)
    print("  full      : %d (%.1f%%)" % (counts["full"], 100.0 * counts["full"] / max(1, tot)))
    print("  partial   : %d" % counts["partial"])
    print("  stub      : %d" % counts["stub"])
    print("  not-started: %d" % counts["none"])

    frontier = [n for n in g.nodes if g.status(n) != "full"
                and all(g.status(c) == "full" for c in g.gsucc.get(n, ()))]
    comps = tarjan(g.nodes, g.gsucc)
    print("\nfrontier (callees all full) : %d   -> --frontier" % len(frontier))
    print("recursion clusters (>1)     : %d   -> --scc" % len(comps))

    lev = []
    for n in g.nodes:
        if g.status(n) == "full":
            continue
        waiting = sum(1 for c in g.gpred.get(n, ()) if g.status(c) in ("full", "partial"))
        if waiting:
            lev.append((waiting, n))
    lev.sort(reverse=True)
    print("\ntop high-leverage leaves (real callers waiting) -> --leverage:")
    for waiting, n in lev[:10]:
        print("  %-5s %3d  0x%08x %s" % (TAG[g.status(n)], waiting, n, g.label(n)))
    if not lev:
        print("  (none)")
    print("\nclosure worklist for any target: --blocks <name|addr>")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--blocks", metavar="FN", help="closure worklist for a target fn/addr")
    ap.add_argument("--frontier", action="store_true", help="fns whose callees are all full")
    ap.add_argument("--leverage", action="store_true", help="leaves unblocking the most callers")
    ap.add_argument("--scc", action="store_true", help="recursion clusters")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    g = Graph()
    if not g.names:
        print("note: no name map (live_functions.txt / master_functions.h) -- "
              "functions show as FUN_<addr>.\n")
    if args.blocks:
        cmd_blocks(g, args.blocks, args.top)
    elif args.frontier:
        cmd_frontier(g, args.top)
    elif args.leverage:
        cmd_leverage(g, args.top)
    elif args.scc:
        cmd_scc(g, args.top)
    else:
        cmd_dashboard(g)


if __name__ == "__main__":
    main()
