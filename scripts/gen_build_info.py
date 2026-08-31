"""
Erzeugt src/build_info.h mit dem aktuellen Git-Stand, vor jedem Build neu.

Grund: ein Coredump verweist nur auf nackte Adressen, keine Quelltextstellen -
die lassen sich nur mit dem GENAU passenden firmware.elf aufloesen (addr2line
gegen das falsche .elf liefert scheinbar gueltige, aber voellig sinnlose
Funktionsnamen - am 2026-08-31 auf echter Hardware genau so erlebt). PlatformIO
ueberschreibt .pio/build/.../firmware.elf bei jedem Build, darum archiviert
scripts/ota_flash.sh es nach dem Git-Stand benannt. Damit sich ein spaeter
gefundener Coredump ueberhaupt einem Archiv-Eintrag zuordnen laesst, muss die
laufende Firmware ihren eigenen Git-Stand kennen - das liefert dieses Skript
als GIT_REV, ausgegeben unter anderem in /api/status.
"""
import os
import subprocess

# __file__ ist in SCons-ausgefuehrten extra_scripts nicht verlaesslich
# gesetzt (SConscript() fuehrt per exec() aus, ohne es zu injizieren) -
# deshalb ueber PlatformIOs env["PROJECT_DIR"] statt relativ zum Skript.
Import("env")

ROOT = env["PROJECT_DIR"]
OUT = os.path.join(ROOT, "src", "build_info.h")


def git_rev():
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
