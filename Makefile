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
CFLAGS   := -O2 -std=c11 -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter -Isrc $(SDL_CF)
GENFLAGS := -O1 -std=c11 -w -Isrc
BUILD    := build

RUNTIME  := src/main.c src/bus.c src/m68k_core.c src/m68k_interp.c src/vdp.c \
            src/z80.c src/ym2612.c src/psg.c src/state.c src/combo.c src/fx.c
GEN      := $(sort $(wildcard generated/recomp_*.c generated/*/recomp_*.c))
OBJS     := $(RUNTIME:src/%.c=$(BUILD)/%.o) $(GEN:generated/%.c=$(BUILD)/gen/%.o)

# The ROM is built into the executable when the file is present (EMBED=0 to
# opt out); otherwise the game loads it from disk at start-up.
EMBED    ?= $(shell test -f "$(ROM)" && echo 1 || echo 0)
ifeq ($(EMBED),1)
OBJS     += $(BUILD)/rom_embed.o $(BUILD)/rom_embed_data.o
else
OBJS     += $(BUILD)/rom_none.o
endif

.PHONY: all recompile recompile-umk3 coverage app tests clean

all: genexia

genexia: $(OBJS)
	$(CC) -o $@ $(OBJS) $(SDL_LD) -lm

$(BUILD)/%.o: src/%.c src/genesis.h src/m68k.h src/bus.h src/combo.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/rom.bin:
	@mkdir -p $(BUILD)
	cp "$(ROM)" $@

$(BUILD)/rom_embed_data.o: src/rom_embed.S $(BUILD)/rom.bin
	$(CC) -c -I$(BUILD) -o $@ src/rom_embed.S

$(BUILD)/gen/%.o: generated/%.c src/m68k.h src/genesis.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c -o $@ $<

src/m68k_interp.c: tools/m68kgen.py tools/gen_interp.py
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
	$(CC) -O2 -std=c11 -Isrc -o $(BUILD)/test_vdp src/vdp.c src/test_vdp.c && $(BUILD)/test_vdp
	$(CC) -O2 -std=c11 -Isrc -o $(BUILD)/test_sound src/z80.c src/ym2612.c src/psg.c src/test_sound.c -lm && $(BUILD)/test_sound

clean:
	rm -rf $(BUILD) genexia Genexia.app
