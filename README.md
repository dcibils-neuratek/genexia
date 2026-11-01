# Genexia

**Genexia is a console recompilation platform for macOS.** It translates a game's
code into native Mac code and runs it on reimplemented console hardware, with one
shared front end for every console: video filters, options menu, controls, speed,
run-ahead and save states. Backends today: Sega Genesis / Mega Drive, SNES and Nintendo 64.

| Game | Status |
|---|---|
| Street Fighter II' Special Champion Edition | Recompiled and built in; widescreen, killer combos, mana |
| Ultimate Mortal Kombat 3 (USA) | Recompiled; widescreen; ROM loaded with `--rom` |
| Any other Genesis ROM | Runs on the interpreter |
| Super Mario Kart (USA), SNES | Recompiled, widescreen races (Mode 7 draws real extra track); ROM loaded with `--rom` |
| Other SNES ROMs | Run on the interpreter; only DSP-1 cartridges have their extra chip implemented |
| Super Mario 64 (USA), Nintendo 64 | Recompiled, about 3.7x full speed; 4:3 only; ROM loaded with `--rom` |
| Other N64 ROMs | Run on the interpreter; only Super Mario 64 has been tested |

## Architecture

| Folder | What goes there |
|---|---|
| `src/core/` | Shared front end: window, filters, menu, input, audio output, pacing, run-ahead, save states, and the `Backend` interface (`backend.h`) |
| `src/chips/` | Reusable chips with no console knowledge: 68000, Z80, 65C816, YM2612, PSG, R4300i, RSP, RDP. Each talks to the machine through callbacks, so a new console reuses them directly |
| `src/backends/<console>/` | Only what is unique to one console: memory map, video chip, wiring, frame timing |
| `tools/` | Recompiler pipeline: `recompile.py` (68000), `recompile_65816.py` (65C816, per address and M/X mode), `recompile_mips.py` (N64) |
| `recomps/<console>/<game>/` | One folder per recompiled game: generated C and its `coverage.txt` |

Every emulated variable is tagged `EMU_STATE`, which is why save states and
run-ahead work on every backend without per-chip code.

This is a static recompilation of the Mega Drive / Genesis ROM
`Street Fighter 2 Plus Champion Edition (J) [!].bin` (T-12033, 3 MB, checksum `FB5E`)
into a native arm64 macOS program.

The game's 68000 machine code has been translated into C (`recomps/genesis/sf2/recomp_*.c`,
16,525 functions) and compiled with clang. The hardware the game talks to is
reimplemented in C: the VDP video chip, the YM2612 FM synth, the SN76489 PSG,
the Z80 sound CPU, the controllers and the bus. A generated 68000 interpreter
is kept only as a safety net for code the recompiler never found. In the final
verification rounds it was never needed: 20 headless sessions of about 25 minutes
each, covering menus, character select and fights.

## Recompiled vs emulated

A console game is machine code for a CPU the Mac doesn't have. There are two ways
to run it, and Genexia uses both.

**Interpreted (emulated).** A loop in C reads one console instruction at a time,
works out what it means, and does it. The decoding is repeated every time the
instruction runs, even in a loop that runs a million times. It works for any ROM
straight away, with no preparation.

**Recompiled.** Before the game runs, a tool translates its machine code into C
once, and clang compiles that into native Mac code. The decoding work is gone at
run time, because the Mac simply executes the translated code.

| | Interpreted | Recompiled |
|---|---|---|
| Which games | Any ROM | Only games prepared in `recomps/` |
| Preparation | None | Run the recompiler for that exact ROM, then compile |
| CPU code | Decoded while playing | Native Mac code |
| Speed | Slower | Faster: Super Mario 64 runs 1.8x faster overall |
| Forcing it | `--interp` | The default when a recompile exists |

Only the game's **CPU code** is recompiled. The rest of the console is still
reimplemented in C either way: video, sound, controllers and, on N64, the RSP and
RDP. So a recompiled game still needs Genexia's backends, and it still needs its ROM
for graphics and sound data. It is also why the speed gain has a ceiling, since
much of the N64's time goes to those chips.

