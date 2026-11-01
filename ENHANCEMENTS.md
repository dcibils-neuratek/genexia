# Genexia enhancement tracker

Status: ✅ done · 🚧 in progress · ⬜ planned

## Done so far

| | Enhancement | Notes |
|---|---|---|
| ✅ | Static recompilation to native arm64 | 29,715 instructions → C, interpreter fallback never needed in coverage runs |
| ✅ | Self-contained app | ROM built into the executable (`.incbin`), SDL2 and SDL3 bundled; `Genexia.app` runs with nothing else installed |
| ✅ | Other Genesis ROMs | `--rom <file>`; unknown games run on the interpreter (SF2 features stay SF2-only), region read from the header. **Ultimate Mortal Kombat 3 (USA) is recompiled too**: 47,113 instructions, 17,821 blocks, pixel-identical to the interpreter in attract mode up to frame 3,400, sound and save states work, and widescreen is on by default with its sprite HUD pinned to the centre with no side bars in fights: layers narrower than the wide view repeat their own art in the margins (small seams possible); menus stay centred |
| ✅ | Widescreen 16:9 | Real stage graphics in the extra width; follows stage edges, camera rise on jumps, mask bands handled; HUD, names, timer and round text pinned to the centre |
| ✅ | Game speed 50–200% | `-`/`=`/`0` keys, on-screen slider, `--speed` |
| ✅ | Even frame pacing | Locked to display vsync, audio rate control absorbs the 59.92 vs 60 Hz difference |
| ✅ | Exact frame timing | Absolute 488-cycle line timeline, fixed audio produced 1.3% too fast |
| ✅ | Video filters | Sharp, Smooth, Scanlines, CRT (scanlines, aperture mask, beam softness, glow), three scanline strengths |
| ✅ | Options menu | F10, or R3 / Start+Back on a gamepad: widescreen, filter, picture shape, sprite limit, run-ahead, speed; saved between runs |
| ✅ | Renamed to Genexia | A Genesis recompilation platform: `Genexia.app`, `genexia` |

## Smoothness and speed

| | # | Enhancement | Notes |
|---|---|---|---|
| ✅ | 1 | Remove sprite flicker | Per-line limits lifted (F4 toggles). **Finding:** fights never exceed the hardware's 16-sprite / 256-px line budget and the game never alternates sprites itself. The attract-mode intro does overflow on 89 scanlines, and those sprites no longer drop out |
| ✅ | 2 | Lower input lag (run-ahead) | 1 frame by default (F6 / `--runahead 0-2`), cuts 16–33 ms. Built on whole-machine save states (`EMU_STATE` section, ~140 KB, one memcpy); 900-frame mid-fight replays after a load are bit-identical, and the run-ahead frame equals the true future frame bit for bit. F5/F7 save/load slot |
| ✅ | 3 | Faster emulation | Profiling showed the recompiled 68000 code is only ~6% of run time (video ~50%, FM ~25%, Z80 ~13%), so the work went where the time is: frames nobody sees aren't drawn, discarded run-ahead audio only advances the chip timers, silent FM channels skip synthesis, the IRQ check before each code block became a plain variable read, Z80 runs in 64-cycle steps, and frame waits sleep instead of spinning. Run-ahead 2 went from 23.8 s to 16.3 s per 20k frames; live play uses ~18–24% of one core at any setting |

## Picture

| | # | Enhancement | Notes |
|---|---|---|---|
| ⬜ | 4 | Metal shader pipeline | Curvature, bloom, phosphor/slot masks, presets (consumer TV, PVM, arcade) |
| ⬜ | 5 | Sharp-bilinear scaling | Even pixel rows at non-integer window sizes |
| ⬜ | 6 | Pixel-art upscalers | xBRZ / ScaleFX style edge smoothing |
| ⬜ | 7 | Full widescreen on every stage | Extend background art where stages run out at the edges |

## Sound

| | # | Enhancement | Notes |
|---|---|---|---|
| ⬜ | 8 | Higher-accuracy sound | Cycle-accurate YM2612 (operator delays, DAC ladder), band-limited resampling with low-pass filter |
| ⬜ | 9 | Pitch-preserving speed | Time-stretch audio so 150% doesn't raise the pitch |
| ⬜ | 10 | Arcade / remastered music | Replace FM tracks with higher-fidelity versions |

## Gameplay features

| | # | Enhancement | Notes |
|---|---|---|---|
| 🚧 | 11 | Save states and rewind | Save states ✅ (F5/F7). Rewind still to do: a ring buffer of states |
| ⬜ | 12 | Training mode | Hitbox / frame-data overlay from the ROM's tables, input display, infinite health/timer |
| ⬜ | 13 | Online play with rollback | Builds on #2/#11 |
| ⬜ | 14 | Remapping menu, rumble | In-game button configuration, controller rumble on hits |
| ✅ | 15 | Killer combos with special effects | Two fixed 8–9 move combos per character (Q/W for P1, U/O for P2, LB/LT on a controller), mixing punches, kicks, jumps and only this version's special-move inputs. Every gap auto-tuned against a standing opponent from two distances: 97–177 damage of 176, several full-health KOs. Fire aura (rising flames, rim light, embers) or thunder aura (outline glow, lightning bolts), plus the combo name and a live hit counter. **Mana meter**: combos need a full bar, which fills over 10 s of fighting (`--mana N`, 0 = off), restarts empty each round, pauses during a combo and shows READY + key hints when full |
