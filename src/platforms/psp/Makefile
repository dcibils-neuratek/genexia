# One packaged SNES game for the PSP; tools/package_game.py drives this.
#   make -f src/platforms/psp/Makefile GAME=<game folder> OUT=<dir> TITLE=<name>
PSPDEV ?= $(HOME)/pspdev
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC := $(PSPDEV)/bin/psp-gcc
OBJ := $(GAME)/obj-psp
CFLAGS := -std=gnu11 -O2 -G0 -w -fno-strict-aliasing -DGENEXIA_PSP -DROM_MAX=0x600000 \
  -I$(ROOT)/build -I$(ROOT)/src -I$(ROOT)/src/core -I$(ROOT)/src/chips -I$(ROOT)/src/backends/genesis \
  -I$(ROOT)/src/backends/snes -I$(PSPDEV)/psp/sdk/include -D_PSP_FW_VERSION=600
CORE := src/platforms/psp/psp_main.c src/chips/w65816.c src/backends/snes/machine.c src/backends/snes/ppu.c \
  src/backends/snes/apu.c src/backends/snes/dsp1.c
CODE := $(wildcard $(GAME)/code/*.c)
OBJS := $(CORE:%.c=$(OBJ)/core/%.o) $(CODE:$(GAME)/code/%.c=$(OBJ)/%.o) $(OBJ)/gamelib.o
LIBS := -L$(PSPDEV)/psp/sdk/lib -lpspaudio -lpspgu -lpspctrl -lpspdisplay -lpsppower -lpsprtc -lpspge -lpspdisplay \
  -lc -lm -lpspuser -lpspkernel

$(OUT)/EBOOT.PBP: $(OBJ)/game.prx
	mkdir -p $(OUT)
	$(PSPDEV)/bin/mksfoex -d MEMSIZE=1 '$(TITLE)' $(OBJ)/PARAM.SFO
	$(PSPDEV)/bin/pack-pbp $@ $(OBJ)/PARAM.SFO $(or $(ICON),NULL) NULL NULL NULL NULL $< NULL

$(OBJ)/game.prx: $(OBJS)
	$(CC) -specs=$(PSPDEV)/psp/sdk/lib/prxspecs -Wl,-q,-T$(PSPDEV)/psp/sdk/lib/linkfile.prx -L$(PSPDEV)/psp/lib $^ $(LIBS) -o $(OBJ)/game.elf
	$(PSPDEV)/bin/psp-fixup-imports $(OBJ)/game.elf
	$(PSPDEV)/bin/psp-prxgen $(OBJ)/game.elf $@

$(OBJ)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O3 -c $< -o $@
$(OBJ)/%.o: $(GAME)/code/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DGAME_LIB -c $< -o $@
$(OBJ)/gamelib.o: $(ROOT)/src/backends/snes/gamelib.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DGAME_LIB -DGAME_REGISTER=recomp_register_game -c $< -o $@