The two also work together. Code the recompiler never saw, or anything unusual
such as a hardware register access on N64, falls back to the interpreter, so a
game keeps working even with partial coverage.

### What the `recomp_*.c` files are

They are the game's own program, translated. Each C function is one **block**: a
short run of the game's instructions, ending at a jump. A single instruction like
`addiu t2, t2, 8` becomes one line such as `gpr[10] = gpr[10] + 8;`, where `gpr`
holds the console CPU's registers. At the end of a block, the code moves on to the
block the game would have jumped to.

There are so many because a game is a lot of code: tens of thousands of blocks.

| Game | Files | Size |
|---|---|---|
| Street Fighter II | 13 | 6 MB |
| Ultimate Mortal Kombat 3 | 14 | 10 MB |
| Super Mario Kart | 8 | 3 MB |
| Super Mario 64 | 22 | 96 MB (62,000 blocks) |

Splitting them across numbered files means nothing except keeping each one a
reasonable size, so clang can compile them in parallel. `recomp_register.c` is the
index: it tells Genexia which function belongs to which console address. Each
game's `coverage.txt` lists the addresses the recompiler should start from, which
are found by playing the game.

These files are generated, so never edit them by hand. They contain the game's
code, so they aren't committed: `make recompile-*` recreates them from your own ROM.

## Getting started from a fresh clone

This repository contains no game data: no ROMs, recompiled game code or
disassemblies. Bring your own legally obtained ROMs. Put the SF2 ROM in the
project folder and any others in `roms/`, then generate the game code once:

```bash
make recompile && make recompile-umk3 && make recompile-smk && make recompile-sm64
```

## Build & run

Requires the Xcode command line tools and SDL2 (`brew install sdl2`). Keep the
ROM file in the project folder when you build.

```bash
make app
```

That builds **`Genexia.app`, a self-contained Mac app**: double-click it, or drag it
to Applications. The ROM is built into the executable. It provides all the
graphics, music, voices and game tables, which the recompiled code still reads,
so no ROM file is needed next to the app. The SDL2 and SDL3 libraries are bundled
inside too. Save states go to `~/Library/Application Support/Genexia/Genexia/`.

For development, `make` builds the plain `./genexia` executable, which also has the
ROM built in:

```bash
make && ./genexia
```

### Playing another Genesis ROM

Run any Genesis / Mega Drive ROM with `--rom`:

```bash
./genexia --rom "roms/Ultimate Mortal Kombat 3 (USA).md"
```

With the app:

```bash
Genexia.app/Contents/MacOS/genexia --rom "roms/Ultimate Mortal Kombat 3 (USA).md"
```

Two games are recompiled to native code: Street Fighter II' SCE and Ultimate
Mortal Kombat 3 (USA). The app recognises each exact ROM by its header and uses
the matching code from `recomps/genesis/`. Any other ROM runs on
the built-in 68000 interpreter with the same video and sound chips. Only the
SF2 code is paired with a built-in ROM, so UMK3 still needs `--rom`.

Both recompiled games start in 16:9 widescreen with their HUD pinned to the
centre (**F2** switches back to the original picture). Combos, mana and auras are
SF2-only. Other games use the original picture with hardware sprite limits. `--wide` and `--no-sprite-limit` turn those on anyway,
though widescreen only looks right if the game keeps graphics beyond its view.
The console region comes from the ROM header (override with `--region`).
Speed control, run-ahead, save states (F5/F7) and the CRT effect work on every
game. Save states go next to that ROM, as `<rom>.state`.

A ROM path on the command line overrides the built-in copy. `make EMBED=0`
builds without embedding; the game then looks for the ROM file next to itself
at start-up.

### Controls (6-button pad)

