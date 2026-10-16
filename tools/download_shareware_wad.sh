#!/usr/bin/env bash
# Downloads the legally redistributable Doom shareware IWAD (doom1.wad).
# Copy the file to the root of a FAT32 microSD card for the VGA32.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="${ROOT}/data"
mkdir -p "${DEST_DIR}"

URL="${DOOM_WAD_URL:-https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad}"
OUT="${DEST_DIR}/doom1.wad"

echo "Downloading shareware IWAD to ${OUT}"
curl -L --fail --retry 3 -o "${OUT}" "${URL}"
ls -l "${OUT}"
echo
echo "Copy data/doom1.wad to the root of the microSD card, then insert it in the VGA32."
echo "Remove the SD card before USB firmware upload (GPIO12 is a strapping pin)."
