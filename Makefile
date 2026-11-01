# Genexia - a Genesis / Mega Drive recompilation platform for macOS
#
#   make            build ./genexia (recompiled game + runtime)
#   make recompile  regenerate recomps/genesis/sf2 from the ROM and its coverage list
#   make coverage   run the game headless and append newly found entry points
#   make app        build Genexia.app bundle
#   make tests      build and run component tests

ROM      ?= Street Fighter 2 Plus Champion Edition (J) [!].bin
CC       := clang
SDL_CF   := $(shell sdl2-config --cflags)
SDL_LD   := $(shell sdl2-config --libs)
# core/ = shared front end, chips/ = reusable chips with no console knowledge,
# backends/<console>/ = what is unique to one console
INCS     := -Isrc -Isrc/core -Isrc/chips -Isrc/backends/genesis -Isrc/backends/snes -Isrc/backends/n64
CFLAGS   := -O2 -std=c11 -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter $(INCS) $(SDL_CF)
GENFLAGS := -O1 -std=c11 -w $(INCS)
BUILD    := build

RUNTIME  := src/core/main.c src/core/state.c src/core/enhance.c \
            src/chips/m68k_core.c src/chips/m68k_interp.c src/chips/z80.c \
            src/chips/ym2612.c src/chips/psg.c \
            src/backends/genesis/bus.c src/backends/genesis/vdp.c \
            src/backends/genesis/combo.c src/backends/genesis/fx.c \
            src/chips/w65816.c src/backends/snes/machine.c src/backends/snes/ppu.c \
            src/backends/snes/apu.c src/backends/snes/dsp1.c \
            src/chips/r4300.c src/chips/rsp.c src/chips/rdp.c src/backends/n64/machine.c
# recompiled games: recomps/<console>/<game>/, generated code plus coverage.txt
GEN      := $(sort $(wildcard recomps/*/*/recomp_*.c))
OBJS     := $(RUNTIME:src/%.c=$(BUILD)/%.o) $(GEN:recomps/%.c=$(BUILD)/gen/%.o)

# The ROM is built into the executable when the file is present (EMBED=0 to
# opt out); otherwise the game loads it from disk at start-up.
EMBED    ?= $(shell test -f "$(ROM)" && echo 1 || echo 0)
ifeq ($(EMBED),1)
OBJS     += $(BUILD)/core/rom_embed.o $(BUILD)/rom_embed_data.o
else
OBJS     += $(BUILD)/core/rom_none.o
endif

.PHONY: all recompile recompile-umk3 recompile-smk recompile-sm64 coverage app tests clean

all: genexia

genexia: $(OBJS)
	$(CC) -o $@ $(OBJS) $(SDL_LD) -lm

HDRS     := $(wildcard src/*.h src/*/*.h src/backends/*/*.h)
$(BUILD)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/rom.bin:
	@mkdir -p $(BUILD)
	cp "$(ROM)" $@

$(BUILD)/rom_embed_data.o: src/core/rom_embed.S $(BUILD)/rom.bin
	$(CC) -c -I$(BUILD) -o $@ src/core/rom_embed.S

$(BUILD)/gen/n64/%.o: GENFLAGS := -O2 -std=c11 -w $(INCS)
$(BUILD)/gen/%.o: recomps/%.c src/chips/m68k.h src/chips/r4300_recomp.h src/genesis.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c -o $@ $<

src/chips/m68k_interp.c: tools/m68kgen.py tools/gen_interp.py
	python3 tools/gen_interp.py $@

recompile:
	python3 tools/recompile.py "$(ROM)" recomps/genesis/sf2 --entries recomps/genesis/sf2/coverage.txt --listing docs/sf2.lst

# second game: UMK3_ROM recompiled into recomps/genesis/umk3 with its own symbols
UMK3_ROM ?= roms/Ultimate Mortal Kombat 3 (USA).md
recompile-umk3:
	python3 tools/recompile.py "$(UMK3_ROM)" recomps/genesis/umk3 --entries recomps/genesis/umk3/coverage.txt --listing docs/umk3.lst --prefix U_ --register recomp_register_umk3

SMK_ROM ?= roms/snes/Super Mario Kart (USA).sfc
recompile-smk:
	python3 tools/recompile_65816.py "$(SMK_ROM)" recomps/snes/smk --entries recomps/snes/smk/coverage.txt --listing docs/smk.lst --prefix S_ --register recomp_register_smk

SM64_ROM ?= roms/n64/Super Mario 64 (USA).z64
recompile-sm64:
	python3 tools/recompile_mips.py "$(SM64_ROM)" recomps/n64/sm64 --entries recomps/n64/sm64/coverage.txt --prefix M_ --register recomp_register_sm64

coverage: genexia
	SF2_MISSLOG=recomps/genesis/sf2/coverage.txt ./genexia --frames 4000 "$(ROM)"
	sort -u recomps/genesis/sf2/coverage.txt -o recomps/genesis/sf2/coverage.txt

app: genexia
	rm -rf Genexia.app
	mkdir -p Genexia.app/Contents/MacOS Genexia.app/Contents/Frameworks Genexia.app/Contents/Resources
	cp genexia Genexia.app/Contents/MacOS/genexia
	cp "$$(sdl2-config --prefix)/lib/libSDL2-2.0.0.dylib" Genexia.app/Contents/Frameworks/
	@# Homebrew's SDL2 is sdl2-compat, which loads SDL3 at start-up from
	@# @loader_path/libSDL3.dylib: bundle the real SDL3 library next to it
	if [ -e "$$(sdl2-config --prefix)/lib/libSDL3.dylib" ] || [ -e /opt/homebrew/lib/libSDL3.dylib ]; then \
	  cp -L "$$(ls "$$(sdl2-config --prefix)/lib/libSDL3.dylib" /opt/homebrew/lib/libSDL3.dylib 2>/dev/null | head -1)" Genexia.app/Contents/Frameworks/libSDL3.dylib; \
	  install_name_tool -id @loader_path/libSDL3.dylib Genexia.app/Contents/Frameworks/libSDL3.dylib; fi
	install_name_tool -change "$$(otool -L genexia | awk '/libSDL2/{print $$1}')" @executable_path/../Frameworks/libSDL2-2.0.0.dylib Genexia.app/Contents/MacOS/genexia
	cp tools/Info.plist Genexia.app/Contents/Info.plist
	codesign --force -s - --deep Genexia.app
	@if [ "$(EMBED)" = 1 ]; then echo "Genexia.app is self-contained (ROM built in)"; \
	 else echo "No ROM embedded: put it next to the app or in Genexia.app/Contents/MacOS/ as sf2.bin"; fi

tests:
	@mkdir -p $(BUILD)
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_vdp src/backends/genesis/vdp.c src/backends/genesis/test_vdp.c && $(BUILD)/test_vdp
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_ppu src/backends/snes/ppu.c src/backends/snes/test_ppu.c && $(BUILD)/test_ppu | tail -1
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_apu src/backends/snes/apu.c src/backends/snes/dsp1.c src/backends/snes/test_apu.c -lm && $(BUILD)/test_apu | tail -1
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_sound src/chips/z80.c src/chips/ym2612.c src/chips/psg.c src/chips/test_sound.c -lm && $(BUILD)/test_sound
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_r4300 src/chips/r4300.c src/chips/test_r4300.c && $(BUILD)/test_r4300
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_rsp src/chips/test_rsp.c && $(BUILD)/test_rsp
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_rdp src/chips/rdp.c src/chips/test_rdp.c -lm && $(BUILD)/test_rdp

clean:
	rm -rf $(BUILD) genexia Genexia.app
