#!/bin/bash
# Plays a recompiled PSP game in a window: tools/psp_play.sh GAME_ID PROGRAM DISC.iso
#   PSP_SCALE sets how many times the PSP's resolution to draw at, 4 unless told
#   PSP_SIZE sets the window, as 1920x1088; PSP_FULLSCREEN=1 starts fullscreen; Cmd-F switches
#   PSP_MSAA sets anti-aliasing samples: 1 for none, 2, or 4 unless told
#   arrows D-pad, Z Cross, X Circle, A Square, S Triangle, Q/W L/R, Return Start, Tab Select
set -eo pipefail
cd "$(dirname "$0")/.."
G="$HOME/Library/Application Support/Genexia/games-psp/$1"
python3 tools/psp_nids.py build/psp_nids.h
clang -O2 -w -c -fobjc-arc -Isrc/backends/psp $(sdl2-config --cflags) src/backends/psp/psp_metal.m -o build/psp_metal.o
clang -O2 -w -std=gnu11 -DPSP_WINDOW -DPSP_METAL -Isrc/chips -Ibuild -Isrc/backends/psp $(sdl2-config --cflags) \
  src/backends/psp/psp_runtime.c src/backends/psp/psp_vfpu.c src/backends/psp/psp_ge.c build/psp_metal.o "$G"/obj/*.o \
  $(sdl2-config --libs) -lm -framework Metal -framework QuartzCore -framework Foundation -o "$G/play"
exec "$G/play" "$2" "$3" 999999
