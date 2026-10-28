#!/usr/bin/env python3
"""Find the name of the feature flag (FFlag) a function reads.

Roblox flags are variables registered at startup next to their name string
(roughly: lea rsi,[flag_var]; lea rdi,["FlagName"]; call register). This
disassembles a function (and the functions it calls, one level down), collects
the data addresses it reads, then scans the whole library for other code that
references the same addresses and prints any C strings loaded nearby. Those
strings are the candidate flag names. Read-only; needs objdump (binutils).

Usage:
    python3 -I tools/flagref.py extracted/libroblox.so 0x2e68d0a [--bytes 0x80] [--context 8]
"""

import argparse
import re
import struct
import subprocess
import sys
from collections import defaultdict

RIP_REF = re.compile(r"#\s*([0-9a-f]+)")
CALL = re.compile(r"\scall\s+([0-9a-f]+)\s")
LINE = re.compile(r"^\s*([0-9a-f]+):\s*(.*)$")


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


def cstr(data, segs, va, limit=120):
    for sva, fsz, off, flags in segs:
        if sva <= va < sva + fsz and not flags & 1:
            raw = data[off + va - sva: off + va - sva + limit]
            end = raw.find(b"\0")
            if end < 3:
                return None
            s = raw[:end]
            if all(32 <= c < 127 for c in s):
                return s.decode()
    return None


def is_code(segs, va):
    return any(sva <= va < sva + fsz and flags & 1 for sva, fsz, _, flags in segs)


def disasm(lib, start, length):
    out = subprocess.run(
        ["objdump", "-d", "--no-show-raw-insn", "-M", "intel", f"--start-address={start:#x}",
         f"--stop-address={start + length:#x}", lib],
        capture_output=True, text=True).stdout
    lines = []
    for ln in out.splitlines():
        m = LINE.match(ln)
        if m:
            lines.append((int(m.group(1), 16), m.group(2)))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("offset", type=lambda s: int(s, 0))
    ap.add_argument("--bytes", type=lambda s: int(s, 0), default=0x80)
    ap.add_argument("--context", type=int, default=8, help="instructions to look at around each reference")
    a = ap.parse_args()

    with open(a.lib, "rb") as f:
        data = f.read()
    segs = load_segments(data)

    # 1. What the function (and its direct callees) read.
    funcs = [a.offset]
    targets = set()
    seen = set()
    for depth in range(2):
        nxt = []
        for fn in funcs:
            if fn in seen:
                continue
            seen.add(fn)
            body = disasm(a.lib, fn, a.bytes)
            print(f"--- function {fn:#x}")
            for addr, ins in body:
                print(f"    {addr:x}: {ins}")
                for m in RIP_REF.finditer(ins):
                    t = int(m.group(1), 16)
                    if not is_code(segs, t):
                        targets.add(t)
                c = CALL.search(" " + ins + " ")
                if c and depth == 0:
                    nxt.append(int(c.group(1), 16))
                if ins.startswith("ret") or ins.startswith("jmp "):
                    break
        funcs = nxt
    if not targets:
        sys.exit("no data references found; try a larger --bytes")
    print("\ndata addresses read:", ", ".join(f"{t:#x}" for t in sorted(targets)))

    # 2. Everything else in the library that references those addresses, with
    #    the strings loaded around it.
    print("\nscanning the whole library (this takes a minute)...", flush=True)
    proc = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", "-M", "intel", a.lib],
                            stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    window = []
    pending = []  # (target, hit address, remaining lines to collect, collected)
    hits = defaultdict(list)
    for ln in proc.stdout:
        m = LINE.match(ln)
        if not m:
            continue
        addr, ins = int(m.group(1), 16), m.group(2)
        window.append((addr, ins))
        if len(window) > a.context:
            window.pop(0)
        for p in pending:
            p[2] -= 1
            p[3].append((addr, ins))
        for p in [p for p in pending if p[2] <= 0]:
            hits[p[0]].append((p[1], p[3]))
            pending.remove(p)
        for r in RIP_REF.finditer(ins):
            t = int(r.group(1), 16)
            if t in targets and not (a.offset <= addr < a.offset + a.bytes):
                pending.append([t, addr, a.context, list(window)])
    proc.wait()

    for t in sorted(targets):
        print(f"\n=== references to {t:#x}: {len(hits[t])}")
        for at, ctx in hits[t][:12]:
            found = {}
            for iaddr, ins in ctx:
                for r in RIP_REF.finditer(ins):
                    s = cstr(data, segs, int(r.group(1), 16))
                    if s:
                        found[s] = min(found.get(s, 1 << 30), abs(iaddr - at))
            names = [f"{s} ({d:+} bytes)" for s, d in sorted(found.items(), key=lambda kv: kv[1])]
            print(f"  at {at:#x}: strings nearby, closest first: {names if names else '(none)'}")


if __name__ == "__main__":
    main()
