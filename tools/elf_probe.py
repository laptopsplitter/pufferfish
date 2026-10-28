#!/usr/bin/env python3
"""Report the structural facts about an x86_64 ELF shared object that decide how
a custom loader must work: relocation formats, TLS, init arrays, segment
alignment and exported JNI entry points. Read-only; nothing is executed.

Usage:
    python3 -I elf_probe.py extracted/libroblox.so [--json probe.json]
"""

import argparse
import json
import struct
import subprocess
import sys
from collections import Counter

PT = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
      0x6474E550: "GNU_EH_FRAME", 0x6474E551: "GNU_STACK", 0x6474E552: "GNU_RELRO",
      0x6474E553: "GNU_PROPERTY"}

DT = {1: "NEEDED", 2: "PLTRELSZ", 4: "HASH", 5: "STRTAB", 6: "SYMTAB", 7: "RELA", 8: "RELASZ",
      9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI", 14: "SONAME",
      15: "RPATH", 16: "SYMBOLIC", 17: "REL", 18: "RELSZ", 19: "RELENT", 20: "PLTREL",
      21: "DEBUG", 22: "TEXTREL", 23: "JMPREL", 24: "BIND_NOW", 25: "INIT_ARRAY",
      26: "FINI_ARRAY", 27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ", 29: "RUNPATH", 30: "FLAGS",
      32: "PREINIT_ARRAY", 33: "PREINIT_ARRAYSZ", 35: "RELRSZ", 36: "RELR", 37: "RELRENT",
      0x6000000F: "ANDROID_REL", 0x60000010: "ANDROID_RELSZ", 0x60000011: "ANDROID_RELA",
      0x60000012: "ANDROID_RELASZ", 0x6FFFE000: "ANDROID_RELR", 0x6FFFE001: "ANDROID_RELRSZ",
      0x6FFFE003: "ANDROID_RELRENT", 0x6FFFFEF5: "GNU_HASH", 0x6FFFFFF0: "VERSYM",
      0x6FFFFFF9: "RELACOUNT", 0x6FFFFFFB: "FLAGS_1", 0x6FFFFFFE: "VERNEED",
      0x6FFFFFFF: "VERNEEDNUM"}

RELOC = {0: "NONE", 1: "64", 2: "PC32", 4: "PLT32", 5: "COPY", 6: "GLOB_DAT", 7: "JUMP_SLOT",
         8: "RELATIVE", 10: "32", 11: "32S", 16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64",
         36: "TLSDESC", 37: "IRELATIVE"}

DF_STATIC_TLS = 0x10


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            sys.exit("error: not a 64-bit little-endian ELF")
        (_, self.etype, self.machine, _, self.entry, phoff, _, _, _, phentsize, phnum, _, _, _) = \
            struct.unpack_from("<16sHHIQQQIHHHHHH", d, 0)
        self.phdrs = []
        for i in range(phnum):
            self.phdrs.append(struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize))

    def loads(self):
        return [p for p in self.phdrs if p[0] == 1]

    def v2o(self, vaddr):
        for _, _, off, va, _, fsz, _, _ in self.loads():
            if va <= vaddr < va + fsz:
                return off + (vaddr - va)
        return None

    def read_v(self, vaddr, size):
        o = self.v2o(vaddr)
        return None if o is None else self.data[o:o + size]

    def dynamic(self):
        for p in self.phdrs:
            if p[0] == 2:
                _, _, off, _, _, fsz, _, _ = p
                out = []
                for i in range(0, fsz, 16):
                    tag, val = struct.unpack_from("<qQ", self.data, off + i)
                    if tag == 0:
                        break
                    out.append((tag, val))
                return out
        return []

    def cstr(self, strtab_v, idx):
        o = self.v2o(strtab_v + idx)
        if o is None:
            return "?"
        e = self.data.index(b"\0", o)
        return self.data[o:e].decode("utf-8", "replace")


def sleb(buf, pos):
    result, shift = 0, 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                result -= 1 << shift
            return result, pos


def decode_aps2(buf):
    """Decode Android packed relocations (APS2). Returns a Counter of reloc types."""
    if buf[:4] != b"APS2":
        return None
    pos = 4
    total, pos = sleb(buf, pos)
    _off, pos = sleb(buf, pos)
    types, done = Counter(), 0
    while done < total:
        size, pos = sleb(buf, pos)
        flags, pos = sleb(buf, pos)
        by_info, by_delta, by_addend, has_addend = flags & 1, flags & 2, flags & 4, flags & 8
        if by_delta:
            _, pos = sleb(buf, pos)
        info = 0
        if by_info:
            info, pos = sleb(buf, pos)
        if has_addend and by_addend:
            _, pos = sleb(buf, pos)
        for _ in range(size):
            if not by_delta:
                _, pos = sleb(buf, pos)
            if not by_info:
                info, pos = sleb(buf, pos)
            if has_addend and not by_addend:
                _, pos = sleb(buf, pos)
            types[RELOC.get(info & 0xFFFFFFFF, str(info & 0xFFFFFFFF))] += 1
        done += size
    return types


