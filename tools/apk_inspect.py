#!/usr/bin/env python3
"""Inspect the native libraries inside an Android package and report which
external symbols they need.

Accepts .apk, .apkm, .xapk or any zip that contains lib/<abi>/*.so, including
bundles that nest split APKs. Extracted libraries go into --out, which must be
a new or empty directory. Nothing from the package is executed.

Usage:
    python3 -I tools/apk_inspect.py PACKAGE --out OUTDIR [--abi x86_64] [--json report.json]
"""

import argparse
import io
import json
import os
import re
import subprocess
import sys
import zipfile
from collections import defaultdict

SAFE_SO = re.compile(r"^lib/(?P<abi>[A-Za-z0-9_\-]+)/(?P<name>[A-Za-z0-9_.+\-]+\.so)$")

# Heuristic grouping of external symbols by the Android/bionic library that
# normally provides them. ELF does not record which library a symbol comes from,
# so this is a prefix guide, not ground truth.
CATEGORIES = [
    ("liblog", re.compile(r"^__android_log_")),
    ("libandroid", re.compile(r"^(ANativeWindow_|AAsset|AConfiguration_|AInput|AKey|AMotion|ALooper_|ASensor|AChoreographer_|ANativeActivity_|AHardwareBuffer_|ASurface|AFont|ABitmap|AndroidBitmap_|ATrace_)")),
    ("libEGL", re.compile(r"^egl[A-Z]")),
    ("libGLESv2/v3", re.compile(r"^gl[A-Z]")),
    ("libvulkan", re.compile(r"^vk[A-Z]")),
    ("libOpenSLES", re.compile(r"^(sl[A-Z]|SL_IID_)")),
    ("libaaudio", re.compile(r"^AAudio")),
    ("libmediandk", re.compile(r"^(AMedia|AImage)")),
    ("libjnigraphics", re.compile(r"^AndroidBitmap_")),
    ("bionic (properties/misc)", re.compile(r"^(__system_property_|__libc_|android_|__cxa_thread_atexit_impl$|__errno$|__sF$|__stack_chk_)")),
    ("libdl", re.compile(r"^(dlopen|dlsym|dlclose|dlerror|dladdr|dl_iterate_phdr|android_dlopen_ext)$")),
    ("libz", re.compile(r"^(inflate|deflate|crc32|adler32|compress|uncompress|gz)")),
    ("libc++ (C++ runtime)", re.compile(r"^(_ZNSt|_ZSt|_ZNKSt|_ZTVN10__cxxabiv|_ZTISt|_Znwm|_Znam|_ZdlPv|_ZdaPv|__cxa_|__gxx_|_Unwind_|_ZNSt3__1)")),
    ("pthread/libc/libm", re.compile(r"^(pthread_|sem_|mmap|munmap|mprotect|malloc|free|calloc|realloc|memcpy|memset|memmove|str|sprintf|snprintf|fopen|fread|fwrite|fclose|open|read|write|close|socket|connect|send|recv|poll|epoll_|getaddrinfo|sin|cos|tan|pow|sqrt|exp|log|floor|ceil|fmod|atan|asin|acos)")),
]


def categorize(sym):
    for name, rx in CATEGORIES:
        if rx.match(sym):
            return name
    return "other"


def run_readelf(args, path):
    res = subprocess.run(
        ["readelf", *args, path],
        capture_output=True, text=True, errors="replace", check=False,
    )
    return res.stdout


def parse_dynamic(path):
    needed, soname = [], None
    for line in run_readelf(["-d", "-W"], path).splitlines():
        m = re.search(r"\((NEEDED)\)\s+Shared library: \[(.+?)\]", line)
        if m:
            needed.append(m.group(2))
            continue
        m = re.search(r"\((SONAME)\)\s+Library soname: \[(.+?)\]", line)
        if m:
            soname = m.group(2)
    return needed, soname


def parse_symbols(path):
    undefined, defined = set(), set()
    for line in run_readelf(["--dyn-syms", "-W"], path).splitlines():
        parts = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        bind, ndx, name = parts[4], parts[6], parts[7]
        name = name.split("@", 1)[0]
        if not name:
            continue
        if ndx == "UND":
            if bind in ("GLOBAL", "WEAK"):
                undefined.add((name, bind == "WEAK"))
        elif bind in ("GLOBAL", "WEAK"):
            defined.add(name)
    return undefined, defined


