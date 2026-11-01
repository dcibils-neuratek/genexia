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
INCS     := -Ibuild -Isrc -Isrc/core -Isrc/chips -Isrc/backends/genesis -Isrc/backends/snes -Isrc/backends/n64
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
GEN      := $(sort $(filter-out recomps/n64/sm64c/%,$(wildcard recomps/*/*/recomp_*.c)))
RDP_WORKERS := 0 1 2 3 4 5
OBJS     := $(RUNTIME:src/%.c=$(BUILD)/%.o) $(GEN:recomps/%.c=$(BUILD)/gen/%.o) $(RDP_WORKERS:%=$(BUILD)/chips/rdp_w%.o)

# The ROM is built into the executable when the file is present (EMBED=0 to
# opt out); otherwise the game loads it from disk at start-up.
EMBED    ?= $(shell test -f "$(ROM)" && echo 1 || echo 0)
ifeq ($(EMBED),1)
OBJS     += $(BUILD)/core/rom_embed.o $(BUILD)/rom_embed_data.o
else
OBJS     += $(BUILD)/core/rom_none.o
endif

.PHONY: FORCE os9 os9-n64bench os9-push os9-log all recompile recompile-umk3 recompile-smk recompile-sm64 coverage app tests clean

all: genexia

# Build number: commits so far and the commit, + when the code had uncommitted
# changes. Shown in the window title, the options menu and by --version.
BUILD_NO := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
BUILD_ID := $(BUILD_NO) $(shell git rev-parse --short HEAD 2>/dev/null)$(shell git diff --quiet HEAD -- src tools Makefile 2>/dev/null || echo +)
$(BUILD)/version.h: FORCE
	@mkdir -p $(BUILD)
	@echo '#define GENEXIA_BUILD "$(BUILD_ID)"' > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
$(BUILD)/core/main.o: $(BUILD)/version.h

genexia: $(OBJS)
	$(CC) -o $@ $(OBJS) $(SDL_LD) -lm