def count_rela(buf):
    types = Counter()
    for i in range(0, len(buf) - 23, 24):
        _, info, _ = struct.unpack_from("<QQq", buf, i)
        types[RELOC.get(info & 0xFFFFFFFF, str(info & 0xFFFFFFFF))] += 1
    return types


def count_relr(buf):
    n = 0
    for i in range(0, len(buf) - 7, 8):
        (w,) = struct.unpack_from("<Q", buf, i)
        n += 1 if not w & 1 else bin(w >> 1).count("1")
    return n


def exported_jni(path):
    out = subprocess.run(["readelf", "--dyn-syms", "-W", path], capture_output=True,
                         text=True, errors="replace").stdout
    java, special = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].endswith(":") or p[6] == "UND":
            continue
        name = p[7].split("@", 1)[0]
        if name.startswith("Java_"):
            java.append(name)
        elif name in ("JNI_OnLoad", "JNI_OnUnload", "ANativeActivity_onCreate", "android_main"):
            special.append(name)
    return sorted(java), sorted(special)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("--json")
    a = ap.parse_args()

    e = Elf(a.lib)
    rep = {"file": a.lib, "machine": e.machine, "type": e.etype}
    print(f"file     : {a.lib}")
    print(f"machine  : {e.machine} ({'x86_64' if e.machine == 62 else 'NOT x86_64'})")

    print("\nprogram headers:")
    for t, fl, off, va, _, fsz, msz, al in e.phdrs:
        name = PT.get(t, hex(t))
        f = ("R" if fl & 4 else "-") + ("W" if fl & 2 else "-") + ("X" if fl & 1 else "-")
        print(f"  {name:<14} {f}  vaddr={va:#x} filesz={fsz:#x} memsz={msz:#x} align={al:#x}")
    tls = [p for p in e.phdrs if p[0] == 7]
    rep["tls"] = {"present": bool(tls)}
    if tls:
        rep["tls"].update(memsz=tls[0][6], filesz=tls[0][5], align=tls[0][7])
        print(f"\nTLS segment: YES, memsz={tls[0][6]:#x} align={tls[0][7]:#x}")
    else:
        print("\nTLS segment: none")

    dyn = e.dynamic()
    tags = {}
    for tag, val in dyn:
        tags.setdefault(tag, []).append(val)
    print("\ndynamic tags present:")
    print("  " + ", ".join(sorted({DT.get(t, hex(t)) for t in tags})))

    flags = tags.get(30, [0])[0]
    flags1 = tags.get(0x6FFFFFFB, [0])[0]
    print(f"\nDT_FLAGS={flags:#x} (STATIC_TLS={'yes' if flags & DF_STATIC_TLS else 'no'})  DT_FLAGS_1={flags1:#x}")
    strtab = tags.get(5, [None])[0]
    if strtab is not None:
        print("NEEDED : " + ", ".join(e.cstr(strtab, v) for v in tags.get(1, [])))

    print("\nrelocation formats:")
    rel = {}
    for name, (tag, szt) in {"RELA": (7, 8), "JMPREL(PLT)": (23, 2)}.items():
        if tag in tags and szt in tags:
            buf = e.read_v(tags[tag][0], tags[szt][0])
            if buf:
                rel[name] = dict(count_rela(buf))
                print(f"  {name:<12} {dict(count_rela(buf))}")
    if 0x60000011 in tags:
        buf = e.read_v(tags[0x60000011][0], tags[0x60000012][0])
        res = decode_aps2(buf) if buf else None
        rel["ANDROID_RELA(packed)"] = dict(res) if res is not None else "undecodable"
        print(f"  ANDROID_RELA (packed APS2) {dict(res) if res is not None else 'could not decode'}")
    for tag, szt, nm in ((36, 35, "RELR"), (0x6FFFE000, 0x6FFFE001, "ANDROID_RELR")):
        if tag in tags and szt in tags:
            buf = e.read_v(tags[tag][0], tags[szt][0])
            rel[nm] = count_relr(buf) if buf else 0
            print(f"  {nm:<12} {rel[nm]} RELATIVE relocations")
    if not rel:
        print("  none found")
    rep["relocations"] = rel

    ia = tags.get(27, [0])[0] // 8
    pa = tags.get(33, [0])[0] // 8
    print(f"\ninit_array entries: {ia}   preinit_array: {pa}   DT_INIT: {'yes' if 12 in tags else 'no'}")
    rep["init_array"] = ia

    java, special = exported_jni(a.lib)
    print(f"\nexported entry points: {special or 'none'}")
    print(f"exported Java_* native methods: {len(java)}")
    for n in java[:60]:
        print("  " + n)
    if len(java) > 60:
        print(f"  ... {len(java) - 60} more (use --json)")
    rep["entry_points"], rep["java_exports"] = special, java

    if a.json:
        with open(a.json, "w") as f:
            json.dump(rep, f, indent=2)
        print(f"\nwrote {a.json}")


if __name__ == "__main__":
    main()