def iter_libs(zf, abi, label=""):
    """Yield (display_name, basename, bytes) for each lib/<abi>/*.so, recursing
    into nested zip members (split APKs inside .apkm/.xapk bundles)."""
    for info in zf.infolist():
        if info.is_dir():
            continue
        name = info.filename
        m = SAFE_SO.match(name)
        if m and m.group("abi") == abi:
            yield f"{label}{name}", m.group("name"), zf.read(info)
        elif name.lower().endswith((".apk", ".zip")) and info.file_size < 2_000_000_000:
            try:
                with zipfile.ZipFile(io.BytesIO(zf.read(info))) as inner:
                    yield from iter_libs(inner, abi, label=f"{label}{name}!")
            except zipfile.BadZipFile:
                continue


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package")
    ap.add_argument("--out", required=True, help="new or empty directory for extracted libs")
    ap.add_argument("--abi", default="x86_64")
    ap.add_argument("--json", help="also write a machine-readable report here")
    args = ap.parse_args()

    if os.path.exists(args.out) and os.listdir(args.out):
        sys.exit(f"error: --out {args.out} exists and is not empty")
    os.makedirs(args.out, exist_ok=True)

    try:
        zf = zipfile.ZipFile(args.package)
    except (zipfile.BadZipFile, FileNotFoundError) as e:
        sys.exit(f"error: cannot open {args.package}: {e}")

    libs = {}
    with zf:
        for display, base, data in iter_libs(zf, args.abi):
            if base in libs:
                continue
            dest = os.path.join(args.out, base)
            with open(dest, "wb") as f:
                f.write(data)
            libs[base] = {"source": display, "path": dest, "size": len(data)}

    if not libs:
        sys.exit(f"no lib/{args.abi}/*.so found in {args.package}")

    all_defined = set()
    for base, info in libs.items():
        needed, soname = parse_dynamic(info["path"])
        und, dfn = parse_symbols(info["path"])
        info.update(needed=needed, soname=soname, undefined=und)
        all_defined |= dfn

    bundled = set(libs) | {i["soname"] for i in libs.values() if i["soname"]}
    system_needed = defaultdict(list)
    for base, info in libs.items():
        for n in info["needed"]:
            if n not in bundled:
                system_needed[n].append(base)

    external = defaultdict(lambda: {"weak": True, "used_by": set()})
    for base, info in libs.items():
        for sym, weak in info["undefined"]:
            if sym in all_defined:
                continue
            e = external[sym]
            e["weak"] = e["weak"] and weak
            e["used_by"].add(base)

    grouped = defaultdict(list)
    for sym in sorted(external):
        grouped[categorize(sym)].append(sym)

    print(f"package : {args.package}")
    print(f"abi     : {args.abi}")
    print(f"libs    : {len(libs)} bundled native libraries\n")
    for base, info in sorted(libs.items(), key=lambda kv: -kv[1]["size"]):
        print(f"  {base:<40} {info['size']/1e6:8.2f} MB  undef={len(info['undefined'])}")

    print("\nsystem libraries required (not bundled):")
    for n in sorted(system_needed):
        print(f"  {n:<24} <- {', '.join(sorted(system_needed[n]))}")

    print(f"\nexternal symbols the runtime must provide: {len(external)}")
    for cat in sorted(grouped, key=lambda c: -len(grouped[c])):
        print(f"  {cat:<28} {len(grouped[cat]):>5}")

    if args.json:
        report = {
            "package": args.package,
            "abi": args.abi,
            "libs": {b: {"source": i["source"], "size": i["size"], "needed": i["needed"], "soname": i["soname"]} for b, i in libs.items()},
            "system_needed": {k: sorted(v) for k, v in system_needed.items()},
            "external_symbols": {
                cat: [{"name": s, "weak": external[s]["weak"], "used_by": sorted(external[s]["used_by"])} for s in syms]
                for cat, syms in grouped.items()
            },
        }
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
