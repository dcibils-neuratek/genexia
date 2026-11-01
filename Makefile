# Genexia - a Genesis / Mega Drive recompilation platform for macOS
#
#   make            build ./genexia (recompiled game + runtime)
#   make recompile  regenerate generated/*.c from the ROM (+ coverage/entries.txt)
#   make coverage   run the game headless and append newly found entry points
#   make app        build Genexia.app bundle
#   make tests      build and run component tests

ROM      ?= Street Fighter 2 Plus Champion Edition (J) [!].bin
CC       := clang
SDL_CF   := $(shell sdl2-config --cflags)
SDL_LD   := $(shell sdl2-config --libs)
# core/ = shared front end, chips/ = reusable chips with no console knowledge,
# backends/<console>/ = what is unique to one console
INCS     := -Isrc -Isrc/core -Isrc/chips -Isrc/backends/genesis -Isrc/backends/snes
CFLAGS   := -O2 -std=c11 -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter $(INCS) $(SDL_CF)
GENFLAGS := -O1 -std=c11 -w $(INCS)
BUILD    := build

RUNTIME  := src/core/main.c src/core/state.c \
            src/chips/m68k_core.c src/chips/m68k_interp.c src/chips/z80.c \
            src/chips/ym2612.c src/chips/psg.c \
            src/backends/genesis/bus.c src/backends/genesis/vdp.c \
            src/backends/genesis/combo.c src/backends/genesis/fx.c \
            src/chips/w65816.c src/backends/snes/machine.c src/backends/snes/ppu.c \
            src/backends/snes/apu.c src/backends/snes/dsp1.c
GEN      := $(sort $(wildcard generated/recomp_*.c generated/*/recomp_*.c))
OBJS     := $(RUNTIME:src/%.c=$(BUILD)/%.o) $(GEN:generated/%.c=$(BUILD)/gen/%.o)

# The ROM is built into the executable when the file is present (EMBED=0 to
# opt out); otherwise the game loads it from disk at start-up.
EMBED    ?= $(shell test -f "$(ROM)" && echo 1 || echo 0)
ifeq ($(EMBED),1)
OBJS     += $(BUILD)/core/rom_embed.o $(BUILD)/rom_embed_data.o
else
OBJS     += $(BUILD)/core/rom_none.o
endif

.PHONY: all recompile recompile-umk3 coverage app tests clean

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

$(BUILD)/gen/%.o: generated/%.c src/chips/m68k.h src/genesis.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c -o $@ $<

src/chips/m68k_interp.c: tools/m68kgen.py tools/gen_interp.py
	python3 tools/gen_interp.py $@

recompile:
	python3 tools/recompile.py "$(ROM)" generated --entries coverage/entries.txt --listing docs/sf2.lst

# second game: UMK3_ROM recompiled into generated/umk3 with its own symbols
UMK3_ROM ?= roms/Ultimate Mortal Kombat 3 (USA).md
recompile-umk3:
	python3 tools/recompile.py "$(UMK3_ROM)" generated/umk3 --entries coverage/umk3.txt --listing docs/umk3.lst --prefix U_ --register recomp_register_umk3

coverage: genexia
	@mkdir -p coverage
	SF2_MISSLOG=coverage/entries.txt ./genexia --frames 4000 "$(ROM)"
	sort -u coverage/entries.txt -o coverage/entries.txt

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

clean:
	rm -rf $(BUILD) genexia Genexia.app
