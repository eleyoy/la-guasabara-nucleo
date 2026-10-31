"""Builds guasa-core (level 1) and its test program.

    python -I build.py

Reuses the spike's build (spike/harness/build.py): the same pinned fork,
patches, generated files, compiler and flags. game/build.py uses
core_objects() to link the same core into the game.
"""
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("spikebuild", os.path.join(REPO, "spike", "harness", "build.py"))
sb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sb)

OUT = os.path.join(REPO, "spike", "build")
ZLIB = os.path.join(sb.FB, "dep", "libs", "zlib")
ZLIB_SOURCES = ["adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "zutil.c"]


def compile_cpp(src, extra=()):
    obj = os.path.join(sb.OBJ, "core_" + os.path.basename(src) + ".o")
    cmd = [sb.CXX] + sb.flags() + ["-I" + HERE, "-I" + ZLIB, "-std=gnu++17"] + list(extra) + ["-c", src, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("FAILED", os.path.basename(src))
        print(r.stderr[:3000])
        return None
    return obj


def core_objects():
    """The fork's CPS2 core, zlib and guasa_core.cpp, compiled. None if anything failed."""
    sb.fetch_fork()
    sb.generate()
    os.makedirs(sb.OBJ, exist_ok=True)
    jobs = [(s, os.path.join(sb.FB, s), s.endswith(".cpp")) for s in sb.SOURCES]
    jobs.append(("m68kops.c", os.path.join(sb.GEN, "m68kops.c"), False))
    # zlib (inflate only), to read the ROMs straight from their zips
    jobs += [(n, os.path.join(ZLIB, n), False) for n in ZLIB_SOURCES]
    objs, failed = [], False
    for src, path, cxx in jobs:
        obj, err = sb.compile_one(src, path, cxx)
        if obj is None:
            print("FAILED", src, err[:2000])
            failed = True
        objs.append(obj)
    core = compile_cpp(os.path.join(HERE, "guasa_core.cpp"))
    if failed or core is None:
        return None
    return objs + [core]


def main():
    objs = core_objects()
    test = compile_cpp(os.path.join(HERE, "tests", "core_test.cpp"))
    if objs is None or test is None:
        return 1
    exe = os.path.join(OUT, "core_test.exe" if sb.WINDOWS else "core_test")
    r = subprocess.run([sb.CXX, "-static" if sb.WINDOWS else "-pthread", "-o", exe] + objs + [test],
                       capture_output=True, text=True)
    if r.returncode:
        print("LINK FAILED")
        print(r.stderr[:4000])
        return 1
    print("built", exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
