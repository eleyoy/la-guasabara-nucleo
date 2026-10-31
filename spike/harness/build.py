"""Builds the spike harness: the fightcade-fbneo CPS2 core with no window.

    python -I build.py

Fetches the pinned fork and applies spike/patches if needed, generates the
68000 core, CPS tile code and driver list, then compiles.

Windows: the portable clang in spike/tools. Linux (test tool only, the player
client is Windows only): clang from the system. Flags follow the fork's Visual
Studio release build (what Fightcade ships): no USE_SPEEDHACKS, no A68K.
"""
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys

FORK_URL = "https://github.com/fightcadeorg/fightcade-fbneo.git"
FORK_COMMIT = "c95950140e2424643a0bb589dc444f0defe03155"
# GekkoNet release v20261009095145 (BSD-2-Clause): rollback netcode for spike step 4
GEKKO_URL = "https://github.com/HeatXD/GekkoNet.git"
GEKKO_COMMIT = "0d491a4524e10173fc5d2aae22d042d3ae9f2d44"

WINDOWS = os.name == "nt"
SPIKE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORK = os.path.join(SPIKE, "third_party", "fightcade-fbneo")
FB = os.path.join(FORK, "src")
GEKKO = os.path.join(SPIKE, "third_party", "GekkoNet", "GekkoLib")
GEN = os.path.join(SPIKE, "build", "generated")
OBJ = os.path.join(SPIKE, "build", "obj")
# SDL3 3.4.18 (zlib license) for the play mode window, Windows only
SDL = os.path.join(SPIKE, "tools", "SDL3-3.4.18", "x86_64-w64-mingw32")
if WINDOWS:
    BIN = os.path.join(SPIKE, "tools", "llvm-mingw-20260922-ucrt-x86_64", "bin")
    CC = os.path.join(BIN, "clang.exe")
    CXX = os.path.join(BIN, "clang++.exe")
else:
    CC, CXX = "clang", "clang++"

INCLUDE_DIRS = ["burn", "burn/devices", "burn/snd", "burn/drv", "burn/drv/capcom",
                "cpu", "cpu/m68k", "cpu/z80", "intf"]

# Same defines as the Release|Win32/x64 configuration of fbneo_vs2015.vcxproj (a Unicode build),
# minus INCLUDE_LIB_PNGH and INCLUDE_AVI_RECORDING (front-end only). On Linux the
# fork's SDL port supplies TCHAR (plain char); text only, the emulation is the same.
DEFINES = ["LSB_FIRST", "INLINE=static inline", "C_INLINE=inline", "MAME_INLINE=static inline"]
if WINDOWS:
    DEFINES += ["BUILD_WIN32", "UNICODE", "_UNICODE", "_CRT_SECURE_NO_WARNINGS"]
else:
    DEFINES += ["BUILD_SDL"]
    INCLUDE_DIRS.append("burner/sdl")

# Determinism rule 3: no fast-math, and no fused multiply-add (MSVC doesn't fuse).
COMMON = ["-O2", "-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing", "-w"]

