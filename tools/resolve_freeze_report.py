#!/usr/bin/env python3
"""Resolve a Mine64 freeze post-mortem against a symbol listing.

Usage: resolve_freeze_report.py <freeze.txt> <nm-listing>

The listing is `mips-n64-nm -n` output for the exact binary that froze
(build/mine64-deployed.out, archived by live-load/perma-load).  PC and RA
rows become symbol+offset, CAUSE becomes a named CPU exception, and the
position rows decode from raw float bits to blocks.
"""

import struct
import sys

CAUSES = {
    0: "interrupt",
    2: "TLB miss (load)",
    3: "TLB miss (store)",
    4: "address error (load)",
    5: "address error (store)",
    8: "syscall",
    9: "breakpoint",
    10: "reserved instruction",
    11: "coprocessor unusable",
    12: "arithmetic overflow",
    13: "trap",
    15: "floating point exception (the classic VR4300 case is an "
        "unimplemented-op on a denormal, NaN, or out-of-range conversion)",
}


def load_symbols(path):
    syms = []
    for line in open(path):
        parts = line.split()
        if len(parts) != 3 or parts[1] not in "TtDdBb":
            continue
        address = int(parts[0], 16) & 0xFFFFFFFF
        if address >= 0x80000000:
            syms.append((address, parts[2]))
    syms.sort()
    return syms


def resolve(syms, value):
    best = None
    for address, name in syms:
        if address <= value:
            best = (address, name)
        else:
            break
    if best is None or value - best[0] > 0x100000:
        return "(not in the image)"
    return "%s+0x%x" % (best[1], value - best[0])


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    syms = load_symbols(sys.argv[2])
    for line in open(sys.argv[1]):
        parts = line.split()
        if len(parts) != 2:
            continue
        label, raw = parts[0], int(parts[1], 16)
        note = ""
        if label in ("PC", "RA"):
            note = resolve(syms, raw)
        elif label == "CAUSE":
            code = (raw >> 2) & 0x1F
            note = "exception %d: %s" % (code, CAUSES.get(code, "?"))
        elif label in ("POSX", "POSY", "POSZ"):
            value = struct.unpack(">f", struct.pack(">I", raw))[0]
            note = "%.2f units = %.2f blocks" % (value, value / 64.0)
        elif label == "KVAL" and raw != 0:
            cx = ((raw >> 15) & 0x7FFF ^ 0x4000) - 0x4000
            cz = (raw & 0x7FFF ^ 0x4000) - 0x4000
            note = "resident=%d cx=%d cz=%d" % (raw >> 31, cx, cz)
        print("%-7s %08X  %s" % (label, raw, note))


if __name__ == "__main__":
    main()