HDRS     := $(wildcard src/*.h src/*/*.h src/backends/*/*.h)
$(BUILD)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# the RDP once more per high-res worker, see src/chips/rdp.c
$(BUILD)/chips/rdp_w%.o: src/chips/rdp.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DRDP_WORKER=$* -c -o $@ $<

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

# dev: each recompiled block checked against the interpreter, see machine.c
LS_SRC := src/backends/snes/machine.c $(wildcard recomps/snes/smk/recomp_*.c)
LS_OBJ := $(patsubst %.c,$(BUILD)/ls/%.o,$(LS_SRC))
LS_REST := $(filter-out $(BUILD)/backends/snes/machine.o $(BUILD)/gen/snes/%,$(OBJS))
$(BUILD)/ls/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -std=gnu11 -w $(INCS) -DSNES_LOCKSTEP -c -o $@ $<
smk-lockstep: $(LS_REST) $(LS_OBJ)
	$(CC) -o genexia-lockstep $^ $(SDL_LD) -lm

# dev: Mario 64 from the compact recompiler, run on this Mac
N64C_OBJ := $(patsubst recomps/%.c,$(BUILD)/gen/%.o,$(wildcard recomps/n64/sm64c/recomp_*.c))
genexia-n64c: $(filter-out $(BUILD)/gen/n64/sm64/%,$(OBJS)) $(N64C_OBJ)
	$(CC) -o $@ $^ $(SDL_LD) -lm

N64L_OBJ := $(patsubst %.c,$(BUILD)/l6/%.o,src/chips/r4300.c $(wildcard recomps/n64/sm64c/recomp_*.c))
$(BUILD)/l6/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -std=gnu11 -w $(INCS) -DR4_LOCKSTEP -c -o $@ $<
n64c-lockstep: $(filter-out $(BUILD)/gen/n64/sm64/% $(BUILD)/chips/r4300.o,$(OBJS)) $(N64L_OBJ)
	$(CC) -o genexia-n64l $^ $(SDL_LD) -lm

SM64_ROM ?= roms/n64/Super Mario 64 (USA).z64
recompile-sm64c:
	python3 tools/recompile_mips32.py "$(SM64_ROM)" recomps/n64/sm64c --entries recomps/n64/sm64/coverage.txt --register recomp_register_sm64

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
	plutil -replace CFBundleVersion -string "$(BUILD_NO)" Genexia.app/Contents/Info.plist
	codesign --force -s - --deep Genexia.app
	@if [ "$(EMBED)" = 1 ]; then echo "Genexia.app is self-contained (ROM built in)"; \
	 else echo "No ROM embedded: put it next to the app or in Genexia.app/Contents/MacOS/ as sf2.bin"; fi

# Mac OS 9 on PowerPC, with Retro68 (RETRO68 = its build folder). Leaves
# build/os9/Genexia.dsk: an HFS disk with the app and, when ROM exists, sf2.bin.
RETRO68 ?= $(HOME)/retro68-work/Retro68-build
os9: $(BUILD)/version.h
	PATH="$(RETRO68)/toolchain/bin:$$PATH" cmake -S src/platforms/os9 -B $(BUILD)/os9/cmake \
	  -DCMAKE_TOOLCHAIN_FILE=$(RETRO68)/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
	PATH="$(RETRO68)/toolchain/bin:$$PATH" cmake --build $(BUILD)/os9/cmake -- -j8
	cp $(BUILD)/os9/cmake/Genexia.bin $(BUILD)/os9/Genexia.bin
	dd if=/dev/zero of=$(BUILD)/os9/Genexia.dsk bs=1m count=32 2>/dev/null
	PATH="$(RETRO68)/toolchain/bin:$$PATH"; hformat -l Genexia $(BUILD)/os9/Genexia.dsk >/dev/null && \
	  hcopy -m $(BUILD)/os9/Genexia.bin :Genexia && \
	  { [ ! -f "$(ROM)" ] || hcopy -r "$(ROM)" :sf2.bin; } && humount
	@ls -l $(BUILD)/os9/Genexia.dsk

# Send the Mac OS 9 build to a machine with File Sharing on, read its log
IMAC ?= 192.168.1.38
os9-push: os9
	python3 tools/os9_share.py $(IMAC) push $(BUILD)/os9/Genexia.bin "Genexia $(BUILD_NO)"
# Times Mario 64's game code on the iMac; needs recompile-sm64c first.
os9-n64bench: $(BUILD)/version.h
	PATH="$(RETRO68)/toolchain/bin:$$PATH" cmake -S tools/os9_n64bench -B $(BUILD)/os9/n64bench \
	  -DCMAKE_TOOLCHAIN_FILE=$(RETRO68)/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
	PATH="$(RETRO68)/toolchain/bin:$$PATH" cmake --build $(BUILD)/os9/n64bench -- -j8
os9-log:
	@python3 tools/os9_share.py $(IMAC) get genexia.log

tests:
	@mkdir -p $(BUILD)
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_vdp src/backends/genesis/vdp.c src/backends/genesis/test_vdp.c && $(BUILD)/test_vdp
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_ppu src/backends/snes/ppu.c src/backends/snes/test_ppu.c && $(BUILD)/test_ppu | tail -1
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_apu src/backends/snes/apu.c src/backends/snes/dsp1.c src/backends/snes/test_apu.c -lm && $(BUILD)/test_apu | tail -1
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_sound src/chips/z80.c src/chips/ym2612.c src/chips/psg.c src/chips/test_sound.c -lm && $(BUILD)/test_sound
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_r4300 src/chips/r4300.c src/chips/test_r4300.c && $(BUILD)/test_r4300
	$(CC) -O2 -std=c11 $(INCS) -o $(BUILD)/test_rsp src/chips/test_rsp.c && $(BUILD)/test_rsp
	$(CC) -O2 -std=c11 $(INCS) -DRDP_NO_WORKERS -o $(BUILD)/test_rdp src/chips/rdp.c src/chips/test_rdp.c -lm && $(BUILD)/test_rdp

clean:
	rm -rf $(BUILD) genexia Genexia.app

# Mario 64 through src/chips/f3d.c, off-screen on this Mac; needs recompile-sm64c.
GLT_FLAGS := -O2 -std=gnu11 -w -DN64_BENCH $(INCS)
n64gl: $(wildcard $(BUILD)/gen/n64/sm64c/recomp_*.o)
	$(CC) $(GLT_FLAGS) tools/n64gl/gltest.c src/chips/f3d.c src/chips/aspmain.c src/chips/r4300.c src/backends/n64/machine.c $^ -framework OpenGL -lm -o genexia-n64gl

# Audio tasks through src/chips/aspmain.c and the real RSP, compared.
n64-aspcheck: $(filter-out $(BUILD)/gen/n64/sm64/% $(BUILD)/backends/n64/machine.o,$(OBJS)) $(wildcard $(BUILD)/gen/n64/sm64c/recomp_*.o)
	$(CC) -DASP_CHECK -c $(CFLAGS) src/backends/n64/machine.c -o $(BUILD)/asp_machine.o
	$(CC) -c $(CFLAGS) src/chips/aspmain.c -o $(BUILD)/aspmain.o
	$(CC) -o genexia-n64a $^ $(BUILD)/asp_machine.o $(BUILD)/aspmain.o $(SDL_LD) -lm
