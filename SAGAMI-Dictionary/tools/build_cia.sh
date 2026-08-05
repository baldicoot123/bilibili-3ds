#!/usr/bin/env bash
set -euo pipefail

if ! command -v makerom >/dev/null 2>&1; then
  echo "makerom is required to build a CIA" >&2
  exit 1
fi

if ! command -v bannertool >/dev/null 2>&1; then
  echo "bannertool is required to build a CIA banner" >&2
  exit 1
fi

if [[ ! -f sagami_dictionary.elf || ! -f sagami_dictionary.smdh ]]; then
  echo "Build the 3DSX/SMDH first" >&2
  exit 1
fi

bannertool makebanner -i cia/banner.png -a cia/audio.wav \
  -o build/banner.bnr >/dev/null

makerom -f cia -o sagami_dictionary.cia \
  -elf sagami_dictionary.elf \
  -rsf cia/sagami.rsf \
  -DAPP_ROMFS="$(pwd)/romfs" \
  -banner build/banner.bnr \
  -icon sagami_dictionary.smdh \
  -exefslogo -target t
