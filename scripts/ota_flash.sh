#!/usr/bin/env bash
#
# Builds the firmware, archives the matching firmware.elf (named after the git
# revision from build_info.h) and uploads it via OTA.
#
# Reason: .pio/build/.../firmware.elf is overwritten on every build. Without
# an archive, a coredump found on the device later (/api/coredump) can no
# longer be resolved reliably - addr2line against a different build only
# yields meaningless matches.
#
# Usage: scripts/ota_flash.sh <ip> [admin-key]

set -euo pipefail

IP="${1:?Usage: scripts/ota_flash.sh <ip> [admin-key]}"
KEY="${2:-}"

cd "$(dirname "$0")/.."

"$HOME/.pio-py313/bin/pio" run -e wt32-eth01

REV=$(sed -n 's/.*GIT_REV "\(.*\)"/\1/p' src/build_info.h)
if [ -z "$REV" ]; then
  echo "Could not read GIT_REV from src/build_info.h - aborting." >&2
  exit 1
fi

TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p elf_archive
ARCHIVE="elf_archive/${REV}_${TS}.elf"
cp .pio/build/wt32-eth01/firmware.elf "$ARCHIVE"
echo "Archived: $ARCHIVE"

CURL_ARGS=(-s -m 180 -X POST --data-binary @.pio/build/wt32-eth01/firmware.bin
           -w "\nhttp:%{http_code} size_uploaded:%{size_upload} time:%{time_total}s\n")
if [ -n "$KEY" ]; then
  CURL_ARGS=(-H "X-Admin-Key: ${KEY}" "${CURL_ARGS[@]}")
fi

curl "${CURL_ARGS[@]}" "http://${IP}/api/update"
