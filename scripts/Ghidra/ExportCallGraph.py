# Run this inside Ghidra Script Manager with the swep1r binary loaded
# (or headless: analyzeHeadless <proj_dir> <proj> -process <prog>
#               -postScript ExportCallGraph.py -noanalysis).
#
# Dumps the whole-image call graph as one edge line per function:
#
#     <caller_addr>\t<callee_addr>,<callee_addr>,...
#
# Addresses are lowercase 8-hex (no 0x), matching live_functions.txt / naming_gap.py.
# Self-calls (direct recursion) are kept; external/import callees are dropped (they are
# never game functions and have non-numeric entry points). A function with no in-image
# callees still gets a line with an empty callee list, so leaves are visible.
#
# Output: scripts/Ghidra/call_edges.txt  (a static dump, .gitignore'd like the others).
# Consume it with scripts/Ghidra/reimpl_plan.py.
#
# @category SWR
# @runtime Jython

import os

monitor = ghidra.util.task.TaskMonitor.DUMMY

script_path = os.path.dirname(os.path.realpath(__file__))
out_fname = os.path.join(script_path, "call_edges.txt")


def addr_str(func):
    # in-image functions have a plain 8-hex entry point; lowercase for consistency.
    return func.getEntryPoint().toString().lower()


functions = list(currentProgram.functionManager.getFunctions(True))
out = open(out_fname, "w")
out.write("# call graph: <caller_hex>\\t<callee_hex>,...  (static dump from Ghidra)\n")
out.write("# regenerate with ExportCallGraph.py; consume with reimpl_plan.py\n")

edges = 0
for i, function in enumerate(functions):
    if i % 200 == 0:
        print("{}/{} ({:.0f}%)".format(i, len(functions),
                                       100.0 * i / max(1, len(functions))))
    if function.isExternal():
        continue
    try:
        callees = function.getCalledFunctions(monitor)
    except TypeError:
        callees = function.getCalledFunctions()
    cs = []
    for c in callees:
        if c.isExternal():
            continue
        cs.append(addr_str(c))
    cs = sorted(set(cs))
    edges += len(cs)
    out.write("%s\t%s\n" % (addr_str(function), ",".join(cs)))

out.close()
print("wrote %d functions, %d edges -> %s" % (len(functions), edges, out_fname))