| Player 1 | Key | Player 2 | Key |
|---|---|---|---|
| Move | Arrow keys | Move | I J K L |
| Light / Medium / Heavy punch | A S D | Punches | R T Y |
| Light / Medium / Heavy kick | Z X C | Kicks | F G H |
| Start | Return | Start | 1 |
| Mode | Right Shift | | |

**Gamepad** (Xbox / PlayStation / Switch Pro and other SDL controllers). The first controller to press a button becomes P1, the next becomes P2. Assignments are printed in the terminal:

| Button (Xbox / PlayStation) | Action |
|---|---|
| D-pad or left stick | Move |
| X / Square | Light punch |
| Y / Triangle | Medium punch |
| RB / R1 | Heavy punch |
| A / Cross | Light kick |
| B / Circle | Medium kick |
| RT / R2 | Heavy kick |
| **LB / L1** | **Fire combo** (when the mana bar says READY) |
| **LT / L2** | **Thunder combo** (when the mana bar says READY) |
| Start / Options | Start (also cancels a running combo) |
| Back / Share | Mode |

Other keys: **P** pause,
**Tab** (hold) fast-forward, **F1** reset, **F2** widescreen on/off,
**F3** CRT effect on/off, **F4** sprite-limit removal on/off, **F5** save state,
**F7** load state, **F6** run-ahead 0/1/2, **F11** fullscreen, **Esc** quit.

### Killer combos with special effects

Every character has two fixed **killer combos** of 8–9 moves, mixing normal
punches, kicks and jumps with that character's special moves. They're built
only from the special-move inputs of this version.

**How to trigger a combo**

1. Start a fight.
2. Watch the **mana bar** under your health bar and name. It starts empty
   every round and fills over **10 seconds** of fighting.
3. When it's full, it glows gold, says **READY**, and shows your keys underneath.
4. Press a combo key. The combo plays itself and the bar empties. Pressing too
   early flashes **NOT READY**.

| | Player 1 | Player 2 | Controller |
|---|---|---|---|
| **Fire combo**: flame aura, rim light, embers | **Q** | **U** | Left bumper |
| **Thunder combo**: electric glow, lightning bolts | **W** | **O** | Left trigger |

The bar doesn't fill while your combo is running, so you get a few combos per
round instead of one after another. Change the fill time with `--mana 15`, or
turn the meter off with `--mana 0`. The fill time is in game time, so it
follows the speed setting.

While a combo runs, its name and a live **hit counter** appear on your side of
the screen, and the counter pops on every hit. **Start** cancels a combo.
Directions follow where the opponent is, so a cross-up mid-combo still works.

