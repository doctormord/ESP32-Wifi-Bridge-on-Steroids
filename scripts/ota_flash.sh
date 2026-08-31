#!/usr/bin/env bash
#
# Baut die Firmware, archiviert das dazugehoerige firmware.elf (benannt nach
# dem Git-Stand aus build_info.h) und spielt sie per OTA auf.
#
# Grund: .pio/build/.../firmware.elf wird bei jedem Build ueberschrieben.
# Ohne Archiv ist ein spaeter am Geraet gefundener Coredump (/api/coredump)
# nicht mehr zuverlaessig aufloesbar - am 2026-08-31 auf echter Hardware
# genau so erlebt: ein alter Coredump war da, aber das passende .elf laengst
# weg, addr2line gegen den aktuellen Build lieferte nur sinnlose Treffer.
#
# Usage: scripts/ota_flash.sh <ip> [admin-key]

set -euo pipefail

IP="${1:?Usage: scripts/ota_flash.sh <ip> [admin-key]}"
KEY="${2:-}"

cd "$(dirname "$0")/.."

"$HOME/.pio-py313/bin/pio" run -e wt32-eth01

REV=$(sed -n 's/.*GIT_REV "\(.*\)"/\1/p' src/build_info.h)
if [ -z "$REV" ]; then
  echo "Konnte GIT_REV nicht aus src/build_info.h lesen - Build abgebrochen." >&2
  exit 1
fi

TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p elf_archive
ARCHIVE="elf_archive/${REV}_${TS}.elf"
cp .pio/build/wt32-eth01/firmware.elf "$ARCHIVE"
echo "Archiviert: $ARCHIVE"

CURL_ARGS=(-s -m 180 -X POST --data-binary @.pio/build/wt32-eth01/firmware.bin
           -w "\nhttp:%{http_code} size_uploaded:%{size_upload} time:%{time_total}s\n")
if [ -n "$KEY" ]; then
  CURL_ARGS=(-H "X-Admin-Key: ${KEY}" "${CURL_ARGS[@]}")
fi

curl "${CURL_ARGS[@]}" "http://${IP}/api/update"