SOURCES = [
    # core
    "burn/burn.cpp", "burn/burn_bitmap.cpp", "burn/burn_gun.cpp", "burn/burn_led.cpp",
    "burn/burn_shift.cpp", "burn/burn_memory.cpp", "burn/burn_pal.cpp", "burn/burn_sound.cpp",
    "burn/burn_sound_c.cpp", "burn/cheat.cpp", "burn/debug_track.cpp", "burn/hiscore.cpp",
    "burn/load.cpp", "burn/tilemap_generic.cpp", "burn/tiles_generic.cpp", "burn/timer.cpp",
    "burn/vector.cpp",
    "burn/devices/eeprom.cpp", "burn/devices/joyprocess.cpp",
    # CPS1 sound, linked in by cps.cpp
    "burn/snd/burn_ym2151.cpp", "burn/snd/ym2151.c", "burn/snd/msm6295.cpp",
    # CPUs
    "cpu/m68000_intf.cpp", "cpu/m68k/m68kcpu.c", "cpu/z80_intf.cpp", "cpu/z80/z80.cpp",
    "cpu/z80/z80daisy.cpp", "cpu/z80/z80ctc.cpp", "cpu/z80/z80pio.cpp",
    # CPS2
    "burn/drv/capcom/cps.cpp", "burn/drv/capcom/cps2_crpt.cpp", "burn/drv/capcom/cps_config.cpp",
    "burn/drv/capcom/cps_draw.cpp", "burn/drv/capcom/cps_mem.cpp", "burn/drv/capcom/cps_obj.cpp",
    "burn/drv/capcom/cps_pal.cpp", "burn/drv/capcom/cps_run.cpp", "burn/drv/capcom/cps_rw.cpp",
    "burn/drv/capcom/cps_scr.cpp", "burn/drv/capcom/cpsr.cpp", "burn/drv/capcom/cpsrd.cpp",
    "burn/drv/capcom/cpst.cpp", "burn/drv/capcom/ctv.cpp", "burn/drv/capcom/kabuki.cpp",
    "burn/drv/capcom/ps.cpp", "burn/drv/capcom/ps_m.cpp", "burn/drv/capcom/ps_z.cpp",
    "burn/drv/capcom/qs.cpp", "burn/drv/capcom/qs_c.cpp", "burn/drv/capcom/qs_z.cpp",
    "burn/drv/capcom/d_cps2.cpp",
]
EXTRA = sys.argv[1:]  # more sources, relative to src/, while chasing link errors


def flags():
    out = list(COMMON)
    out += ["-I" + GEN] + ["-I" + os.path.join(FB, d) for d in INCLUDE_DIRS]
    out += ["-I" + os.path.join(GEKKO, "include"), "-DGEKKONET_STATIC"]
    out += ["-D" + d for d in DEFINES]
    return out


def gekko_flags():
    inc = [os.path.join(GEKKO, d) for d in ("include", "include/private", "thirdparty", "thirdparty/asio")]
    return ["-O2", "-w", "-std=gnu++20", "-DGEKKONET_STATIC"] + ["-I" + d for d in inc]


def compile_one(src, path, cxx):
    obj = os.path.join(OBJ, os.path.basename(src) + ".o")
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(path):
        return obj, ""
    if src.startswith("gekko/"):
        cmd = [CXX] + gekko_flags()
    else:
        cmd = [CXX if cxx else CC] + flags() + (["-std=gnu++17"] if cxx else ["-std=gnu11"])
    cmd += ["-c", path, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return (obj if r.returncode == 0 else None), r.stderr


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def fetch(path, url, commit):
    if not os.path.isdir(os.path.join(path, ".git")):
        os.makedirs(path, exist_ok=True)
        run(["git", "init", "-q"], cwd=path)
        run(["git", "fetch", "-q", "--depth", "1", url, commit], cwd=path)
        run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=path)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, capture_output=True, text=True).stdout.strip()
    if head != commit:
        sys.exit(f"{path} is at {head}, expected {commit}")


def fetch_fork():
    fetch(FORK, FORK_URL, FORK_COMMIT)
    fetch(os.path.dirname(GEKKO), GEKKO_URL, GEKKO_COMMIT)
    for patch in sorted(glob.glob(os.path.join(SPIKE, "patches", "*.patch"))):
        applied = subprocess.run(["git", "apply", "--reverse", "--check", patch], cwd=FORK, capture_output=True).returncode == 0
        if not applied:
            run(["git", "apply", patch], cwd=FORK)
            print("applied", os.path.basename(patch))