| Character | Fire combo | Thunder combo |
|---|---|---|
| Ryu | **Shinku Inferno**: jump HK, cr.MK › Hadoken, cr.LK, cr.MP › Tatsumaki, Hadoken, st.HP, Shoryuken | **Thunder Storm**: cr.LK ×2, cr.MP › Hadoken, st.MK, Tatsumaki, jump HP, cr.HP › Shoryuken |
| Ken | **Dragon Inferno**: same route as Ryu | **Thunder Dragon**: same route as Ryu |
| E. Honda | **Sumo Inferno**: Headbutt, cr.MK, Hundred Hand Slap, st.HP, cr.LK, Hundred Hand Slap, Headbutt, jump HK | **Thunder Palms**: cr.LK, cr.MP, Hundred Hand Slap, st.MK, Headbutt, jump HP, cr.MK, Hundred Hand Slap |
| Blanka | **Jungle Blaze**: Rolling Attack, cr.MK, Electric Thunder, st.HP, Rolling Attack, jump HK, cr.LP, Electric Thunder | **Electric Storm**: cr.LK, Electric, cr.MP, Electric, Rolling Attack, st.HK, cr.MK, Electric |
| Guile | **Sonic Inferno**: Sonic Boom, Flash Kick, jump HK, cr.MK, Sonic Boom, cr.LP, st.HP, Flash Kick | **Flash Thunder**: cr.LK, cr.MP, Sonic Boom, Flash Kick, st.MK, cr.HK, Sonic Boom, Flash Kick |
| Chun-Li | **Phoenix Legs**: Kikoken, cr.MK, Lightning Kick, st.HP, cr.LK, Lightning Kick, Kikoken, jump HK | **Thunder Kicks**: cr.LK, Lightning Kick, cr.MP, Kikoken, st.MK, Lightning Kick, cr.HK, Kikoken |
| Zangief | **Red Cyclone**: st.HP, cr.MK, Clothesline, cr.LK, st.MP, Clothesline, Piledriver, jump HK | **Siberian Thunder**: cr.LK ×2, cr.MP, Clothesline, st.HK, Piledriver, cr.HK, Clothesline |
| Dhalsim | **Yoga Inferno**: Yoga Fire, Yoga Flame, Teleport, cr.MK, Yoga Fire, st.HP, Yoga Flame, slide | **Yoga Thunder**: cr.LK, Yoga Fire, st.MK, Yoga Flame, Teleport, Yoga Fire, slide, Yoga Fire |
| Balrog | **Buffalo Blaze**: Dash Punch, cr.MP, Dash Uppercut, st.HP, cr.LP, Dash Punch, jump HP, Dash Uppercut | **Buffalo Thunder**: cr.LP ×2, cr.MP, Dash Punch, st.HP, Dash Uppercut, cr.HP, Dash Punch |
| Vega | **Scarlet Flame**: Claw Roll, cr.MK, Claw Dive, st.HP, cr.LK, Claw Roll, jump HK, cr.HK | **Claw Thunder**: cr.LK, cr.MP, Claw Roll, st.MK, Claw Dive, jump HK, cr.HK, Claw Roll |
| Sagat | **Tiger Blaze**: Tiger Shot ×2, jump HK, cr.MK › Tiger Shot, st.HP, cr.LK, Tiger Uppercut | **Tiger Thunder**: cr.LK ×2, cr.MP › Tiger Shot, st.MK, cr.HK, Tiger Shot, st.HP, Tiger Uppercut |
| M. Bison | **Psycho Inferno**: Psycho Crusher, cr.MK, Scissor Kick, st.HP, cr.LK, Psycho Crusher, jump HK, Scissor Kick | **Psycho Thunder**: cr.LK, cr.MP, Scissor Kick, st.MK, Psycho Crusher, cr.HK, Scissor Kick, Psycho Crusher |

`›` means the special move is input during the previous attack (a cancel).

**How the timing was found.** Every gap between moves (written `5:~N` in
`src/combo.c`) was tuned automatically. A headless tuner sets up a V.S. BATTLE
fight for each character against a standing opponent, then runs the combo
hundreds of times from a save state, from two starting distances. It keeps the
gap values that land the most hits and damage. Against a standing opponent the
24 combos now deal **97–177 damage of 176**; several are full-health KOs.
A CPU or human who blocks will take less. To re-tune one combo and bake all
results back in:

```bash
SF2_COMBO_TEST=0 SF2_COMBO_TUNE=0 ./genexia --frames 1 "Street Fighter 2 Plus Champion Edition (J) [!].bin"
```

`SF2_COMBO_TEST` is the character's grid slot (0–11) and `SF2_COMBO_TUNE` picks
the combo (0 = Fire, 1 = Thunder). Save each run's output as
`<dir>/s<slot>w<combo>.txt`, then:

```bash
python3 tools/bake_combos.py <dir>
```

### SNES controls

On a gamepad, the buttons match their positions on a SNES pad: A / Cross = B,
B / Circle = A, X / Square = Y, Y / Triangle = X, RB = R, RT = L, Start = Start,
Back = Select. On the keyboard: Z = B, X = A, A = Y, S = X, D = R, C = L,
Return = Start, Right Shift = Select. In Super Mario Kart, B accelerates and X
toggles the map view.

Super Mario Kart's 1-player mode is split-screen too: you on top, the course map
or rear view below. Pick 1P GAME with the cursor on the title menu, then confirm
with Start or B.

### Nintendo 64

