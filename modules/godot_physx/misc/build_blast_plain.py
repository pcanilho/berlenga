#!/usr/bin/env python3
"""Builds the four Blast libraries the module links (NvBlast, NvBlastGlobals,
NvBlastExtShaders, NvBlastExtAuthoring) with the system C++ compiler, without
NVIDIA's packman/repo_build tooling.

Used where that tooling can't run: on Linux arm64 it pulls host packages NVIDIA
doesn't publish for arm64. Mirrors the projects in blast/premake5.lua (same
sources, include dirs and release defines) and lays the result out like the
official build: <out>/include (Blast's headers) and <out>/bin (the .so files).
Fracture authoring needs Boost.Multiprecision headers (libboost-dev).

  build_blast_plain.py <blast dir> <out dir> [--jobs N]
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# name -> (source dirs under source/sdk, extra include dirs, libraries it links)
LIBS = [
    ("NvBlast", ["lowlevel"], ["include/shared/NvFoundation"], []),
    (
        "NvBlastGlobals",
        ["globals"],
        ["include/lowlevel", "source/shared/NsFoundation/include", "include/shared/NvFoundation"],
        [],
    ),
    (
        "NvBlastExtShaders",
        ["extensions/shaders"],
        ["include/lowlevel", "include/globals", "include/shared/NvFoundation", "source/shared/NsFoundation/include"],
        ["NvBlast", "NvBlastGlobals"],
    ),
    (
        "NvBlastExtAuthoring",
        ["extensions/authoring", "extensions/authoringCommon", "extensions/authoring/VHACD/src"],
        [
            "include/lowlevel",
            "include/globals",
            "include/extensions/assetutils",
            "include/extensions/authoringCommon",
            "source/sdk/extensions/authoring",
            "source/sdk/extensions/authoringCommon",
            "source/sdk/extensions/authoring/VHACD/inc",
            "source/sdk/extensions/authoring/VHACD/public",
            "include/shared/NvFoundation",
            "source/shared/NsFoundation/include",
        ],
        ["NvBlast", "NvBlastGlobals"],
    ),
]

CXXFLAGS = [
    "-std=c++14",
    "-O2",
    "-fPIC",
    "-pthread",
    "-fvisibility=hidden",
    "-fnon-call-exceptions",
    "-fno-strict-aliasing",  # Blast casts between its vector types (see patch 0005)
    "-D_FILE_OFFSET_BITS=64",
    "-DNDEBUG",
    "-DCARB_DEBUG=0",
    "-w",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blast_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = ap.parse_args()

    blast = os.path.abspath(args.blast_dir)
    out = os.path.abspath(args.out_dir)
    obj_root = os.path.join(out, "_obj")
    bin_dir = os.path.join(out, "bin")
    os.makedirs(bin_dir, exist_ok=True)

    for name, src_dirs, incs, links in LIBS:
        main_dir = src_dirs[0]
        include_dirs = ["include", "source/sdk/common", "include/" + main_dir, "source/sdk/" + main_dir] + incs
        flags = CXXFLAGS + ['-DLOG_COMPONENT="%s"' % name] + ["-I" + os.path.join(blast, d) for d in include_dirs]
        sources = sorted(glob.glob(os.path.join(blast, "source/sdk/common/*.cpp")))
        for d in src_dirs:
            sources += sorted(glob.glob(os.path.join(blast, "source/sdk", d, "*.cpp")))
        obj_dir = os.path.join(obj_root, name)
        os.makedirs(obj_dir, exist_ok=True)

        def compile_one(src):
            obj = os.path.join(obj_dir, os.path.relpath(src, blast).replace("/", "_") + ".o")
            cmd = [args.cxx] + flags + ["-c", src, "-o", obj]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                return obj, "%s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr)
            return obj, None

        print("Blast: compiling %s (%d files)" % (name, len(sources)), flush=True)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(compile_one, sources))
        errors = [e for _, e in results if e]
        if errors:
            print(errors[0])
            sys.exit("Blast: %s failed to compile (%d files)" % (name, len(errors)))

        so = os.path.join(bin_dir, "lib%s.so" % name)
        cmd = (
            [args.cxx, "-shared", "-o", so, "-Wl,-soname,lib%s.so" % name, "-Wl,--no-undefined"]
            + [o for o, _ in results]
            + ["-L" + bin_dir]
            + ["-l" + l for l in links]
            + ["-Wl,-rpath,$ORIGIN", "-pthread", "-lrt"]
        )
        print("Blast: linking lib%s.so" % name, flush=True)
        subprocess.check_call(cmd)

    inc_out = os.path.join(out, "include")
    if os.path.isdir(inc_out):
        shutil.rmtree(inc_out)
    shutil.copytree(os.path.join(blast, "include"), inc_out)
    shutil.rmtree(obj_root, ignore_errors=True)
    print("Blast SDK ready: " + out)


if __name__ == "__main__":
    main()