def generate():
    os.makedirs(GEN, exist_ok=True)
    tools = os.path.join(SPIKE, "build", "tools")
    os.makedirs(tools, exist_ok=True)
    exe = ".exe" if WINDOWS else ""
    if not os.path.exists(os.path.join(GEN, "m68kops.c")):
        m68kmake = os.path.join(tools, "m68kmake" + exe)
        run([CC, "-w", "-O1", os.path.join(FB, "cpu", "m68k", "m68kmake.c"), "-o", m68kmake])
        run([m68kmake, GEN + os.sep, os.path.join(FB, "cpu", "m68k", "m68k_in.c")], capture_output=True)
    if not os.path.exists(os.path.join(GEN, "ctv.h")):
        ctv_make = os.path.join(tools, "ctv_make" + exe)
        run([CXX, "-w", "-O1", os.path.join(FB, "burn", "drv", "capcom", "ctv_make.cpp"), "-o", ctv_make])
        with open(os.path.join(GEN, "ctv.h"), "w") as out:
            run([ctv_make], stdout=out)
    if not os.path.exists(os.path.join(GEN, "driverlist.h")):
        # Only the CPS2 drivers: the list comes from d_cps2.cpp alone.
        run(["perl", os.path.join(FB, "dep", "scripts", "gamelist.pl"), "-o", os.path.join(GEN, "driverlist.h"),
             "-l", os.path.join(SPIKE, "build", "gamelist.txt"), os.path.join(FB, "burn", "drv", "capcom", "d_cps2.cpp")],
            capture_output=True)


def main():
    fetch_fork()
    generate()
    os.makedirs(OBJ, exist_ok=True)
    jobs = [(s, os.path.join(FB, s), s.endswith(".cpp")) for s in SOURCES + EXTRA]
    jobs.append(("m68kops.c", os.path.join(GEN, "m68kops.c"), False))
    jobs.append(("spike.cpp", os.path.join(SPIKE, "harness", "spike.cpp"), True))
    for g in sorted(glob.glob(os.path.join(GEKKO, "src", "*.cpp"))):
        jobs.append(("gekko/" + os.path.basename(g), g, True))
    objs, failed = [], False
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        for (src, _, _), (obj, err) in zip(jobs, ex.map(lambda j: compile_one(*j), jobs)):
            if obj is None:
                failed = True
                print("FAILED", src)
                print(err[:3000])
            objs.append(obj)
    if failed:
        return 1
    exe = os.path.join(SPIKE, "build", "spike.exe" if WINDOWS else "spike")
    libs = ["-lws2_32", "-lmswsock", "-lwinmm"] if WINDOWS else []
    r = subprocess.run([CXX, "-static" if WINDOWS else "-pthread", "-o", exe] + objs + libs, capture_output=True, text=True)
    if r.returncode:
        undefined = sorted({l.split("undefined symbol: ")[1] for l in r.stderr.splitlines() if "undefined symbol: " in l})
        print("LINK FAILED;", len(undefined), "undefined symbols:")
        print("\n".join(undefined[:80]) if undefined else r.stderr[:4000])
        return 1
    print("built", exe)
    if WINDOWS and os.path.isdir(SDL):
        return build_play(objs, libs)
    return 0


def build_play(objs, libs):
    """guasa_play.exe: the same program with a window, controls and sound (SDL3)."""
    src = os.path.join(SPIKE, "harness", "spike.cpp")
    obj = os.path.join(OBJ, "spike_play.o")
    cmd = [CXX] + flags() + ["-std=gnu++17", "-DSPIKE_SDL", "-I" + os.path.join(SDL, "include"), "-c", src, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("FAILED play build")
        print(r.stderr[:3000])
        return 1
    exe = os.path.join(SPIKE, "build", "guasa_play.exe")
    core = [o for o in objs if not o.endswith("spike.cpp.o")]
    r = subprocess.run([CXX, "-static-libstdc++", "-static-libgcc", "-o", exe, obj] + core + libs +
                       ["-L" + os.path.join(SDL, "lib"), "-lSDL3"], capture_output=True, text=True)
    if r.returncode:
        print("LINK FAILED (play)")
        print(r.stderr[:3000])
        return 1
    shutil.copy2(os.path.join(SDL, "bin", "SDL3.dll"), os.path.join(SPIKE, "build", "SDL3.dll"))
    print("built", exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