```bash
./Genexia.app/Contents/MacOS/genexia --rom "roms/n64/Super Mario 64 (USA).z64"
```

Super Mario 64 is recompiled with `make recompile-sm64` into `recomps/n64/sm64/`:
one C function per basic block of the game's code. Blocks keep the interpreter's
cycle counts and leave for the interpreter on anything unusual, so a recompiled run
matches an interpreted one exactly (`--interp` forces the interpreter). Before a
block is used, its code is compared with RDRAM, because the game loads overlays.

The R4300i, the RSP (running the game's real graphics and audio microcode) and a
software RDP are all emulated, so there is no per-game graphics plugin. The CIC-6102
boot is shortcut, so other games using that chip may boot, but only Super Mario 64
has been tested. Save data goes to emulated EEPROM. Widescreen is off for N64 and
run-ahead defaults to 0, since each state is 12 MB.

On a gamepad: left stick = analog stick (the D-pad does the same at full tilt),
A = A, B or X = B, RT = Z, RB = R, Back = L, right stick = C buttons, Y = C-Up,
Start = Start. On the keyboard: arrows = stick, Z = A, X or A = B, C = Z, D = R,
S = C-Up, Return = Start, Right Shift = L.

### Choosing a game

Run Genexia with no game named, or open `Genexia.app`, and a game selector appears.
It lists the built-in game plus every ROM in a `roms/` folder (and its subfolders)
in the current folder, next to the app, or in `~/Library/Application Support/Genexia/Genexia/roms`.
Each row shows the console and whether that ROM is recompiled or interpreted. Use
Up/Down and Enter, a gamepad (D-pad or stick, then A or Start), or the mouse. Esc
quits. `--rom` skips the selector.

### Options menu

Press **F10** in the game, or **R3** (right stick click) or **Start + Back**
together on a gamepad. The game pauses while the menu is open, and your choices
are remembered.

| Option | Choices |
|---|---|
| Widescreen | 16:9 or off (remembered per game) |
| Video filter | Sharp, Smooth, Scanlines, CRT (**F3** cycles them) |
| Scanline strength | Soft, Medium, Strong |
| Picture shape | Square pixels or TV 4:3, when widescreen is off |
| Sprite limit | Removed or Original (remembered per game) |
| Run-ahead | 0, 1 or 2 frames |
| Game speed | 50–200% |

On the keyboard use the arrow keys, Return and Esc; on a gamepad the D-pad, A
and B. `--filter sharp|smooth|scanlines|crt` picks the filter at start-up.

### Input lag (run-ahead) and save states

Run-ahead is on by default at 1 frame. After every real frame the whole machine
is snapshotted, the next 1 or 2 frames run with your current input, that future
picture is shown, and the snapshot is restored. The game reacts to your input
16–33 ms sooner and gameplay is unchanged. **F6** or `--runahead 0|1|2`
changes it, and the current value shows in the window title when it isn't 1.

Save states use the same machinery. Every variable of the emulated machine is
tagged `EMU_STATE`, which collects them in one memory section
(`__DATA,__emustate`, about 140 KB), so a snapshot is a single copy. **F5**
saves to `<rom>.state` and **F7** loads it.

### CRT effect

Press **F3**, or start with `--crt`. Three GPU passes go over the sharp picture:
* horizontal beam softness
* scanlines made at the window's real pixel resolution, so every game line is a
  bright beam with a dark gap at any window size, plus a faint RGB
  aperture-grille mask
* a soft glow that brings back the brightness the mask takes away

In small windows the scanlines fade out instead of aliasing.

### Game speed (50% – 200%)

| Control | Effect |
|---|---|
| **−** / **=** (or keypad − / +) | Speed down / up by 10% |
| **0** | Back to 100% |
| Mouse near the bottom of the window | Shows the speed slider; click or drag it (steps of 5%) |
| `--speed 150` | Start at that speed |

The current speed also shows in the window title. Speed scales the game's own
clock, so audio pitch follows it, like a tape running faster or slower.

Frame pacing is locked to the display's vsync:
* At 100% on a 60 Hz or 120 Hz screen, every refresh gets exactly 1 frame (or 1 every 2 refreshes), so scrolling is perfectly even.
* The 0.1% gap between the console's 59.92 Hz and the display is absorbed by tiny audio-rate adjustments.
* Speeds that don't divide evenly into the refresh rate, like 120%, alternate between 1 and 2 frames per refresh.

### Widescreen 16:9

On by default (`--no-wide` or **F2** for the original picture). The VDP renderer
draws one third more width than the game asks for: 256 → 342 px in fights,
320 → 426 px in menus. That picture is shown at TV-correct 16:9.

Fight stages are kept whole in video memory (512 px wide), so the extra columns
show real stage graphics and sprites. At the start of every frame the renderer
checks how far stage graphics actually extend on each side of the game's camera:
* At a stage edge the extra width shifts to the other side, like a camera stopping at a wall.
* If there are no stage graphics to widen into (logos, menus), it adds black bars.
* The HUD stays where it was, and the stage continues behind it in the margins.

Gameplay is unchanged: fighters are still held to the original 256-px view,
so corners work exactly as in the original.

### Command-line options

`--runahead N` sets frames of run-ahead (0–2) and `--crt` starts with the CRT effect.
`--sprite-limit` restores the hardware's per-line sprite limits.
`--interp` uses the interpreter only, as a reference. `--no-wide` gives the
original 4:3 picture, and `--speed N` sets the starting speed (50–200). `--scale N` sets the window size.
`--region 0xA0` makes the console report as a US one. Headless testing options:
`--frames N --shot out.ppm` renders N frames with no window. `--wav out.wav` records
the audio. `--monkey SEED` plays automatically with random input.

## Layout

| Path | What |
|---|---|
| `tools/m68kgen.py` | 68000 decoder and C code generator: one description of every instruction, used for both the interpreter and the recompiler |
| `tools/recompile.py` | Static recompiler: finds code by following control flow from the vectors and jump tables, plus coverage entries, then writes `generated/` and `docs/sf2.lst` |
| `tools/gen_interp.py` | Generates `src/m68k_interp.c` (1,391 deduplicated opcode handlers) |
| `recomps/<console>/<game>/` | **Every recompiled game**: generated C (`recomp_*.c`, never committed) plus `coverage.txt` (addresses only). A game without a folder here runs on the interpreter |
| `recomps/*/*/coverage.txt` | Extra entry points (mostly indirect jump targets) logged by the runtime, per game |
| `src/m68k.h`, `src/m68k_core.c` | CPU state, instruction meaning shared by both paths, exceptions, block dispatcher |
| `src/vdp.c` | VDP: planes, window, sprites, line/column scroll, shadow/highlight, DMA, interrupts, widescreen layout |
| `src/ym2612.c`, `src/psg.c`, `src/z80.c` | Sound hardware |
| `src/bus.c` | Memory map, I/O ports, 6-button pad protocol, Z80 bus arbitration |
| `src/main.c` | Scanline scheduler, audio mixing/resampling, run-ahead, SDL video/input, CRT effect, `.app` host |
| `src/state.c` | Whole-machine save states (snapshot of the `__emustate` section) |
| `src/combo.c` | Per-character combo scripts and the input engine that plays them |
| `src/fx.c` | Fire and thunder auras drawn from the VDP's per-pixel fighter mask |
| `docs/ANALYSIS.md` | Reverse-engineering notes on the ROM |
| `docs/sf2.lst` | Annotated disassembly of all 29,715 recovered instructions |

## Regenerating the recompiled code

```bash
make coverage
```

```bash
make recompile && make
```

`make coverage` plays the game headless and logs any address that had to fall
back to the interpreter. `make recompile && make` then translates those addresses
too. When you run the game normally, set `SF2_MISSLOG=recomps/genesis/sf2/coverage.txt` to do
the same logging.
