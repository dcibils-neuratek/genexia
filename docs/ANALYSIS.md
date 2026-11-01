# ROM analysis — Street Fighter II' Special Champion Edition (Mega Drive, Japan)

## Cartridge header ($100–$1FF)

| Field | Value |
|---|---|
| System | `SEGA MEGA DRIVE` |
| Copyright | `(C)T-12 1993.SEP` (T-12 = Capcom's licensee code) |
| Domestic title | `STREET FIGHTER II' PLUS CHAMPION EDITION` (the `II'` is in Shift-JIS) |
| Overseas title | `STREET FIGHTER II'SPECIAL CHAMPION EDITION` |
| Serial | `GM T-12033,0.0` |
| Checksum | `$FB5E`: verified as the sum of all words from $200 to the end |
| I/O support | `J6` (joypad, 6-button pad) |
| ROM range | `$000000–$2FFFFF` (3 MB, no mapper needed, under the 4 MB window) |
| RAM range | `$FF0000–$FFFFFF` |
| Region | `J` |

The initial SSP is `$FFFC00` and the reset PC is `$008000`. Everything below `$8000`
other than the vector table and header is a mix of data and a few helper routines
($0232–$0EFF).

## Exception vectors

| Vector | Handler | Role |
|---|---|---|
| Reset | `$8000` | Standard Sega boot: TMSS `SEGA` write, VDP register init, Z80 and PSG init, RAM clear |
| Level 4 (H-int) | `$8304` | Raster effects. It dispatches through a jump table indexed by `($FFA410).w`, one handler per scene type (stage floors, etc.) |
| Level 6 (V-int) | `$833C` | Per-frame work (below) |
| Level 2 (ext) | `$8300` | `nop; rte` |
| TRAP #0–#5 | `$828C`–`$82F4` | **Task-kernel system calls** (below) |
| TRAP #15 | `$84A4` | Fatal error: sets the flag at `$FFA424` |
| Bus/address/illegal/zero-divide | `$84B0`–`$84F8` | Crash handlers: save all registers and record the error number |

## Program structure: a cooperative multitasking kernel

Capcom built the game on a small task scheduler. The game logic runs in 68000
**user mode**, and tasks switch stacks through TRAP calls.

* There are 8 task control blocks of 16 bytes each at `$FFBFEC`:
  `+0` state, `+1` sleep counter, `+2` saved SR, `+4` saved PC / entry, `+8` saved USP.
* States: `0` free, `1` sleeping (counted down once per frame), `4` ready to resume,
  `8` newly created (start at the entry address). Bit 1 marks the task as suspended.
* The **scheduler loop** at `$8200`:
  1. Clears the VBlank flag `$FFA3FD` and spins until the V-int handler sets it.
  2. Bumps the frame counter `$FFA3FE`.
  3. Walks the 8 slots and resumes every runnable task, then calls `$8B16`.
* System calls (task number in `d0`):
  * `trap #0`: create a task with entry point `a0`.
  * `trap #1`: end the task's turn and go back to the scheduler.
  * `trap #2`: kill a task.
  * `trap #3`: sleep for `d0` frames, saving PC, SR and USP into the block.
  * `trap #4` / `trap #5`: suspend / resume a task.

This design is why the recompiler cannot map 68000 subroutines onto C calls.
Tasks swap stacks (`move usp`, `movea.w ($FFBFEA),a7`) and jump back into the
scheduler from anywhere. So each recompiled basic block returns to a dispatcher,
and the 68000 stack stays in emulated RAM.

### V-int handler ($833C)
1. Saves all registers and requests the Z80 bus.
2. Dispatches on the scene type `($FFA410)` through a jump table, then runs the
   per-frame DMA routines. Sprite table, palettes, scroll tables and tile uploads
   are queued in RAM and sent with 68k→VDP DMA.
3. Sends queued sound commands to the Z80 mailbox at `$A01FFE/$A01FFF`.
4. Sets the VBlank flag `$FFA3FD` so the scheduler runs the next frame.

## Sound

* At `$8FC4` the 68000 loads the Z80 program:
  * `$166` bytes from ROM `$2AC18` to Z80 `$0000`
  * a 1 KB table from ROM `$2CF000` to Z80 `$1000`
* The Z80 program is tiny. It is mainly a **PCM sample player**: it banks ROM
  through `$6000` and streams 8-bit samples to the YM2612 DAC (register `$2A`).
  These are the voices and hit sounds.
* The **68000 drives the FM music directly** through `$A04000` (code at `$903C` and
  `$91D8`). It holds the Z80 bus while it writes and passes PCM requests through
  the mailbox at `$A01FFE`.

## Input
The game uses the 3-button and 6-button pad protocols (header `J6`). It reads
port 1/2 data at `$A10003/5`, toggling TH to get the extra X/Y/Z/Mode buttons.

## Code size and coverage
* 29,715 68000 instructions (103 KB) recovered, all between `$0232` and `$2ABBE`.
  The rest of the 3 MB is graphics, animation, collision boxes, sound data and
  PCM samples.
* 635 subroutines (`jsr`/`bsr` targets) and 255 PC-relative jump tables
  (`move.w (d8,pc,Dn.w),Dn` / `jmp (d8,pc,Dn.w)`, the Capcom state-machine idiom).
  Recursive descent from the vectors finds only 1,652 instructions. Almost all code
  is reached indirectly through task entry points and state tables. The rest came
  from addresses that the runtime logged as interpreter fallbacks while the game
  played itself. That coverage is saved in `coverage/entries.txt`.

## How the recompiler translates code

* `tools/m68kgen.py` describes every 68000 instruction once. For each one it
  generates C statements that call shared helpers in `src/m68k.h`
  (`f_add`, `f_sub`, `f_logic`, shifts, BCD, mul/div, SR handling).
* **Recompiler mode:** everything known at translation time becomes a constant.
  That covers extension words, absolute and PC-relative addresses, branch targets,
  register numbers, and unrolled MOVEM lists. Each basic block becomes
  `void B_xxxxxx(void)`, with the original disassembly as comments.
* **Interpreter mode:** the same generator, with operands read from the opcode at
  run time. 65,536 opcodes reduce to 1,391 handlers.
* Blocks end at any control transfer and at SR writes, so a newly unmasked
  interrupt is taken right away. The dispatcher checks VDP interrupts between
  blocks and adds up each instruction's 68000 cycle cost, which keeps H-int raster
  timing correct.
* The recompiled build matches the interpreter frame for frame in attract mode.
  Headless, it runs at about 50× real time on an M4.

## Fighter data (found for combos and effects)

These were found by logging RAM next to the fighters' sprite positions over
about 18,000 sampled fight frames, then checked against the on-screen names.

| Address (P1 / P2) | Meaning |
|---|---|
| `$FF8000` / `$FF8280` | Player record base (`$280` bytes each) |
| `+$006` (word) | World X position. Its order matches the sprites on screen in 100% of samples, so facing = sign of the opponent's X minus your own |
| `+$042` (word) | Health, 176 = full, only goes down during a round |
| `+$1DB` (byte) | Character, in the arcade's internal order: 0 Ryu, 1 E.Honda, 2 Blanka, 3 Guile, 4 Ken, 5 Chun-Li, 6 Zangief, 7 Dhalsim, 8 Dictator, 9 Sagat, 10 Boxer, 11 Claw |
| Sprite palette 2 / 3 | Every P1 fighter sprite uses palette line 2 and every P2 sprite line 3; palette 0 is the HUD and screen text, palette 1 shadows and effects |

V.S. BATTLE select grid: top row Ryu, E.Honda, Blanka, Guile, Boxer, Claw;
bottom row Ken, Chun-Li, Zangief, Dhalsim, Sagat, Dictator.
