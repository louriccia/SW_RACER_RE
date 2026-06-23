# Carve functions at a list of addresses that auto-analysis missed.
#
# Ghidra does not auto-create functions for entry points that are only reachable
# via DATA pointers (e.g. the swrUI F1 element procs and F2 page procs, which are
# stored as callback pointers rather than reached by a CALL). Those show up as bare
# LAB_ labels, so GhidraMCP (and the decompiler) report "No function found" and they
# cannot be named/decompiled remotely.
#
# This script reads carve_queue.txt (sitting next to it; one "0xADDR" per line,
# "#" starts a comment) and creates a function at each address. It is idempotent:
# addresses that are already functions are skipped. Run it from the Script Manager,
# or headless:  analyzeHeadless <proj_dir> <proj> -process <prog> -postScript CarveQueue.py -noanalysis
# After it runs, the functions exist and can be named/decompiled via GhidraMCP.
#
# @category SWR
# @runtime Jython

import os
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd


def find_queue():
    here = os.path.dirname(os.path.abspath(sourceFile.getAbsolutePath()))
    p = os.path.join(here, "carve_queue.txt")
    if os.path.exists(p):
        return p
    return askFile("Select carve_queue.txt", "Open").getAbsolutePath()


path = find_queue()
created = 0
skipped = 0
failed = []

fh = open(path)
try:
    for raw in fh:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        addr = toAddr(long(line, 16))
        if getFunctionAt(addr) is not None:
            skipped += 1
            continue
        if getInstructionAt(addr) is None:
            DisassembleCommand(addr, None, True).applyTo(currentProgram)
        CreateFunctionCmd(addr).applyTo(currentProgram)
        if getFunctionAt(addr) is not None:
            created += 1
            print("carved %s" % addr)
        else:
            failed.append(str(addr))
finally:
    fh.close()

print("CarveQueue: %d created, %d already existed, %d failed" % (created, skipped, len(failed)))
if failed:
    print("  failed (bad bytes / mid-instruction?): %s" % ", ".join(failed))
