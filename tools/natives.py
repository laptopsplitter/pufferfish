#!/usr/bin/env python3
"""List the Java-callable native methods a library exports by JNI name
(Java_<package>_<Class>_<method>), grouped by Java class. These are the entry
points the app's Java side calls into the native code without RegisterNatives.
Read-only; needs nm (binutils).

Usage:
    python3 -I tools/natives.py extracted/libroblox.so [--class SUBSTRING]
"""

import argparse
import subprocess
from collections import defaultdict


def unescape(part):
    # JNI name mangling: _1 = '_', _2 = ';', _3 = '[', _0xxxx = unicode char.
    out, i = [], 0
    while i < len(part):
        if part[i] == "_" and i + 1 < len(part) and part[i + 1] in "0123":
            c = part[i + 1]
            if c == "1": out.append("_"); i += 2
            elif c == "2": out.append(";"); i += 2
            elif c == "3": out.append("["); i += 2
            else: out.append(chr(int(part[i + 2:i + 6], 16))); i += 6
        else:
            out.append(part[i]); i += 1
    return "".join(out)


def split_unescaped(body):
    # Split on single '_' that are not escapes (_0.._3), then unescape each part.
    parts, cur, i = [], "", 0
    while i < len(body):
        if body[i] == "_" and not (i + 1 < len(body) and body[i + 1] in "0123"):
            parts.append(cur); cur = ""; i += 1
        else:
            cur += body[i]; i += 1
    parts.append(cur)
    return [unescape(p) for p in parts]


def split_jni(sym):
    body = sym[len("Java_"):]
    overload = None
    if "__" in body:  # overloaded: Java_pkg_Cls_method__<mangled args>
        body, overload = body.split("__", 1)
    parts = split_unescaped(body)
    cls = "/".join(parts[:-1])
    # In the overload's argument signature a bare '_' is '/'.
    method = parts[-1] + (f"  (args {'/'.join(split_unescaped(overload))})" if overload else "")
    return cls, method


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("--class", dest="cls", default="", help="only classes containing this text")
    a = ap.parse_args()
    out = subprocess.run(["nm", "-D", "--defined-only", a.lib], capture_output=True, text=True).stdout
    by_class = defaultdict(list)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in "Tt" or not parts[2].startswith("Java_"):
            continue
        name = parts[2].split("@")[0]
        cls, method = split_jni(name)
        by_class[cls].append((method, int(parts[0], 16)))
    total = sum(len(v) for v in by_class.values())
    print(f"{total} JNI-named natives in {len(by_class)} classes\n")
    for cls in sorted(by_class):
        if a.cls and a.cls not in cls:
            continue
        print(f"{cls} ({len(by_class[cls])})")
        for method, addr in sorted(by_class[cls]):
            print(f"    {method:60s} image+{addr:#x}")
    print()


if __name__ == "__main__":
    main()
