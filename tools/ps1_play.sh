#!/bin/bash
# Plays a PlayStation disc in a window: tools/ps1_play.sh GAME.cue [BIOS.bin]
#   the BIOS is your own dump, scph5501.bin from ~/Downloads unless told
#   PS1_FAST=1 skips the BIOS intro and starts the game's program at once
#   arrows D-pad, Z Cross, X Circle, A Square, S Triangle, Q/W L1/R1, E/R L2/R2,
#   Return Start, Tab Select, Cmd-F fullscreen
set -eo pipefail
cd "$(dirname "$0")/.."
BIOS="${2:-$HOME/Downloads/scph5501.bin}"
mkdir -p build/ps1
clang -O2 -std=gnu11 $(sdl2-config --cflags) src/backends/ps1/*.c $(sdl2-config --libs) -lz -o build/ps1/ps1
exec build/ps1/ps1 "$BIOS" "$1"
