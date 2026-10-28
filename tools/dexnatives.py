#!/usr/bin/env python3
"""Read the app's Java side (classes*.dex) and answer three questions the
native side cannot: what is the exact signature of each `native` method, which
Java methods call it, and in what order a given Java method makes its calls.
Pure Python, no dependencies; nothing from the package is executed.

Accepts .apk, .apkm, .xapk (split APKs nested inside are searched) or a .dex.

Usage:
    python3 -I tools/dexnatives.py PACKAGE [--class SUBSTRING] [--callers]
        List native methods with Java signatures and JNI descriptors.
        --callers adds the Java methods that invoke each one.

    python3 -I tools/dexnatives.py PACKAGE --script [--class SUBSTRING]
        Print the natives as tonic --script lines (args left empty).

    python3 -I tools/dexnatives.py PACKAGE --trace CLASS.METHOD [--depth N]
        Print the calls CLASS.METHOD makes, in bytecode order, marking natives
        with '*'. --depth N follows calls into the app's own methods N levels
        deep (default 0). CLASS may be a suffix: MainGameActivity.onCreate.
        --only-natives hides every branch that does not reach a native, so a
        deep trace (--depth 8) shows just the paths to native calls.

    python3 -I tools/dexnatives.py PACKAGE --dump CLASS.METHOD
        Print one method's calls and the string constants it loads, in
        bytecode order: shows what values the app passes to natives.

    python3 -I tools/dexnatives.py PACKAGE --who-calls CLASS.METHOD [--depth N]
        List the methods that call CLASS.METHOD (any method, not just natives);
        --depth N repeats that N levels up the call chain.

    python3 -I tools/dexnatives.py PACKAGE --impl CLASS
        List the classes that implement interface CLASS or extend class CLASS
        (directly or further down), e.g. --impl ak.n0.

    python3 -I tools/dexnatives.py PACKAGE --string TEXT
        List the methods whose code uses a string constant containing TEXT
        (e.g. a message name), to find who handles or sends it.
"""

import argparse
import io
import struct
import sys
import zipfile
from collections import defaultdict

ACC_STATIC = 0x8
ACC_NATIVE = 0x100

# Size in 16-bit code units of every Dalvik opcode (payloads handled apart).
SIZE = [1] * 256
for lo, hi, n in [
    (0x02, 0x02, 2), (0x03, 0x03, 3), (0x05, 0x05, 2), (0x06, 0x06, 3), (0x08, 0x08, 2), (0x09, 0x09, 3),
    (0x13, 0x13, 2), (0x14, 0x14, 3), (0x15, 0x16, 2), (0x17, 0x17, 3), (0x18, 0x18, 5), (0x19, 0x1a, 2),
    (0x1b, 0x1b, 3), (0x1c, 0x1c, 2), (0x1f, 0x20, 2), (0x22, 0x23, 2), (0x24, 0x26, 3), (0x29, 0x29, 2),
    (0x2a, 0x2c, 3), (0x2d, 0x3d, 2), (0x44, 0x6d, 2), (0x6e, 0x72, 3), (0x74, 0x78, 3), (0x90, 0xaf, 2),
    (0xd0, 0xe2, 2), (0xfa, 0xfb, 4), (0xfc, 0xfd, 3), (0xfe, 0xff, 2),
]:
    for op in range(lo, hi + 1):
        SIZE[op] = n
INVOKES = set(range(0x6e, 0x73)) | set(range(0x74, 0x79)) | {0xfa, 0xfb}

PRIMS = {"V": "void", "Z": "boolean", "B": "byte", "S": "short", "C": "char", "I": "int", "J": "long",
         "F": "float", "D": "double"}


def uleb(buf, off):
    result = shift = 0
    while True:
        b = buf[off]
        off += 1
        result |= (b & 0x7F) << shift
        if b < 0x80:
            return result, off
        shift += 7


def java_type(desc):
    dims = len(desc) - len(desc.lstrip("["))
    base = desc[dims:]
    name = PRIMS.get(base) or base[1:-1].replace("/", ".").split(".")[-1]
    return name + "[]" * dims


