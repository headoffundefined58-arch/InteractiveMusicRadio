"""Configure / build InteractiveMusicRadio inside a proper MSVC environment.

CMake cannot locate cl.exe on its own here, so we capture the environment
produced by vcvars64.bat (routed through a helper .bat, which is the working
pattern used by the sibling CMS / AnatomicalCombat projects) and hand it to
every child process.  The proxy vars are stripped so MSBuild's case-insensitive
env map cannot throw MSB6001.

Usage:
    python build.py              # configure + build Release
    python build.py --configure  # configure only
    python build.py --fresh      # wipe build dir first
"""
import os
import shutil
import subprocess
import sys

VCVARS = (r"C:\Program Files\Microsoft Visual Studio\2022\Community"
          r"\VC\Auxiliary\Build\vcvars64.bat")
CMAKE = (r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
         r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")

SOURCE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(SOURCE, "build")


def msvc_env() -> dict:
    """Capture vcvars64's environment via a helper .bat (cmd /c mangles a quoted
    program path, so we route through a file)."""
    if not os.path.isfile(VCVARS):
        raise RuntimeError(f"vcvars64.bat not found at {VCVARS}")
    helper = os.path.join(SOURCE, "_msvc_env.bat")
    with open(helper, "w", encoding="utf-8") as fh:
        fh.write("@echo off\n")
        fh.write(f'call "{VCVARS}"\n')
        fh.write("set\n")
    proc = subprocess.run(
        ["cmd", "/c", helper],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        raise RuntimeError(f"vcvars64.bat failed: {proc.stderr[:500]}")
    env = dict(os.environ)
    for line in proc.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            env[key] = value
    # Strip proxy vars (case-insensitive) so MSBuild never sees the duplicate
    # HTTP_PROXY / http_proxy pair that triggers MSB6001.
    for k in list(env.keys()):
        if k.lower().endswith("_proxy"):
            env.pop(k, None)
    return env


def run(cmd: list, env: dict) -> int:
    print(f"\n$ {' '.join(cmd)}\n", flush=True)
    proc = subprocess.run(
        cmd, env=env, capture_output=True, text=True,
        encoding="utf-8", errors="replace",
    )
    tail = 8000
    if proc.stdout:
        print(proc.stdout[-tail:], flush=True)
    if proc.stderr:
        print(proc.stderr[-tail:], flush=True)
    print(f"[exit {proc.returncode}]", flush=True)
    return proc.returncode


def main() -> int:
    configure_only = "--configure" in sys.argv
    env = msvc_env()

    if os.path.isdir(BUILD) and "--fresh" in sys.argv:
        print(f"removing {BUILD}", flush=True)
        shutil.rmtree(BUILD, ignore_errors=True)

    rc = run([CMAKE, "-B", BUILD, "-S", SOURCE,
              "-G", "Visual Studio 17 2022", "-A", "x64"], env)
    if rc != 0:
        return rc

    if configure_only:
        return 0

    return run([CMAKE, "--build", BUILD, "--config", "Release"], env)


if __name__ == "__main__":
    sys.exit(main())
