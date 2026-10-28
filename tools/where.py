#!/usr/bin/env python3
"""Show what is at one or more offsets in a library: the nearest exported
symbol, the instructions just before the offset (for a return address, the
call that was being made), and any C strings those instructions point at.
Read-only; nothing is executed. Needs objdump (binutils).

Usage:
    python3 -I tools/where.py extracted/libroblox.so 0x2e48365 0x1e3403c ...
    python3 -I tools/where.py LIB OFFSET... --before 0x100
"""

import argparse
import bisect
import re
import struct
import subprocess
import sys


def load_segments(data):
    if data[:4] != b"\x7fELF" or data[4] != 2:
        sys.exit("error: not a 64-bit ELF")
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    segs = []
    for i in range(phnum):
        p_type, p_flags, off, va, _, fsz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((va, fsz, off, p_flags))
    return segs


def read_cstr(data, segs, va, limit=160):
    for sva, fsz, off, flags in segs:
        if sva <= va < sva + fsz and not flags & 1:  # in a non-executable segment
            raw = data[off + va - sva: off + va - sva + limit]
            end = raw.find(b"\0")
            if end < 3:
                return None
            s = raw[:end]
            if all(32 <= b < 127 or b in (9, 10) for b in s):
                return s.decode()
    return None


def exported_functions(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "Tt":
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("offsets", nargs="+")
    ap.add_argument("--before", default="0x60", help="bytes of code to show before each offset")
    ap.add_argument("--after", default="0x8", help="bytes of code to show after each offset")
    a = ap.parse_args()
    before = int(a.before, 0)
    after = int(a.after, 0)

    with open(a.lib, "rb") as f:
        data = f.read()
    segs = load_segments(data)
    syms = exported_functions(a.lib)
    addrs = [s[0] for s in syms]
    wanted = {int(o, 0) for o in a.offsets}  # the hot addresses, flagged when a call lands on one

    def name_of(va):
        i = bisect.bisect_right(addrs, va) - 1
        return f"{syms[i][1]}+{va - syms[i][0]:#x}" if i >= 0 else f"{va:#x}"

    for o in a.offsets:
        off = int(o, 0)
        print(f"=== {off:#x}   nearest export: {name_of(off)}")
        dis = subprocess.run(
            ["objdump", "-d", "--no-show-raw-insn", "-M", "intel", "--demangle",
             f"--start-address={off - before:#x}", f"--stop-address={off + after:#x}", a.lib],
            capture_output=True, text=True).stdout
        for line in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
            if not m:
                continue
            here = int(m.group(1), 16)
            mark = " <== return address (so the call just above is what recurses)" if here == off else ""
            text = m.group(2)
            extra = ""
            # A direct call/jmp to an absolute address: name its target, and flag
            # it if it is one of the hot addresses (that is the recursion edge).
            c = re.match(r"(call|jmp)\s+0x([0-9a-f]+)", text)
            if c:
                tgt = int(c.group(2), 16)
                extra = f"    -> {name_of(tgt)}"
                # The hot address is a *return* address; the call target is the
                # function entry, which is "near" a hot address if one sits just
                # after the call site. Flag when target is itself a listed entry.
                if any(abs(tgt - w) < 0x40 for w in wanted):
                    extra += "   <== recursion edge"
            else:
                t = re.search(r"#\s*(?:0x)?([0-9a-f]+)", text)
                if t:
                    sdata = read_cstr(data, segs, int(t.group(1), 16))
                    if sdata is not None:
                        extra = f'    "{sdata}"'
            print(f"  {here:8x}: {text}{extra}{mark}")
        print()


if __name__ == "__main__":
    main()