class Dex:
    def __init__(self, data, label):
        if data[:4] != b"dex\n":
            raise ValueError(f"{label}: not a dex file")
        self.d, self.label = data, label
        u = lambda o: struct.unpack_from("<I", data, o)[0]
        n_str, o_str, n_type, o_type, n_proto, o_proto = (u(0x38), u(0x3C), u(0x40), u(0x44), u(0x48), u(0x4C))
        n_meth, o_meth, n_cls, o_cls = u(0x58), u(0x5C), u(0x60), u(0x64)
        self.strings = []
        for i in range(n_str):
            _, p = uleb(data, u(o_str + 4 * i))
            end = data.index(b"\0", p)
            self.strings.append(data[p:end].decode("utf-8", "replace"))  # MUTF-8, close enough for names
        self.types = [self.strings[u(o_type + 4 * i)] for i in range(n_type)]
        self.protos = []
        for i in range(n_proto):
            _, ret, params_off = struct.unpack_from("<III", data, o_proto + 12 * i)
            params = []
            if params_off:
                for k in range(u(params_off)):
                    params.append(self.types[struct.unpack_from("<H", data, params_off + 4 + 2 * k)[0]])
            self.protos.append(("(" + "".join(params) + ")" + self.types[ret], params, self.types[ret]))
        self.methods = []  # (class descriptor, name, proto index)
        for i in range(n_meth):
            c, p, n = struct.unpack_from("<HHI", data, o_meth + 8 * i)
            self.methods.append((self.types[c], self.strings[n], p))
        self.defined = []  # (method index, access flags, code offset)
        self.supers = {}  # class -> (superclass, [interfaces])
        for i in range(n_cls):
            cidx, _, sidx, ioff = struct.unpack_from("<IIII", data, o_cls + 32 * i)
            ifaces = []
            if ioff:
                for k in range(u(ioff)):
                    ifaces.append(self.types[struct.unpack_from("<H", data, ioff + 4 + 2 * k)[0]][1:-1])
            sup = self.types[sidx][1:-1] if sidx != 0xFFFFFFFF else None
            self.supers[self.types[cidx][1:-1]] = (sup, ifaces)
            data_off = u(o_cls + 32 * i + 24)
            if not data_off:
                continue
            p = data_off
            counts = []
            for _ in range(4):
                v, p = uleb(data, p)
                counts.append(v)
            for _ in range(counts[0] + counts[1]):  # fields: skip
                _, p = uleb(data, p)
                _, p = uleb(data, p)
            for group in (counts[2], counts[3]):
                idx = 0
                for _ in range(group):
                    diff, p = uleb(data, p)
                    flags, p = uleb(data, p)
                    code, p = uleb(data, p)
                    idx += diff
                    self.defined.append((idx, flags, code))

    def key(self, midx):
        cls, name, proto = self.methods[midx]
        return cls[1:-1], name, self.protos[proto][0]

    def invokes(self, code_off):
        """Method indexes invoked by a code_item, in bytecode order."""
        return [v for kind, v in self.events(code_off) if kind == "call"]

    def events(self, code_off):
        """("call", method index) and ("string", text) in bytecode order."""
        d = self.d
        n_units = struct.unpack_from("<I", d, code_off + 12)[0]
        base, pc, out = code_off + 16, 0, []
        while pc < n_units:
            w = struct.unpack_from("<H", d, base + 2 * pc)[0]
            op = w & 0xFF
            if op == 0 and w in (0x100, 0x200, 0x300):  # switch / array payloads
                if w == 0x100:
                    pc += 4 + 2 * struct.unpack_from("<H", d, base + 2 * pc + 2)[0]
                elif w == 0x200:
                    pc += 2 + 4 * struct.unpack_from("<H", d, base + 2 * pc + 2)[0]
                else:
                    width, count = struct.unpack_from("<HI", d, base + 2 * pc + 2)
                    pc += 4 + (width * count + 1) // 2
                continue
            if op in INVOKES:
                out.append(("call", struct.unpack_from("<H", d, base + 2 * pc + 2)[0]))
            elif op == 0x1A:
                out.append(("string", self.strings[struct.unpack_from("<H", d, base + 2 * pc + 2)[0]]))
            elif op == 0x1B:
                out.append(("string", self.strings[struct.unpack_from("<I", d, base + 2 * pc + 2)[0]]))
            pc += SIZE[op]
        return out


