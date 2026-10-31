"""Builds the game window and the test bot.

    python -I build.py

- guasa-game.exe (Windows): the window, with SDL3 3.4.18 (zlib license) from spike/tools.
- guasa-bot (Windows and Linux): the other side of an online match, no window.

Both link the same core as core/build.py, plus GekkoNet (pinned in the spike's
build), net.cpp and transport.cpp. Everything goes to spike/build/, with SDL3.dll next to the game.
"""
import glob
import importlib.util
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("corebuild", os.path.join(REPO, "core", "build.py"))
cb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cb)
sb = cb.sb

NET_LIBS = ["-lws2_32", "-lmswsock", "-lwinmm"] if sb.WINDOWS else ["-pthread"]


def gekko_objects():
    objs = []
    for g in sorted(glob.glob(os.path.join(sb.GEKKO, "src", "*.cpp"))):
        obj, err = sb.compile_one("gekko/" + os.path.basename(g), g, True)
        if obj is None:
            print("FAILED", g, err[:2000])
            return None
        objs.append(obj)
    return objs


def link(exe, objs, extra=()):
    static = ["-static-libstdc++", "-static-libgcc"] if sb.WINDOWS else []
    r = subprocess.run([sb.CXX] + static + ["-o", exe] + objs + NET_LIBS + list(extra), capture_output=True, text=True)
    if r.returncode:
        print("LINK FAILED", os.path.basename(exe))
        print(r.stderr[:4000])
        return False
    print("built", exe)
    return True


def main():
    core = cb.core_objects()
    gekko = gekko_objects()
    net = cb.compile_cpp(os.path.join(HERE, "net.cpp"))
    transport = cb.compile_cpp(os.path.join(HERE, "transport.cpp"))
    bot = cb.compile_cpp(os.path.join(HERE, "guasa_bot.cpp"))
    if None in (core, gekko, net, transport, bot):
        return 1
    common = core + gekko + [net, transport]
    ok = link(os.path.join(cb.OUT, "guasa-bot.exe" if sb.WINDOWS else "guasa-bot"), common + [bot])

    if sb.WINDOWS and os.path.isdir(sb.SDL):
        game = cb.compile_cpp(os.path.join(HERE, "guasa_game.cpp"), ["-I" + os.path.join(sb.SDL, "include")])
        if game is None:
            return 1
        ok = link(os.path.join(cb.OUT, "guasa-game.exe"), common + [game],
                  ["-L" + os.path.join(sb.SDL, "lib"), "-lSDL3", "-lshell32"]) and ok
        shutil.copy(os.path.join(sb.SDL, "bin", "SDL3.dll"), cb.OUT)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
