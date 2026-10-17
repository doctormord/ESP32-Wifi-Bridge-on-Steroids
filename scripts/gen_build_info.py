"""
Generates src/build_info.h with the current git revision, before every build.

Reason: a coredump only points at bare addresses, not source locations - those
can only be resolved with the EXACTLY matching firmware.elf (addr2line against
the wrong .elf yields plausible-looking but completely meaningless function
names). PlatformIO overwrites .pio/build/.../firmware.elf on every build, which
is why scripts/ota_flash.sh archives it under the git revision. For a coredump
found later to be matched to an archive entry at all, the running firmware
must know its own git revision - this script provides it as GIT_REV, which is
reported in /api/status among other places.
"""
import os
import subprocess

# __file__ is not reliably set in extra_scripts executed by SCons
# (SConscript() runs them via exec() without injecting it) - hence
# PlatformIO's env["PROJECT_DIR"] instead of a path relative to this script.
Import("env")

ROOT = env["PROJECT_DIR"]
OUT = os.path.join(ROOT, "src", "build_info.h")


def git_rev():
    """Short HEAD hash, with "-dirty" appended if the work tree has
    uncommitted changes; "unknown" outside a git checkout."""
    try:
        rev = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=ROOT, stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return "unknown"

    dirty = False
    try:
        subprocess.check_call(["git", "diff", "--quiet"], cwd=ROOT, stderr=subprocess.DEVNULL)
        subprocess.check_call(["git", "diff", "--cached", "--quiet"], cwd=ROOT, stderr=subprocess.DEVNULL)
    except Exception:
        dirty = True

    return rev + ("-dirty" if dirty else "")


with open(OUT, "w") as f:
    f.write("#pragma once\n")
    f.write('#define GIT_REV "%s"\n' % git_rev())