def load_dexes(path):
    if path.endswith(".dex"):
        with open(path, "rb") as f:
            return [Dex(f.read(), path)]
    out = []

    def walk(zf, label):
        for info in zf.infolist():
            n = info.filename
            if n.endswith(".dex") and "/" not in n:
                out.append(Dex(zf.read(info), f"{label}!{n}"))
            elif n.lower().endswith(".apk") and info.file_size < 2_000_000_000:
                try:
                    with zipfile.ZipFile(io.BytesIO(zf.read(info))) as inner:
                        walk(inner, f"{label}!{n}")
                except zipfile.BadZipFile:
                    pass

    with zipfile.ZipFile(path) as zf:
        walk(zf, path)
    if not out:
        sys.exit("error: no classes*.dex found")
    return out


def pretty(key, flags=None):
    cls, name, desc = key
    params = desc[1:desc.index(")")]
    ptypes, i = [], 0
    while i < len(params):
        j = i
        while params[j] == "[":
            j += 1
        j = params.index(";", j) + 1 if params[j] == "L" else j + 1
        ptypes.append(java_type(params[i:j]))
        i = j
    ret = java_type(desc[desc.index(")") + 1:])
    mods = "" if flags is None else ("static " if flags & ACC_STATIC else "") + ("native " if flags & ACC_NATIVE else "")
    return f"{mods}{ret} {name}({', '.join(ptypes)})"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("package")
    ap.add_argument("--class", dest="cls", default="", help="only classes containing this text")
    ap.add_argument("--callers", action="store_true", help="show the Java methods that call each native")
    ap.add_argument("--script", action="store_true", help="print tonic --script lines")
    ap.add_argument("--trace", metavar="CLASS.METHOD", help="print the calls this method makes, in order")
    ap.add_argument("--depth", type=int, default=0, help="with --trace: follow app methods this many levels")
    ap.add_argument("--dump", metavar="CLASS.METHOD", help="print calls and string constants of one method")
    ap.add_argument("--who-calls", metavar="CLASS.METHOD", help="list callers of any method")
    ap.add_argument("--impl", metavar="CLASS", help="list classes implementing/extending CLASS")
    ap.add_argument("--string", metavar="TEXT", help="list methods using a string constant containing TEXT")
    ap.add_argument("--only-natives", action="store_true", help="with --trace: show only paths that reach a native")
    a = ap.parse_args()

    dexes = load_dexes(a.package)
    natives = {}  # key -> flags
    code = {}  # key -> (dex, code_off)
    for dx in dexes:
        for midx, flags, code_off in dx.defined:
            k = dx.key(midx)
            if flags & ACC_NATIVE:
                natives[k] = flags
            if code_off:
                code[k] = (dx, code_off)

    def find_roots(spec):
        cls_part, _, meth = spec.rpartition(".")
        want = cls_part.replace(".", "/")
        roots = sorted(k for k in code if k[1] == meth and (k[0] == want or k[0].endswith("/" + want)
                                                            or k[0].endswith("$" + want)))
        if not roots:
            sys.exit(f"error: no method with code matches {spec}")
        return roots

    if a.who_calls:
        cls_part, _, meth = a.who_calls.rpartition(".")
        want = cls_part.replace(".", "/")

        def matches(k):
            return k[1] == meth and (k[0] == want or k[0].endswith("/" + want) or k[0].endswith("$" + want))

        rev = defaultdict(set)  # callee key -> caller keys
        for k, (dx, off) in code.items():
            for midx in dx.invokes(off):
                rev[dx.key(midx)].add(k)
        targets = sorted(t for t in rev if matches(t))
        if not targets:
            sys.exit(f"error: nothing calls a method matching {a.who_calls}")
        shown = set()

        def up(t, depth, indent):
            for c in sorted(rev.get(t, ())):
                print(f"{indent}<- {c[0]}.{c[1]}{c[2]}")
                if depth > 0 and c not in shown:
                    shown.add(c)
                    up(c, depth - 1, indent + "    ")

        for t in targets:
            print(f"{t[0]}.{t[1]}{t[2]}")
            up(t, a.depth, "    ")
            print()
        return

    if a.impl:
        target = a.impl.replace(".", "/")
        supers = {}
        for dx in dexes:
            supers.update(dx.supers)

        def is_sub(c, seen=()):
            if c in seen or c not in supers:
                return False
            sup, ifaces = supers[c]
            parents = ([sup] if sup else []) + ifaces
            return target in parents or any(is_sub(p, seen + (c,)) for p in parents)

        found = sorted(c for c in supers if is_sub(c))
        for c in found:
            sup, ifaces = supers[c]
            print(f"{c}  extends {sup}" + (f"  implements {', '.join(ifaces)}" if ifaces else ""))
        print(f"\n{len(found)} class(es)")
        return

    if a.string:
        hits = 0
        for k, (dx, off) in sorted(code.items()):
            found = sorted({v for kind, v in dx.events(off) if kind == "string" and a.string in v})
            if found:
                hits += 1
                print(f"{k[0]}.{k[1]}{k[2]}")
                for v in found:
                    print(f"      {v!r}")
        print(f"\n{hits} method(s)")
        return

    if a.dump:
        for r in find_roots(a.dump):
            print(f"{r[0]}.{r[1]}{r[2]}")
            dx, off = code[r]
            for kind, v in dx.events(off):
                if kind == "string":
                    print(f"      {v!r}")
                else:
                    t = dx.key(v)
                    print(f"    {'*' if t in natives else ' '} {t[0]}.{t[1]}{t[2]}")
            print()
        return

    if a.trace:
        cls_part, _, meth = a.trace.rpartition(".")
        want = cls_part.replace(".", "/")
        roots = sorted(k for k in code if k[1] == meth and (k[0] == want or k[0].endswith("/" + want)
                                                            or k[0].endswith("$" + want)))
        if not roots:
            sys.exit(f"error: no method with code matches {a.trace}")
        seen = set()

        # Returns the lines for k's calls; with --only-natives, only the lines
        # on a path to a native. A method already expanded is not expanded again.
        def trace(k, depth, indent):
            dx, off = code[k]
            lines = []
            for midx in dx.invokes(off):
                t = dx.key(midx)
                is_native = t in natives
                sub = []
                again = t in seen and t in code and depth > 0
                if depth > 0 and t in code and t not in seen:
                    seen.add(t)
                    sub = trace(t, depth - 1, indent + "    ")
                if a.only_natives and not is_native and not sub:
                    continue
                note = "   (expanded above)" if again and not a.only_natives else ""
                lines.append(f"{indent}{'*' if is_native else ' '} {t[0]}.{t[1]}{t[2]}{note}")
                lines.extend(sub)
            return lines

        for r in roots:
            print(f"{r[0]}.{r[1]}{r[2]}")
            seen.add(r)
            for line in trace(r, a.depth, "    "):
                print(line)
            print()
        return

    callers = defaultdict(list)
    if a.callers:
        for k, (dx, off) in code.items():
            for midx in dx.invokes(off):
                t = dx.key(midx)
                if t in natives and k not in callers[t]:
                    callers[t].append(k)

    by_class = defaultdict(list)
    for k, flags in natives.items():
        if a.cls in k[0]:
            by_class[k[0]].append((k, flags))

    if a.script:
        print("# tonic --script lines generated by dexnatives.py.")
        print("# Put the calls you want in the order the app makes them and fill in the")
        print("# arguments: \"text\" for a String, numbers, true/false, null, or new for a")
        print("# fresh object of the parameter's class. Missing arguments are 0/null.")
        for cls in sorted(by_class):
            print(f"\n# {cls}")
            for k, flags in sorted(by_class[cls]):
                print(f"{'static ' if flags & ACC_STATIC else ''}{k[0]}.{k[1]}{k[2]}")
        return

    total = sum(len(v) for v in by_class.values())
    print(f"{total} native methods in {len(by_class)} classes ({len(dexes)} dex files)\n")
    for cls in sorted(by_class):
        print(cls.replace("/", "."))
        for k, flags in sorted(by_class[cls]):
            print(f"    {pretty(k, flags):70s} {k[2]}")
            for c in callers.get(k, []):
                print(f"        <- {c[0].replace('/', '.')}.{c[1]}{c[2]}")
        print()


if __name__ == "__main__":
    main()
