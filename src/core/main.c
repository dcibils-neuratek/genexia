/*
 * main.c - Genexia, a macOS host for recompiled Mega Drive / Genesis games.
 * Owns the frame/scanline scheduler, audio mixing/resampling, SDL video
 * output and keyboard/gamepad input.
 *
 * usage: genexia [--rom file] [--interp] [--scale N] [--frames N --shot out.ppm]
 *            [--wav out.wav] [--press frame:buttons ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <time.h>
#include <SDL.h>
#include "m68k.h"
#include "bus.h"
#include "state.h"
#include "combo.h"
#include "backend.h"
#include "snes.h"
#include "machine.h"
#include "n64_machine.h"
#include "version.h"
#include "enhance.h"

extern u64 m68k_blocks_run, m68k_interp_run;

/* ------------------------------------------------------------ timing */
EMU_STATE static s64 line_start_cyc;
EMU_STATE static s64 z80_now68;              /* 68k-cycle timestamp of the running Z80 slice */
int sys_line_cycle(void) {
    s64 d = cpu.cyc - line_start_cyc;
    if (d < 0) d = 0;
    if (d >= M68K_CYCLES_PER_LINE) d = M68K_CYCLES_PER_LINE - 1;
    return (int)d;
}

/* ------------------------------------------------------------- audio */
#define AUD_MAX 4096
static s16 ym_buf[AUD_MAX * 2];
static const Backend *be;               /* active console */
static s16 psg_buf[AUD_MAX];
EMU_STATE static int aud_n;                   /* native samples generated this frame */
EMU_STATE static s64 aud_cyc;                 /* 68k cycle up to which audio is generated */

static int audio_discard;          /* run-ahead frames: advance chip timers only */
static void audio_render_to(s64 t) {
    s64 n = (t - aud_cyc) / 144;
    if (n <= 0) return;
    if (audio_discard) { ym2612_advance((int)n); aud_cyc += n * 144; return; }
    if (aud_n + n > AUD_MAX) n = AUD_MAX - aud_n;
    if (n > 0) {
        ym2612_render(ym_buf + aud_n * 2, (int)n);
        psg_render(psg_buf + aud_n, (int)n);
        aud_n += (int)n;
    }
    aud_cyc += ((t - aud_cyc) / 144) * 144;
}
void audio_sync_68k(void) { audio_render_to(cpu.cyc); }
void audio_sync_z80(void) { audio_render_to(z80_now68); }

/* game speed in percent (50..200). Audio follows the game clock, so it is
 * pitched up/down with the speed, like a tape running faster or slower. */
static int speed_pct = 100;
/* run-ahead: after each real frame, save the machine, run this many extra
 * frames with the current input, show the last one, then restore. The game
 * reacts to input 1-2 frames sooner (16-33 ms) without changing gameplay. */
static int runahead = 1;
static u8 *ra_buf;
static void run_ahead_frames(void);
static double audio_rate_adjust = 1.0;
static long todo_hist[9];
static double produced_samples;   /* dynamic rate control, ~1 +- 0.5% */
static void set_speed(int pct) {
    pct = (pct + 2) / 5 * 5;
    speed_pct = pct < 50 ? 50 : pct > 200 ? 200 : pct;
}

/* resample native 53267 Hz -> host rate, mixing FM + PSG */
#define HOST_RATE 48000
static s16 host_buf[AUD_MAX * 2];
EMU_STATE static double rs_pos;
EMU_STATE static s16 rs_last[2];
static int resample(void) {
    int out = 0;
    double step = (double)be->sample_rate / HOST_RATE * speed_pct / 100.0 * audio_rate_adjust;
    while (rs_pos < aud_n && out < AUD_MAX) {
        int i = (int)rs_pos;
        double fr = rs_pos - i;
        for (int ch = 0; ch < 2; ch++) {
            int a = (i == 0 && fr < 0) ? rs_last[ch] : ym_buf[i * 2 + ch] + psg_buf[i];
            int b = (i + 1 < aud_n) ? ym_buf[(i + 1) * 2 + ch] + psg_buf[i + 1] : a;
            int v = (int)((a + (b - a) * fr) * be->gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            host_buf[out * 2 + ch] = (s16)v;
        }
        out++;
        rs_pos += step;
    }
    rs_pos -= aud_n;
    if (aud_n) { rs_last[0] = ym_buf[(aud_n - 1) * 2]; rs_last[1] = ym_buf[(aud_n - 1) * 2 + 1]; }
    aud_n = 0;
    return out;
}

/* ------------------------------------------------------------- frame */
#define SLICES 4

static void run_frame_snes(void) {
    snes_run_frame();
    int room = AUD_MAX - aud_n;
    int n = snes_apu_take(ym_buf + aud_n * 2, room);
    if (n > 0) {
        memset(psg_buf + aud_n, 0, (size_t)n * sizeof(s16));
        if (!audio_discard) aud_n += n; 
    }
}

static void run_frame_n64(void) {
    n64_run_frame();
    int room = AUD_MAX - aud_n;
    int n = n64_audio_take(ym_buf + aud_n * 2, room);
    if (n > 0) {
        memset(psg_buf + aud_n, 0, (size_t)n * sizeof(s16));
        if (!audio_discard) aud_n += n;
    }
}

static void run_frame_genesis(void) {
    EMU_STATE static s64 z80_debt;
    EMU_STATE static s64 sched = -1;      /* absolute 68k-cycle timeline: exactly 488 per line */
    if (sched < 0 || cpu.cyc - sched > 4 * M68K_CYCLES_PER_LINE) sched = cpu.cyc;
    for (int line = 0; line < LINES_PER_FRAME; line++) {
        vdp_begin_line(line);
        z80_set_irq(vdp_zint_pending());
        /* a block may overshoot the previous line's end; that overshoot is
         * simply paid back on this line instead of stretching the frame */
        line_start_cyc = sched;
        s64 line_end = line_start_cyc + M68K_CYCLES_PER_LINE;
        for (int s = 1; s <= SLICES; s++) {
            s64 slice_end = line_start_cyc + M68K_CYCLES_PER_LINE * s / SLICES;
            s64 z80_from = cpu.cyc < slice_end ? cpu.cyc : slice_end - 1;
            m68k_run(slice_end);
            /* Z80 runs in small steps over the same time window */
            z80_debt += (s64)Z80_CYCLES_PER_LINE / SLICES;
            if (s == SLICES) z80_debt += Z80_CYCLES_PER_LINE % SLICES;
            if (!z80_busreq && !z80_reset_line) {
                s64 total = z80_debt;
                s64 done = 0;
                while (z80_debt > 0) {
                    z80_now68 = z80_from + (slice_end - z80_from) * done / (total ? total : 1);
                    int ran = z80_execute(z80_debt < 64 ? (int)z80_debt : 64);
                    z80_debt -= ran;
                    done += ran;
                }
            } else {
                z80_debt = 0;
            }
            z80_now68 = slice_end;
        }
        (void)line_end;
        sched += M68K_CYCLES_PER_LINE;
        audio_render_to(sched);
    }
}

/* random play for code coverage / tests: hold random inputs for a few frames */
static struct { u16 held[2]; int hold; } mk;
static void monkey_input(unsigned *rng, int f) {
    if (--mk.hold <= 0) {
        *rng = *rng * 1103515245u + 12345u;
        mk.held[0] = (u16)(*rng >> 8) & 0x7FF & ~PAD_START;
        *rng = *rng * 1103515245u + 12345u;
        mk.held[1] = (u16)(*rng >> 8) & 0x7FF & ~PAD_START;
        mk.hold = 2 + (int)((*rng >> 24) & 15);
    }
    int ph = f % 9000;
    if (ph < 1500) {
        /* menu phase: alternate Start / punch pulses to get into a fight */
        u16 pulse = ((f / 40) & 1) ? PAD_START : PAD_X;
        pad_state[0] = (f % 40) < 4 ? pulse : 0;
        if ((f % 40) >= 20 && (f % 40) < 24) pad_state[0] = (u16)(1u << ((mk.held[0] >> 4) & 3));
        pad_state[1] = (*rng & 0x10000) && (f % 40) < 4 ? pulse : 0;
    } else {
        pad_state[0] = mk.held[0];
        pad_state[1] = mk.held[1];
    }
}

static Backend backend_n64;
static void run_frame(void) {
    be->run_frame();
    if (be == &backend_n64) backend_n64.sample_rate = n64_audio_rate;   /* set by the game */
}
static void power_on(void);
static void save_ppm(const char *path);

/* run one combo from a saved state against the idle P2; returns damage and hits */
static void combo_eval(int which, const u8 *st, int pre_walk, int *damage, int *hits) {
    state_load(st);
    for (int f = 0; f < pre_walk; f++) {          /* optional: start closer */
        pad_state[0] = combo_player_x(1) > combo_player_x(0) ? PAD_RIGHT : PAD_LEFT; pad_state[1] = 0; run_frame();
    }
    pad_state[0] = pad_state[1] = 0;
    int hp0 = combo_player_hp(1), last = hp0;
    *hits = 0;
    combo_start(0, which);
    for (int f = 0; f < 1200 && (combo_running(0) || combo_fx_frames[0] > 0); f++) {
        pad_state[0] = combo_input(0, 0); pad_state[1] = 0;
        run_frame();
        int hp = combo_player_hp(1);
        if (hp < last) (*hits)++;
        last = hp;
    }
    combo_cancel(0); combo_fx_frames[0] = 0;
    *damage = hp0 - last;
}

/* coordinate descent over the combo's tunable gaps, scored from two
 * starting distances; prints the tuned script to bake into combo.c */
static void combo_tune(int slot, int ch, int which, const u8 *st) {
    int idx[64], n = combo_tunables(ch, which, idx, 64);
    #define SCORE(out) do { int d1, h1, d2, h2; combo_eval(which, st, 0, &d1, &h1); combo_eval(which, st, 40, &d2, &h2); \
                            out = d1 + d2 + 10 * (h1 + h2); } while (0)
    int best; SCORE(best);
    int start = best;
    for (int pass = 0; pass < 3; pass++) {
        for (int k = 0; k < n; k++) {
            int cur = combo_step_frames(ch, which, idx[k]), bestv = cur;
            int lo = pass == 0 ? 0 : cur - 6, hi = pass == 0 ? 64 : cur + 6, stp = pass == 0 ? 4 : 1;
            for (int v = lo < 0 ? 0 : lo; v <= hi; v += stp) {
                if (v == cur) continue;
                combo_set_step_frames(ch, which, idx[k], v);
                int sc; SCORE(sc);
                if (sc > best) { best = sc; bestv = v; }
            }
            combo_set_step_frames(ch, which, idx[k], bestv);
        }
    }
    #undef SCORE
    int d, h; combo_eval(which, st, 0, &d, &h);
    printf("TUNED slot %d char %d which %d score %d -> %d (hits %d damage %d)\n", slot, ch, which, start, best, h, d);
    combo_print_tuned(ch, which, stdout);
    fflush(stdout);
}

/* Headless combo test: V.S. BATTLE, P1 picks the character at each grid
 * slot, P2 (idle human) keeps the default; both combos run from the same
 * save state and we report damage dealt and hits landed. */
static int combo_test(const char *arg) {
    combo_mana_enabled = 0;                         /* the bench measures combos, not the meter */
    static const char *names[12] = { "Ryu", "E.Honda", "Blanka", "Guile", "Ken", "Chun-Li",
                                     "Zangief", "Dhalsim", "M.Bison", "Sagat", "Balrog", "Vega" };
    const char *shots = getenv("SF2_COMBO_SHOTS");
    for (int slot = 0; slot < 12; slot++) {
        if (strcmp(arg, "all") && atoi(arg) != slot) continue;
        int row = slot / 6, col = slot % 6;
        power_on();
        int fight_at = -1;
        u8 *st = malloc(state_size());
        for (int f = 0; f < 4000; f++) {
            u16 b0 = 0, b1 = 0;
            #define AT(fr) (f >= (fr) && f < (fr) + 6)
            if (AT(1000) || AT(1120) || AT(1310)) b0 |= PAD_START;
            if (AT(1280)) b0 |= PAD_DOWN;
            for (int c = 0; c < col; c++) if (AT(1460 + c * 14)) b0 |= PAD_RIGHT;
            if (row && AT(1460 + col * 14)) b0 |= PAD_DOWN;
            for (int r = 0; r < 4; r++) { if (AT(1560 + r * 60)) b0 |= PAD_X; if (AT(1580 + r * 60)) b1 |= PAD_X; }
            for (int r = 0; r < 3; r++) if (AT(1860 + r * 100)) { b0 |= PAD_START; b1 |= PAD_START; }
            #undef AT
            pad_state[0] = b0; pad_state[1] = b1;
            run_frame();
            if (fight_at < 0 && vdp_screen_w == 342 && combo_player_hp(0) == 176 && combo_player_hp(1) == 176 && combo_player_x(0) > 0)
                fight_at = f;
            if (fight_at >= 0 && f == fight_at + 170) { state_save(st); break; }
        }
        if (fight_at < 0) { printf("slot %2d: no fight reached\n", slot); continue; }
        int ch = combo_player_char(0);
        if (getenv("SF2_COMBO_TUNE")) { combo_tune(slot, ch, atoi(getenv("SF2_COMBO_TUNE")), st); free(st); continue; }
        if (getenv("SF2_MANA_DEMO")) {               /* meter flow: fill, fire, denied press */
            combo_mana_enabled = 1; combo_mana[0] = combo_mana[1] = 0;
            if (getenv("SF2_PAD_HINT")) combo_pad_hint[0] = combo_pad_hint[1] = 1;
            for (int f = 0; f <= 1310; f++) {
                if (f == 720) combo_start(0, 0);
                if (f == 800 || f == 1300) combo_start(0, 1);
                pad_state[0] = combo_input(0, 0); pad_state[1] = combo_input(1, 0);
                run_frame();
                if (f == 60 || f == 300 || f == 640 || f == 740 || f == 802 || f == 1302) {
                    fx_apply(vdp_framebuffer, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h);
                    char p[512]; snprintf(p, sizeof p, "%s/mana_%04d.ppm", getenv("SF2_COMBO_DIR") ? getenv("SF2_COMBO_DIR") : ".", f);
                    save_ppm(p);
                    printf("frame %d mana P1 %.2f P2 %.2f running %d\n", f, combo_mana[0], combo_mana[1], combo_running(0));
                }
            }
            free(st); continue;
        }
        for (int which = 0; which < 2; which++) {
            state_load(st);
            pad_state[0] = pad_state[1] = 0;
            int hp0 = combo_player_hp(1), hits = 0, last = hp0, frames = 0;
            combo_start(0, which);
            for (int f = 0; f < 900 && (combo_running(0) || combo_fx_frames[0] > 0); f++, frames++) {
                pad_state[0] = combo_input(0, 0);
                pad_state[1] = 0;
                run_frame();
                int hp = combo_player_hp(1);
                if (hp < last) hits++;
                last = hp;
                if (shots && which == atoi(shots) && f % 6 == 0) {
                    fx_apply(vdp_framebuffer, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h);
                    char p[512]; snprintf(p, sizeof p, "%s/combo_%02d_%d_%04d.ppm", getenv("SF2_COMBO_DIR") ? getenv("SF2_COMBO_DIR") : ".", slot, which, f);
                    save_ppm(p);
                }
            }
            printf("slot %2d %-8s %-17s frames %4d  hits %2d  damage %3d of %d\n", slot, ch < 12 ? names[ch] : "?",
                   combo_name(0, which), frames, hits, hp0 - last, hp0);
        }
        free(st);
    }
    return 0;
}

static void run_ahead_frames(void) {
    if (runahead <= 0) return;
    if (!ra_buf) ra_buf = malloc(state_size());
    state_save(ra_buf);
    audio_discard = 1;                                /* their audio is thrown away anyway */
    for (int i = 0; i < runahead; i++) {
        vdp_skip_render = i < runahead - 1;           /* only the last one is shown */
        run_frame();
    }
    vdp_skip_render = 0;
    audio_discard = 0;
    state_load(ra_buf);                               /* framebuffer keeps the future frame */
}

static u64 ram_hash(void) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof ram; i++) { h ^= ram[i]; h *= 1099511628211ull; }
    for (int i = 0; i < 320 * 224; i++) { h ^= vdp_framebuffer[(i / 320) * VDP_FB_STRIDE + i % 320]; h *= 1099511628211ull; }
    return h;
}

/* ------------------------------------------------------------ output */
static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", vdp_screen_w, vdp_screen_h);
    for (int y = 0; y < vdp_screen_h; y++)
        for (int x = 0; x < vdp_screen_w; x++) {
            u32 p = vdp_framebuffer[y * VDP_FB_STRIDE + x];
            u8 rgb[3] = { (u8)(p >> 16), (u8)(p >> 8), (u8)p };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static FILE *wav_f;
static u32 wav_bytes;
static void wav_open(const char *path) {
    wav_f = fopen(path, "wb");
    if (!wav_f) return;
    u8 hdr[44] = {0};
    fwrite(hdr, 1, 44, wav_f);
}
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wav_close(void) {
    if (!wav_f) return;
    u8 h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + wav_bytes); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    put32(h + 24, HOST_RATE); put32(h + 28, HOST_RATE * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); put32(h + 40, wav_bytes);
    fseek(wav_f, 0, SEEK_SET); fwrite(h, 1, 44, wav_f); fclose(wav_f);
}

/* ------------------------------------------------------------- input */
typedef struct { int frame; u16 buttons; int player; } Press;
static Press presses[256];
static int npress;

static u16 parse_buttons(const char *s) {
    u16 b = 0;
    for (; *s; s++) switch (*s) {
        case 'S': b |= PAD_START; break; case 'A': b |= PAD_A; break;
        case 'B': b |= PAD_B; break;     case 'C': b |= PAD_C; break;
        case 'X': b |= PAD_X; break;     case 'Y': b |= PAD_Y; break;
        case 'Z': b |= PAD_Z; break;     case 'M': b |= PAD_MODE; break;
        case 'u': b |= PAD_UP; break;    case 'd': b |= PAD_DOWN; break;
        case 'l': b |= PAD_LEFT; break;  case 'r': b |= PAD_RIGHT; break;
    }
    return b;
}

static u16 keyboard_pad(const u8 *k, int player) {
    u16 b = 0;
    if (player == 0) {
        if (k[SDL_SCANCODE_UP]) b |= PAD_UP;
        if (k[SDL_SCANCODE_DOWN]) b |= PAD_DOWN;
        if (k[SDL_SCANCODE_LEFT]) b |= PAD_LEFT;
        if (k[SDL_SCANCODE_RIGHT]) b |= PAD_RIGHT;
        if (k[SDL_SCANCODE_A]) b |= PAD_X;      /* light punch  */
        if (k[SDL_SCANCODE_S]) b |= PAD_Y;      /* medium punch */
        if (k[SDL_SCANCODE_D]) b |= PAD_Z;      /* heavy punch  */
        if (k[SDL_SCANCODE_Z]) b |= PAD_A;      /* light kick   */
        if (k[SDL_SCANCODE_X]) b |= PAD_B;      /* medium kick  */
        if (k[SDL_SCANCODE_C]) b |= PAD_C;      /* heavy kick   */
        if (k[SDL_SCANCODE_RETURN]) b |= PAD_START;
        if (k[SDL_SCANCODE_RSHIFT]) b |= PAD_MODE;
    } else {
        if (k[SDL_SCANCODE_I]) b |= PAD_UP;
        if (k[SDL_SCANCODE_K]) b |= PAD_DOWN;
        if (k[SDL_SCANCODE_J]) b |= PAD_LEFT;
        if (k[SDL_SCANCODE_L]) b |= PAD_RIGHT;
        if (k[SDL_SCANCODE_R]) b |= PAD_X;
        if (k[SDL_SCANCODE_T]) b |= PAD_Y;
        if (k[SDL_SCANCODE_Y]) b |= PAD_Z;
        if (k[SDL_SCANCODE_F]) b |= PAD_A;
        if (k[SDL_SCANCODE_G]) b |= PAD_B;
        if (k[SDL_SCANCODE_H]) b |= PAD_C;
        if (k[SDL_SCANCODE_1]) b |= PAD_START;
    }
    return b;
}

static u16 controller_pad(SDL_GameController *gc) {
    if (!gc) return 0;
    u16 b = 0;
    const int dz = 12000;
    int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -dz) b |= PAD_UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > dz) b |= PAD_DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -dz) b |= PAD_LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > dz) b |= PAD_RIGHT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X)) b |= PAD_X;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y)) b |= PAD_Y;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= PAD_Z;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) b |= PAD_A;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) b |= PAD_B;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= PAD_C;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) b |= PAD_START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) b |= PAD_MODE;
    return b;
}

/* -------------------------------------------------------------- boot */
size_t sf2_embedded_rom(const unsigned char **p);
static int probing;      /* the game selector is looking at ROMs: stay quiet */

/* path == NULL: use the ROM built into the executable */
static int load_rom(const char *path) {
    if (!path) {
        const unsigned char *data;
        size_t n = sf2_embedded_rom(&data);
        if (n < 0x200 || n > sizeof rom) return 0;
        memcpy(rom, data, n);
        rom_size = (u32)n;
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) return 0;
        rom_size = (u32)fread(rom, 1, sizeof rom, f);
        fclose(f);
    }
    if (rom_size < 0x200) return 0;
    if (memcmp(rom + 0x100, "SEGA", 4)) return 1;   /* the checks below are Genesis-only */
    u32 sum = 0;
    for (u32 i = 0x200; i + 1 < rom_size; i += 2) sum += (u32)(rom[i] << 8 | rom[i + 1]);
    u16 want = (u16)(rom[0x18E] << 8 | rom[0x18F]);
    if (!probing) fprintf(stderr, "ROM%s: %.48s  %u bytes, checksum %04X (%s)\n", path ? "" : " (built in)", (char *)rom + 0x150,
            rom_size, sum & 0xFFFF, (sum & 0xFFFF) == want ? "ok" : "MISMATCH");
    return 1;
}

/* Which game is this? The generated code and the SF2 enhancements only
 * apply to that exact ROM; anything else runs on the interpreter. */
static char game_title[64];
static void reset_genesis(void);
static void reset_snes(void) { vdp_init(); snes_reset(); }
static const Backend backend_genesis = { "Genesis", YM_SAMPLE_RATE, 4.0, reset_genesis, run_frame_genesis };
static const Backend backend_snes = { "SNES", SNES_DSP_RATE, 1.8, reset_snes, run_frame_snes };
static void reset_n64(void) { vdp_init(); n64_reset(); }
static Backend backend_n64 = { "Nintendo 64", 32000, 1.0, reset_n64, run_frame_n64 };

static void configure_game(int region_set) {
    be = &backend_genesis;
    state_use_section("__emustate");
    state_on_load = NULL;
    if (n64_detect()) {
        be = &backend_n64;
        game_is_sf2 = 0; vdp_hud_rules = 0;
        m68k_register_game = NULL;
        state_use_section("__n64state");
        snprintf(game_title, sizeof game_title, "%s", n64_load());
        /* recompiled code for this exact ROM, if we have it */
        n64_register_game = !strcmp(game_title, "SUPER MARIO 64") && rom[0x10] == 0x63 && rom[0x11] == 0x5A
                            ? recomp_register_sm64 : NULL;
        n64_force_interp = m68k_force_interp || !n64_register_game;
        state_on_load = n64_state_loaded;
        if (!probing) fprintf(stderr, "Game: %s  (Nintendo 64, %s)\n", game_title, n64_force_interp ? "interpreter" : "recompiled");
        return;
    }
    if (memcmp(rom + 0x100, "SEGA", 4) && snes_detect()) {
        be = &backend_snes;
        game_is_sf2 = 0; vdp_hud_rules = 0;
        m68k_register_game = NULL;
        snprintf(game_title, sizeof game_title, "%s", snes_load());
        /* recompiled code for this exact ROM, if we have it */
        snes_register_game = !strcmp(game_title, "SUPER MARIO KART") && rom[0xFFDE] == 0x44 && rom[0xFFDF] == 0xEB
                             ? recomp_register_smk : NULL;
        snes_force_interp = m68k_force_interp || !snes_register_game;
        if (!probing) fprintf(stderr, "Game: %s  (SNES, %s)\n", game_title, snes_force_interp ? "interpreter" : "recompiled");
        return;
    }
    u16 hdr = (u16)(rom[0x18E] << 8 | rom[0x18F]);
    game_is_sf2 = hdr == 0xFB5E && !memcmp(rom + 0x180, "GM T-12033", 10);
    /* pick the recompiled code that belongs to this exact ROM, if any */
    int is_umk3 = hdr == 0xFC4C && !memcmp(rom + 0x180, "GM T-097146", 11);
    m68k_register_game = game_is_sf2 ? recomp_register : is_umk3 ? recomp_register_umk3 : NULL;
    if (!m68k_register_game) m68k_force_interp = 1;
    vdp_hud_rules = game_is_sf2 ? 1 : is_umk3 ? 2 : 0;
    /* title from the header: overseas name, else domestic */
    const char *src = (char *)rom + 0x150;
    int n = 48;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == 0)) n--;
    if (!n) { src = (char *)rom + 0x120; n = 48; while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == 0)) n--; }
    int j = 0;
    for (int i = 0; i < n && j < 63; i++) {         /* collapse padding, drop non-ASCII */
        char c = src[i];
        if (c < 32 || c > 126 || (c == ' ' && j && game_title[j - 1] == ' ')) continue;
        game_title[j++] = c;
    }
    game_title[j] = 0;
    if (game_is_sf2) snprintf(game_title, sizeof game_title, "Street Fighter II' Special Champion Edition");
    /* console region from the header: new-style hex digit or J/U/E letters */
    if (!region_set) {
        int jp = 0, us = 0, eu = 0;
        for (int i = 0x1F0; i < 0x1F3; i++) {
            char c = (char)rom[i];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
            if (c == 'J') jp = 1; else if (c == 'U') us = 1; else if (c == 'E') eu = 1;
            else { if (v & 1) jp = 1; if (v & 4) us = 1; if (v & 8) eu = 1; }
        }
        region_version = us ? 0xA0 : jp ? 0x20 : eu ? 0xE0 : 0xA0;
    }
    if (!probing) fprintf(stderr, "Game: %s  (%s, region %02X)\n", game_title,
            m68k_register_game && !m68k_force_interp ? "recompiled" : "interpreter", region_version);
}

static void power_on(void) { be->reset(); }
static void reset_genesis(void) {
    bus_reset();
    vdp_init();
    z80_reset();
    ym2612_reset();
    psg_reset();
    m68k_reset();
}

static const char *find_rom(const char *argv0) {
    static char buf[4096];
    const char *names[] = { "sf2.bin", "Street Fighter 2 Plus Champion Edition (J) [!].bin" };
    char *base = SDL_GetBasePath();
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
            if (pass == 0) snprintf(buf, sizeof buf, "%s", names[i]);
            else if (base) snprintf(buf, sizeof buf, "%s%s", base, names[i]);
            else continue;
            FILE *f = fopen(buf, "rb");
            if (f) { fclose(f); return buf; }
        }
    (void)argv0;
    return NULL;
}

/* ------------------------------------------------- speed slider (OSD) */
typedef struct { int x, y, w, h; } OsdRect;
static OsdRect osd_layout(int ww, int wh) {     /* in window coordinates */
    OsdRect r;
    r.w = ww / 2; r.h = wh / 60 < 6 ? 6 : wh / 60;
    r.x = (ww - r.w) / 2; r.y = wh - wh / 10;
    return r;
}
/* 3x5 pixel font for the label */
static const char *glyph(char c) {
    switch (c) {
    case '0': return "111101101101111"; case '1': return "010110010010111";
    case '2': return "111001111100111"; case '3': return "111001111001111";
    case '4': return "101101111001001"; case '5': return "111100111001111";
    case '6': return "111100111101111"; case '7': return "111001010010010";
    case '8': return "111101111101111"; case '9': return "111101111001111";
    case '%': return "101001010100101"; case 'S': return "111100111001111";
    case 'P': return "111101111100100"; case 'E': return "111100111100111";
    case 'D': return "110101101101110"; default:  return "000000000000000";
    }
}
static void draw_text(SDL_Renderer *ren, const char *t, int x, int y, int px) {
    for (; *t; t++, x += 4 * px) {
        const char *g = glyph(*t);
        for (int i = 0; i < 15; i++) if (g[i] == '1') {
            SDL_Rect q = { x + (i % 3) * px, y + (i / 3) * px, px, px };
            SDL_RenderFillRect(ren, &q);
        }
    }
}
static void draw_osd(SDL_Renderer *ren, SDL_Window *win) {
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    double k = (double)ow / ww;                 /* HiDPI scale */
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetViewport(ren, NULL);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    OsdRect r = osd_layout(ww, wh);
    int x = (int)(r.x * k), y = (int)(r.y * k), w = (int)(r.w * k), h = (int)(r.h * k);
    int px = (int)(k * (wh / 140 < 2 ? 2 : wh / 140));
    SDL_Rect bg = { x - 6 * px, y - 10 * px, w + 12 * px, h + 14 * px };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);
    SDL_RenderFillRect(ren, &bg);
    SDL_Rect bar = { x, y, w, h };
    SDL_SetRenderDrawColor(ren, 90, 90, 110, 255);
    SDL_RenderFillRect(ren, &bar);
    /* fill up to the current speed, tick at 100% */
    int pos = (int)(w * (speed_pct - 50) / 150.0);
    SDL_Rect fill = { x, y, pos, h };
    SDL_SetRenderDrawColor(ren, 255, 200, 40, 255);
    SDL_RenderFillRect(ren, &fill);
    SDL_Rect tick = { x + w / 3 - px / 2, y - 2 * px, px, h + 4 * px };
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
    SDL_RenderFillRect(ren, &tick);
    SDL_Rect knob = { x + pos - 2 * px, y - 2 * px, 4 * px, h + 4 * px };
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderFillRect(ren, &knob);
    char label[16];
    snprintf(label, sizeof label, "SPEED %d%%", speed_pct);
    draw_text(ren, label, x, y - 8 * px, px);
}

/* ------------------------------------------------------- CRT effect */
/* Drawn on the GPU in three passes over the sharp picture:
 *  1. a half-pixel-shifted copy at 50% blends neighbours horizontally (beam softness)
 *  2. a multiply overlay, generated at the real output resolution: each game
 *     line becomes a bright beam with a dark gap, plus a faint RGB stripe mask
 *  3. an additive low-alpha copy brings back the brightness the mask removed */
enum { FILTER_SHARP, FILTER_SMOOTH, FILTER_SCANLINES, FILTER_CRT, FILTER_COUNT };
static int video_filter = FILTER_SHARP;
static int scan_strength = 1;      /* 0 soft, 1 medium, 2 strong */
static int pic_shape;              /* outside widescreen: 0 square pixels, 1 TV 4:3 */
static SDL_Texture *crt_tex;
static int crt_w, crt_h, crt_lines, crt_key;

static void crt_build(SDL_Renderer *ren, int w, int h, int lines) {
    int key = video_filter * 8 + scan_strength;
    if (crt_tex && w == crt_w && h == crt_h && lines == crt_lines && key == crt_key) return;
    if (crt_tex) SDL_DestroyTexture(crt_tex);
    crt_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!crt_tex) return;
    crt_w = w; crt_h = h; crt_lines = lines; crt_key = key;
    u32 *px = malloc((size_t)w * h * 4);
    if (!px) return;
    double scale = (double)h / lines;                  /* output pixels per game line */
    static const double strength[3] = { 0.32, 0.55, 0.78 };
    double full = strength[scan_strength];
    double depth = scale < 2.0 ? 0.0 : scale < 4.0 ? full * (scale - 2.0) / 2.0 : full;
    double mask = scale < 3.0 || video_filter != FILTER_CRT ? 0.0 : 0.12;
    for (int y = 0; y < h; y++) {
        double gy = (y + 0.5) / scale, fr = gy - floor(gy);
        double beam = sin(M_PI * fr);                   /* 0 at line edges, 1 at centre */
        double v = 1.0 - depth * (1.0 - pow(beam, 0.6));
        for (int x = 0; x < w; x++) {
            double r = v, g = v, b = v;
            switch (x % 3) {                            /* aperture-grille stripes */
            case 0: g *= 1 - mask; b *= 1 - mask; break;
            case 1: r *= 1 - mask; b *= 1 - mask; break;
            default: r *= 1 - mask; g *= 1 - mask; break;
            }
            px[(size_t)y * w + x] = 0xFF000000u | (u32)(r * 255) << 16 | (u32)(g * 255) << 8 | (u32)(b * 255);
        }
    }
    SDL_UpdateTexture(crt_tex, NULL, px, w * 4);
    free(px);
    SDL_SetTextureBlendMode(crt_tex, SDL_BLENDMODE_MOD);
}

/* cols x lines is the console's picture, whatever the size of the texture */
static void crt_draw(SDL_Renderer *ren, SDL_Texture *tex, const SDL_Rect *src, const SDL_Rect *dst, int cols, int lines) {
    /* 1: horizontal softness (full CRT only) */
    if (video_filter == FILTER_CRT) {
        SDL_FRect d1 = { dst->x + (float)dst->w / cols * 0.5f, (float)dst->y, (float)dst->w, (float)dst->h };
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(tex, 110);
        SDL_RenderCopyF(ren, tex, src, &d1);
    }
    /* 2: scanlines + mask */
    crt_build(ren, dst->w, dst->h, lines);
    if (crt_tex) SDL_RenderCopy(ren, crt_tex, NULL, dst);
    /* 3: glow */
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    SDL_SetTextureAlphaMod(tex, video_filter == FILTER_CRT ? 60 : 40);
    SDL_RenderCopy(ren, tex, src, dst);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(tex, 255);
}

/* ------------------------------------------------------------ settings */
static char settings_path[4200];
static int enh_res = 1, enh_aa = 1, enh_upscale, enh_color;   /* see enhance_frame() */
static int enh_aspect;             /* N64: 0 4:3, 1 16:9, 2 ultrawide */
static const double n64_aspects[3] = { 4.0 / 3.0, 16.0 / 9.0, 43.0 / 18.0 };   /* 3440 x 1440 */
/* a wide picture exists only in the high-res replay */
static int n64_res(void) { return enh_aspect && enh_res < 2 ? 2 : enh_res; }
static int n64_width(void) { return ((int)(320 * n64_res() * n64_aspects[enh_aspect] * 0.75 + 1)) & ~1; }
static void n64_video(void) {
    rdp_set_hires(n64_res() * enh_aa, n64_width() * enh_aa);
    n64_set_wide(n64_aspects[enh_aspect]);
}
static int settings_get(const char *key, int def) {
    FILE *f = settings_path[0] ? fopen(settings_path, "r") : NULL;
    if (!f) return def;
    char k[64]; int v;
    while (fscanf(f, "%63s %d", k, &v) == 2) if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return def;
}
/* widescreen and sprite limit are remembered per game */
static char key_wide[32], key_limit[32];
static void settings_save(void) {
    if (!settings_path[0] || getenv("SF2_NOSAVE")) return;     /* dev runs leave the player's settings */
    /* keep other games' entries */
    char keep[256][80]; int n = 0;
    FILE *f = fopen(settings_path, "r");
    if (f) {
        char k[64]; int v;
        while (n < 256 && fscanf(f, "%63s %d", k, &v) == 2)
            if ((!strncmp(k, "wide_", 5) || !strncmp(k, "limit_", 6)) && strcmp(k, key_wide) && strcmp(k, key_limit))
                snprintf(keep[n++], sizeof keep[0], "%s %d", k, v);
        fclose(f);
    }
    f = fopen(settings_path, "w");
    if (!f) return;
    fprintf(f, "filter %d\nstrength %d\nshape %d\nrunahead %d\nspeed %d\nres %d\naa %d\nupscale %d\ncolor %d\naspect %d\n%s %d\n%s %d\n",
            video_filter, scan_strength, pic_shape, runahead, speed_pct, enh_res, enh_aa, enh_upscale, enh_color, enh_aspect,
            key_wide, vdp_widescreen, key_limit, vdp_no_sprite_limit);
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", keep[i]);
    fclose(f);
}

/* ---------------------------------------------------------- options menu */
enum { M_WIDE, M_RES, M_AA, M_UPSCALE, M_FILTER, M_STRENGTH, M_COLOR, M_SHAPE, M_LIMIT, M_BACK,
       M_ENHANCE, M_RUNAHEAD, M_SPEED, M_CLOSE, M_COUNT };
static const int menu_pages[2][10] = {
    { M_ENHANCE, M_RUNAHEAD, M_SPEED, M_CLOSE, -1 },
    { M_WIDE, M_RES, M_AA, M_UPSCALE, M_FILTER, M_STRENGTH, M_COLOR, M_SHAPE, M_LIMIT, M_BACK },
};
static int menu_page;
static int menu_open, menu_sel, menu_dirty;
static const char *menu_names[M_COUNT] = { "Widescreen", "Render resolution", "Anti-aliasing", "Pixel art upscaler",
                                           "Video filter", "Scanline strength", "Colour", "Picture shape",
                                           "Sprite limit", "Back", "Enhancements", "Run-ahead", "Game speed", "Close" };
static const char *menu_value(int i, char *b, size_t n) {
    static const char *filters[] = { "Sharp", "Smooth", "Scanlines", "CRT" };
    static const char *strengths[] = { "Soft", "Medium", "Strong" };
    switch (i) {
    case M_WIDE:     if (be == &backend_n64) { static const char *const a[3] = { "4:3", "16:9", "Ultrawide 21:9" }; return a[enh_aspect]; }
                     return vdp_widescreen ? "16:9" : "Off";
    case M_RES:      if (be != &backend_n64) return "-";
                     snprintf(b, n, "%dx  (%d x %d)", n64_res(), n64_width(), 240 * n64_res()); return b;
    case M_AA:       if (be != &backend_n64) return "-";
                     if (enh_aa == 1) return "Off";
                     snprintf(b, n, "%dx SSAA", enh_aa * enh_aa); return b;
    case M_UPSCALE:  return be == &backend_n64 ? "-" : enh_upscale == 2 ? "Scale4x" : enh_upscale ? "Scale2x" : "Off";
    case M_COLOR:    return enh_color ? "Vivid" : "Original";
    case M_ENHANCE:  return ">";
    case M_FILTER:   return filters[video_filter];
    case M_STRENGTH: return video_filter >= FILTER_SCANLINES ? strengths[scan_strength] : "-";
    case M_SHAPE:    return vdp_widescreen ? "-" : pic_shape ? "TV 4:3" : "Square pixels";
    case M_LIMIT:    return vdp_no_sprite_limit ? "Removed" : "Original";
    case M_RUNAHEAD: snprintf(b, n, "%d frame%s", runahead, runahead == 1 ? "" : "s"); return b;
    case M_SPEED:    snprintf(b, n, "%d%%", speed_pct); return b;
    default:         return "";
    }
}
static void menu_change(int i, int dir) {
    switch (i) {
    case M_WIDE:     if (be == &backend_n64) enh_aspect = (enh_aspect + dir + 3) % 3;
                     else { vdp_widescreen = !vdp_widescreen; menu_dirty = 1; }
                     break;
    case M_RES:      if (be == &backend_n64) enh_res = (enh_res - 1 + dir + 4) % 4 + 1; break;
    case M_AA:       if (be == &backend_n64) enh_aa = 3 - enh_aa; break;
    case M_UPSCALE:  if (be != &backend_n64) enh_upscale = (enh_upscale + dir + 3) % 3; break;
    case M_COLOR:    enh_color = !enh_color; break;
    case M_FILTER:   video_filter = (video_filter + dir + FILTER_COUNT) % FILTER_COUNT; break;
    case M_STRENGTH: scan_strength = (scan_strength + dir + 3) % 3; break;
    case M_SHAPE:    pic_shape = !pic_shape; break;
    case M_LIMIT:    vdp_no_sprite_limit = !vdp_no_sprite_limit; menu_dirty = 1; break;
    case M_RUNAHEAD: runahead = (runahead + dir + 3) % 3; break;
    case M_SPEED:    set_speed(speed_pct + dir * 10); break;
    default: break;
    }
}
/* returns 1 when the menu closes */
static int menu_input(int up, int down, int left, int right, int ok, int back) {
    int count = 0;
    while (count < 10 && menu_pages[menu_page][count] >= 0) count++;
    const int item = menu_pages[menu_page][menu_sel];
    if (back) {
        if (!menu_page) return 1;
        menu_page = 0; menu_sel = 0;
        return 0;
    }
    if (up) menu_sel = (menu_sel + count - 1) % count;
    if (down) menu_sel = (menu_sel + 1) % count;
    if (left) menu_change(item, -1);
    if (right) menu_change(item, 1);
    if (ok) {
        if (item == M_CLOSE) return 1;
        if (item == M_ENHANCE) { menu_page = 1; menu_sel = 0; }
        else if (item == M_BACK) { menu_page = 0; menu_sel = 0; }
        else menu_change(item, 1);
    }
    return 0;
}
static void menu_draw(u32 *fb, int stride, int w, int h) {
    fx_box(fb, stride, w, h, 0, 0, w, h, 0x000010, 0.62f);
    int count = 0;
    while (count < 10 && menu_pages[menu_page][count] >= 0) count++;
    const int PW = 248, RH = 15, PH = 54 + count * RH;
    int x0 = (w - PW) / 2, y0 = (h - PH) / 2;
    fx_box(fb, stride, w, h, x0, y0, x0 + PW, y0 + PH, 0x10102C, 0.92f);
    fx_box(fb, stride, w, h, x0, y0, x0 + PW, y0 + 1, 0x5060FF, 1.0f);
    fx_box(fb, stride, w, h, x0, y0 + PH - 1, x0 + PW, y0 + PH, 0x5060FF, 1.0f);
    const char *t = "BUILD " GENEXIA_BUILD;
    fx_text(fb, stride, w, h, t, w - fx_text_width(t, 1) - 4, 4, 1, 0xA0B0D0, 0xA0B0D0);
    t = menu_page ? "ENHANCEMENTS" : "GENEXIA OPTIONS";
    fx_text(fb, stride, w, h, t, (w - fx_text_width(t, 2)) / 2, y0 + 8, 2, 0xFFFFFF, 0x80A0FF);
    for (int i = 0; i < count; i++) {
        const int item = menu_pages[menu_page][i];
        int y = y0 + 32 + i * RH, sel = i == menu_sel;
        if (sel) fx_box(fb, stride, w, h, x0 + 6, y - 3, x0 + PW - 6, y + 10, 0x3040A0, 0.9f);
        char buf[32];
        const char *v = menu_value(item, buf, sizeof buf);
        u32 c = sel ? 0xFFFFFF : 0xB0B8D0;
        fx_text(fb, stride, w, h, menu_names[item], x0 + 12, y, 1, c, c);
        if (*v) {
            char shown[48];
            snprintf(shown, sizeof shown, sel && v[0] != '-' ? "< %s >" : "%s", v);
            fx_text(fb, stride, w, h, shown, x0 + PW - 12 - fx_text_width(shown, 1), y, 1, sel ? 0xFFE060 : 0xD0D8F0, sel ? 0xFFA020 : 0xA0B0D0);
        }
    }
    t = "UP DOWN SELECT   LEFT RIGHT CHANGE";
    fx_text(fb, stride, w, h, t, (w - fx_text_width(t, 1)) / 2, y0 + PH - 14, 1, 0x8090B0, 0x8090B0);
}
/* The picture after enhancements, or NULL when the console's own output is
 * shown as it is. Sets its size; the pixels stay valid until the next call. */
static const u32 *enhance_frame(int *pw, int *ph) {
    static u32 *buf, *tmp;
    const int sw = vdp_screen_w, sh = vdp_screen_h;
    const u32 *out = NULL;
    int w = sw, h = sh;
    if (!buf) { buf = malloc(4096 * 2048 * 4); tmp = malloc(4096 * 2048 * 4); }
    if (be == &backend_n64) {
        n64_video();
        if (n64_res() * enh_aa > 1) {
            w = n64_width() * sw / 320; h = sh * n64_res();
            if (rdp_hires_fetch(n64_vi_origin(), enh_aa, buf, w, h)) out = buf;
            else { w = sw; h = sh; }
        }
    } else if (enh_upscale) {
        enhance_scale2x(vdp_framebuffer, VDP_FB_STRIDE, sw, sh, buf);
        w = sw * 2; h = sh * 2;
        if (enh_upscale == 2) {
            memcpy(tmp, buf, (size_t)w * h * 4);
            enhance_scale2x(tmp, w, w, h, buf);
            w *= 2; h *= 2;
        }
        out = buf;
    }
    if (enh_color) {
        if (!out) {
            for (int y = 0; y < h; y++) memcpy(buf + (size_t)y * w, &vdp_framebuffer[y * VDP_FB_STRIDE], (size_t)w * 4);
            out = buf;
        }
        enhance_vivid(buf, w * h);
    }
    *pw = w; *ph = h;
    return out;
}

/* show a changed video option on the paused picture without advancing the game */
static void refresh_picture(void) {
    if (!ra_buf) ra_buf = malloc(state_size());
    state_save(ra_buf);
    audio_discard = 1;
    run_frame();
    audio_discard = 0;
    state_load(ra_buf);
}

static double display_refresh(SDL_Window *win) {
    SDL_DisplayMode m;
    int d = SDL_GetWindowDisplayIndex(win);
    if (d >= 0 && SDL_GetCurrentDisplayMode(d, &m) == 0 && m.refresh_rate >= 30) return m.refresh_rate;
    return 60.0;
}

static volatile sig_atomic_t quit_req;

/* ------------------------------------------------------- game selector
 * Shown when no game is named on the command line: the built-in game plus
 * every ROM in a roms/ folder. Each file goes through configure_game() so the
 * list says what will really happen when it starts. */
typedef struct { char path[1024], name[96]; const char *console; int recompiled; } GameEntry;
#define MAX_GAMES 256
static GameEntry games[MAX_GAMES];
static int ngames;
static SDL_Window *sel_win;      /* main() carries on in the selector's window */
static SDL_Renderer *sel_ren;

static void game_probe(const char *path, const char *fname) {
    if (ngames == MAX_GAMES) return;
    GameEntry *g = &games[ngames];
    if (!load_rom(path)) return;
    int force = m68k_force_interp;
    configure_game(1);
    g->console = be->name;
    g->recompiled = be == &backend_n64 ? !n64_force_interp
                  : be == &backend_snes ? !snes_force_interp : m68k_register_game && !m68k_force_interp;
    m68k_force_interp = force;
    snprintf(g->path, sizeof g->path, "%s", path ? path : "");
    snprintf(g->name, sizeof g->name, "%s", path ? fname : game_title);
    char *dot = strrchr(g->name, '.');
    if (dot && path) *dot = 0;
    ngames++;
}

static int game_known(const char *fname) {
    for (int i = 0; i < ngames; i++) {
        const char *f = strrchr(games[i].path, '/');
        if (f && !strcmp(f + 1, fname)) return 1;
    }
    return 0;
}

static void game_scan(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    static const char *exts[] = { ".bin", ".md", ".gen", ".smd", ".sfc", ".smc", ".z64", ".n64", ".v64" };
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth) game_scan(p, depth - 1); continue; }
        const char *x = strrchr(e->d_name, '.');
        for (unsigned i = 0; x && i < sizeof exts / sizeof *exts; i++)
            if (!strcasecmp(x, exts[i]) && !game_known(e->d_name)) { game_probe(p, e->d_name); break; }
    }
    closedir(d);
}

static int game_order(const void *a, const void *b) {
    const GameEntry *x = a, *y = b;
    if (!x->path[0] != !y->path[0]) return x->path[0] ? 1 : -1;   /* built-in game first */
    int c = strcmp(x->console, y->console);
    return c ? c : strcasecmp(x->name, y->name);
}

static void game_list(const char *argv0) {
    const unsigned char *data;
    probing = 1;
    if (sf2_embedded_rom(&data) >= 0x200) game_probe(NULL, NULL);
    else {
        const char *near = find_rom(argv0);
        if (near) game_probe(near, strrchr(near, '/') ? strrchr(near, '/') + 1 : near);
    }
    char *base = SDL_GetBasePath(), *pref = SDL_GetPrefPath("Genexia", "Genexia"), p[1024];
    game_scan("roms", 1);
    if (base) {
        snprintf(p, sizeof p, "%sroms", base); game_scan(p, 1);
        snprintf(p, sizeof p, "%s../../../roms", base); game_scan(p, 1);   /* app next to roms/ */
    }
    if (pref) { snprintf(p, sizeof p, "%sroms", pref); game_scan(p, 1); }
    SDL_free(base); SDL_free(pref);
    probing = 0;
    qsort(games, (size_t)ngames, sizeof *games, game_order);
}

enum { SEL_W = 398, SEL_H = 224, SEL_RH = 18, SEL_TOP = 46, SEL_ROWS = 8 };

static void select_draw(u32 *fb, int sel, int top) {
    fx_box(fb, VDP_FB_STRIDE, SEL_W, SEL_H, 0, 0, SEL_W, SEL_H, 0x0A0A1E, 1.0f);
    fx_box(fb, VDP_FB_STRIDE, SEL_W, SEL_H, 0, 34, SEL_W, 35, 0x5060FF, 1.0f);
    const char *t = "GENEXIA";
    fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, t, (SEL_W - fx_text_width(t, 3)) / 2, 8, 3, 0xFFFFFF, 0x80A0FF);
    if (!ngames) {
        t = "No games found. Put ROMs in a roms folder.";
        fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, t, (SEL_W - fx_text_width(t, 1)) / 2, 100, 1, 0xD0D8F0, 0xA0B0D0);
    }
    for (int i = top; i < ngames && i < top + SEL_ROWS; i++) {
        int y = SEL_TOP + (i - top) * SEL_RH, on = i == sel;
        if (on) fx_box(fb, VDP_FB_STRIDE, SEL_W, SEL_H, 10, y - 4, SEL_W - 10, y + 12, 0x3040A0, 0.9f);
        const GameEntry *g = &games[i];
        const char *tag = g->recompiled ? "RECOMPILED" : "INTERPRETED";
        u32 tc = g->recompiled ? 0x60E090 : 0x8090B0;
        int tagw = fx_text_width(tag, 1), conw = 84;
        char name[96];
        snprintf(name, sizeof name, "%s", g->name);
        int room = SEL_W - 28 - tagw - conw - 12;
        for (size_t n = strlen(name); n && fx_text_width(name, 1) > room; n--) name[n - 1] = 0;
        u32 c = on ? 0xFFFFFF : 0xB0B8D0;
        fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, name, 18, y, 1, c, c);
        fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, g->console, SEL_W - 18 - tagw - conw, y, 1, on ? 0xFFE060 : 0xA0B0D0, on ? 0xFFA020 : 0xA0B0D0);
        fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, tag, SEL_W - 18 - tagw, y, 1, tc, tc);
    }
    t = "UP DOWN CHOOSE   ENTER OR A PLAY   ESC QUIT";
    fx_text(fb, VDP_FB_STRIDE, SEL_W, SEL_H, t, (SEL_W - fx_text_width(t, 1)) / 2, SEL_H - 14, 1, 0x8090B0, 0x8090B0);
}

/* Returns the chosen entry, or NULL to quit. */
static const GameEntry *game_select(const char *argv0, int scale) {
    game_list(argv0);
    sel_win = SDL_CreateWindow("Genexia", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SEL_W * scale, SEL_H * scale,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    sel_ren = SDL_CreateRenderer(sel_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = SDL_CreateTexture(sel_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SEL_W, SEL_H);
    SDL_RenderSetLogicalSize(sel_ren, SEL_W, SEL_H);
    SDL_GameController *pads[8];
    int npads = 0;
    for (int i = 0; i < SDL_NumJoysticks() && npads < 8; i++)
        if (SDL_IsGameController(i)) pads[npads++] = SDL_GameControllerOpen(i);

    static u32 fb[VDP_FB_STRIDE * 240];
    int sel = 0, top = 0, stick = 0, frame = 0;
    const GameEntry *pick = NULL;
    int done = 0, shot_at = getenv("SF2_SELECT_SHOT") ? 5 : -1;
    if (getenv("SF2_SELECT_ROW")) sel = atoi(getenv("SF2_SELECT_ROW")) % (ngames ? ngames : 1);
    while (!done) {
        SDL_Event e;
        int move = 0, go = 0, back = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) back = 1;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Scancode c = e.key.keysym.scancode;
                move += c == SDL_SCANCODE_DOWN ? 1 : c == SDL_SCANCODE_UP ? -1 : 0;
                go |= c == SDL_SCANCODE_RETURN || c == SDL_SCANCODE_SPACE;
                back |= c == SDL_SCANCODE_ESCAPE;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = e.cbutton.button;
                move += b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 : b == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 0;
                go |= b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START;
            } else if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int d = e.caxis.value > 16000 ? 1 : e.caxis.value < -16000 ? -1 : 0;
                if (d != stick) move += d;
                stick = d;
            } else if (e.type == SDL_CONTROLLERDEVICEADDED && npads < 8) {
                pads[npads++] = SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                int row = (y - SEL_TOP + 4) / SEL_RH;
                if (y >= SEL_TOP - 4 && row < SEL_ROWS && top + row < ngames) {
                    sel = top + row;
                    go |= e.type == SDL_MOUSEBUTTONDOWN;
                }
            }
        }
        if (ngames) sel = (sel + move + ngames * (move < 0 ? -move : 1)) % ngames;
        if (sel < top) top = sel;
        if (sel >= top + SEL_ROWS) top = sel - SEL_ROWS + 1;
        if (go && ngames) { pick = &games[sel]; done = 1; }
        if (back || quit_req) done = 1;
        select_draw(fb, sel, top);
        SDL_UpdateTexture(tex, NULL, fb, VDP_FB_STRIDE * 4);
        SDL_RenderClear(sel_ren);
        SDL_RenderCopy(sel_ren, tex, NULL, NULL);
        SDL_RenderPresent(sel_ren);
        if (frame++ == shot_at) {                 /* dev: capture, then start the row */
            FILE *f = fopen(getenv("SF2_SELECT_SHOT"), "wb");
            fprintf(f, "P6\n%d %d\n255\n", SEL_W, SEL_H);
            for (int y = 0; y < SEL_H; y++) for (int x = 0; x < SEL_W; x++) {
                u32 v = fb[y * VDP_FB_STRIDE + x];
                fputc((int)(v >> 16) & 255, f); fputc((int)(v >> 8) & 255, f); fputc((int)v & 255, f);
            }
            fclose(f);
            if (ngames) pick = &games[sel];
            done = 1;
        }
    }
    for (int i = 0; i < npads; i++) if (pads[i]) SDL_GameControllerClose(pads[i]);
    SDL_DestroyTexture(tex);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_CONTROLLERAXISMOTION + 8);
    return pick;
}

static void on_signal(int s) { (void)s; quit_req = 1; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--version")) { printf("Genexia build %s\n", GENEXIA_BUILD); return 0; }
    const char *rom_path = NULL, *shot = NULL, *wav = NULL;
    int frames = 0, scale = 3, headless = 0, state_test = 0;
    int wide_opt = -1, limit_opt = -1, region_set = 0, runahead_cli = 0;
    unsigned monkey = 0;
    /* saved options first (window mode only), command line overrides them */
    int gui = 1;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--frames")) gui = 0;
    if (gui) {
        char *pref = SDL_GetPrefPath("Genexia", "Genexia");
        if (pref) { snprintf(settings_path, sizeof settings_path, "%ssettings.txt", pref); SDL_free(pref); }
        video_filter = settings_get("filter", video_filter) % FILTER_COUNT;
        scan_strength = settings_get("strength", scan_strength) % 3;
        enh_res = settings_get("res", 1); enh_aa = settings_get("aa", 1); enh_aspect = settings_get("aspect", 0);
        enh_upscale = settings_get("upscale", 0); enh_color = settings_get("color", 0);
        pic_shape = settings_get("shape", pic_shape) & 1;
        runahead = settings_get("runahead", runahead) % 3;
        set_speed(settings_get("speed", speed_pct));
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) m68k_force_interp = 1;
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--wide")) wide_opt = 1;
        else if (!strcmp(argv[i], "--no-wide")) wide_opt = 0;
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) enh_res = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) enh_aspect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aa") && i + 1 < argc) enh_aa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upscale") && i + 1 < argc) enh_upscale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) enh_color = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crt")) video_filter = FILTER_CRT;
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            static const char *n[] = { "sharp", "smooth", "scanlines", "crt" };
            i++;
            for (int k = 0; k < FILTER_COUNT; k++) if (!strcmp(argv[i], n[k])) video_filter = k;
        }
        else if (!strcmp(argv[i], "--mana") && i + 1 < argc) {      /* seconds to fill, 0 = no meter */
            int m = atoi(argv[++i]);
            if (m <= 0) combo_mana_enabled = 0; else combo_mana_seconds = m;
        }
        else if (!strcmp(argv[i], "--runahead") && i + 1 < argc) runahead_cli = 1, runahead = atoi(argv[++i]) < 0 ? 0 : atoi(argv[i]) > 3 ? 3 : atoi(argv[i]);
        else if (!strcmp(argv[i], "--state-test")) state_test = 1;
        else if (!strcmp(argv[i], "--sprite-limit")) limit_opt = 0;
        else if (!strcmp(argv[i], "--no-sprite-limit")) limit_opt = 1;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) set_speed(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoi(argv[++i]); headless = 1; }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--monkey") && i + 1 < argc) monkey = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) { region_version = (u8)strtol(argv[++i], NULL, 0); region_set = 1; }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 256) {
            char *s = argv[++i], *colon = strchr(s, ':');
            if (colon) {
                /* frame:buttons for P1, frame:2buttons for P2 */
                presses[npress].frame = atoi(s);
                presses[npress].player = colon[1] == '2';
                presses[npress].buttons = parse_buttons(colon + 1 + (colon[1] == '2'));
                npress++;
            }
        }
        else rom_path = argv[i];
    }
    if (SDL_Init(headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* no game named: let the player choose */
    if (gui && !rom_path) {
        const GameEntry *g = game_select(argv[0], scale);
        if (!g) { SDL_Quit(); return 0; }
        if (g->path[0]) rom_path = g->path;
    }
    /* a ROM given on the command line wins, then the built-in copy, then a file nearby */
    int embedded = 0;
    if (!rom_path && load_rom(NULL)) embedded = 1;
    else {
        if (!rom_path) rom_path = find_rom(argv[0]);
        if (!rom_path || !load_rom(rom_path)) {
            fprintf(stderr, "Cannot load ROM. Usage: %s --rom path/to/game.bin\n", argv[0]);
            return 1;
        }
    }
    configure_game(region_set);
    snprintf(key_wide, sizeof key_wide, "wide_%02X%02X", rom[0x18E], rom[0x18F]);
    snprintf(key_limit, sizeof key_limit, "limit_%02X%02X", rom[0x18E], rom[0x18F]);
    int enhanced = vdp_hud_rules != 0 || (be == &backend_snes && snes_register_game);
    if (be == &backend_n64 && !runahead_cli) runahead = 0;   /* 8 MB per snapshot: opt-in */
    vdp_widescreen = wide_opt >= 0 ? wide_opt : settings_get(key_wide, enhanced);
    vdp_no_sprite_limit = limit_opt >= 0 ? limit_opt : settings_get(key_limit, enhanced);
    power_on();
    if (wav) wav_open(wav);
    /* save-state slot: next to the ROM file, or in Application Support when built in */
    static char state_path[4200];
    if (embedded) {
        char *pref = headless ? NULL : SDL_GetPrefPath("Genexia", "Genexia");
        snprintf(state_path, sizeof state_path, "%ssf2.state", pref ? pref : "");
        SDL_free(pref);
    } else snprintf(state_path, sizeof state_path, "%s.state", rom_path);

    int state_at = getenv("SF2_STATE_AT") ? atoi(getenv("SF2_STATE_AT")) : 0;
    u8 *st_buf = NULL; unsigned st_monkey = 0; u64 st_h1 = 0, st_h2 = 0; int st_pass = 0;
    __typeof__(mk) st_mk;
    if (headless && getenv("SF2_COMBO_TEST")) return combo_test(getenv("SF2_COMBO_TEST"));
    if (headless && state_test) {
        /* determinism / completeness check of save states */
        for (int f = 0; f < frames; f++) run_frame();
        u8 *buf = malloc(state_size());
        state_save(buf);
        u64 h1 = 0, h2 = 0;
        for (int f = 0; f < 600; f++) { pad_state[0] = (f / 7) % 3 ? PAD_RIGHT : PAD_X; run_frame(); h1 ^= ram_hash() + (u64)f; }
        state_load(buf);
        for (int f = 0; f < 600; f++) { pad_state[0] = (f / 7) % 3 ? PAD_RIGHT : PAD_X; run_frame(); h2 ^= ram_hash() + (u64)f; }
        fprintf(stderr, "state size %zu bytes; replay after load %s\n", state_size(), h1 == h2 ? "IDENTICAL" : "DIFFERENT");
        return h1 == h2 ? 0 : 2;
    }
    if (headless) {
        for (int f = 0; f < frames; f++) {
            u16 b[2] = { 0, 0 };
            for (int i = 0; i < npress; i++)
                if (presses[i].frame <= f && f < presses[i].frame + 6) b[presses[i].player] |= presses[i].buttons;
            pad_state[0] = b[0];
            pad_state[1] = b[1];
            if (getenv("SF2_HOLD")) {          /* dev: start,end,buttons held on P1; several separated by ; */
                int h0, h1, used; char hb[16];
                for (const char *h = getenv("SF2_HOLD"); sscanf(h, "%d,%d,%15[^;]%n", &h0, &h1, hb, &used) == 3; h += used + (h[used] == ';')) {
                    if (f >= h0 && f < h1) pad_state[0] |= parse_buttons(hb);
                    if (!h[used]) break;
                }
            }
            if (state_at && f == state_at && !st_buf && !st_pass) {   /* determinism check mid-game */
                st_buf = malloc(state_size()); state_save(st_buf); st_monkey = monkey; st_mk = mk;
            }
            if (monkey) monkey_input(&monkey, f);
            { extern int vdp_layout_debug; vdp_layout_debug = getenv("SF2_LAYOUT") && f == atoi(getenv("SF2_LAYOUT")); }
            { extern int rdp_debug; rdp_debug = getenv("SF2_RDPLOG") && f == atoi(getenv("SF2_RDPLOG")); }
            { extern int n64_dl_log; if (getenv("SF2_RDPLOG")) { int t = atoi(getenv("SF2_RDPLOG")); if (f == t - 2) n64_dl_log = 2; if (f > t) n64_dl_log = 0; } }
            if (be == &backend_n64) n64_video();
            vdp_skip_render = getenv("SF2_LIVELIKE") != NULL;   /* dev: as the window loop does */
            run_frame();
            vdp_skip_render = 0;
            if (getenv("SF2_HEADLESS_RA")) run_ahead_frames();
            if (getenv("SF2_STATEHASH") && f % atoi(getenv("SF2_STATEHASH")) == 0) {   /* dev: compare two runs */
                static u8 *hb; if (!hb) hb = malloc(state_size());
                state_save(hb);
                u64 h = 1469598103934665603ull;
                for (size_t i = 0; i < state_size(); i += 8) { u64 w; memcpy(&w, hb + i, 8); h = (h ^ w) * 1099511628211ull; }
                printf("%d %016llx\n", f, (unsigned long long)h);
            }
            if (st_buf && f >= state_at && f < state_at + 900) {
                if (st_pass == 0) st_h1 = st_h1 * 31 + ram_hash(); else st_h2 = st_h2 * 31 + ram_hash();
                if (f == state_at + 899) {
                    if (st_pass == 0) {   /* rewind and replay the same 900 frames */
                        state_load(st_buf); monkey = st_monkey; mk = st_mk; st_pass = 1; f = state_at - 1;
                        continue;
                    }
                    fprintf(stderr, "mid-game save state replay (900 frames from %d): %s\n", state_at,
                            st_h1 == st_h2 ? "IDENTICAL" : "DIFFERENT");
                    free(st_buf); st_buf = NULL;
                }
            }
            if (getenv("SF2_RAMLOG") && f >= 13000 && f % 10 == 0) {
                int vdp_debug_hscroll(int, int);
                static FILE *rl; if (!rl) rl = fopen(getenv("SF2_RAMLOG"), "wb");
                s32 hs[4] = { vdp_debug_hscroll(100, 0), vdp_debug_hscroll(100, 1), vdp_debug_hscroll(200, 0), vdp_debug_hscroll(200, 1) };
                fwrite(hs, 4, 4, rl); fwrite(ram, 1, sizeof ram, rl); fflush(rl);
            }
            if (getenv("SF2_CHARLOG") && vdp_screen_w == 342 && f % 30 == 0) {
                static FILE *cl; if (!cl) cl = fopen(getenv("SF2_CHARLOG"), "w");
                #define RW(a) ((s16)(ram[(a) & 0xFFFF] << 8 | ram[((a) + 1) & 0xFFFF]))
                fprintf(cl, "%d %d %d %d %d %d %d\n", f, ram[0x81DB], ram[0x845B], RW(0x8006), RW(0x8286), RW(0x8042), RW(0x82C2));
                #undef RW
            }
            if (getenv("SF2_FIGHTLOG") && vdp_screen_w == 342 && f % 5 == 0) {
                int vdp_debug_pal_bbox(int, int *, int *, int *, int *);
                static FILE *fl; if (!fl) fl = fopen(getenv("SF2_FIGHTLOG"), "wb");
                s32 rec[12] = { f };
                rec[1] = vdp_debug_pal_bbox(2, &rec[2], &rec[3], &rec[4], &rec[5]);
                rec[6] = vdp_debug_pal_bbox(3, &rec[7], &rec[8], &rec[9], &rec[10]);
                rec[11] = vdp_view_x;
                fwrite(rec, 4, 12, fl); fwrite(ram, 1, sizeof ram, fl);
            }
            if (getenv("SF2_PAL0LOG") && (vdp_screen_w == 342 || vdp_screen_w == 426)) {
                void vdp_debug_pal0(FILE *, int);
                static FILE *pl; if (!pl) pl = fopen(getenv("SF2_PAL0LOG"), "w");
                vdp_debug_pal0(pl, f);
            }
            if (getenv("SF2_VIEWLOG")) {
                int vdp_debug_hscroll(int, int);
                static FILE *vl; if (!vl) vl = fopen(getenv("SF2_VIEWLOG"), "w");
                int vdp_debug_vscroll(int);
                fprintf(vl, "%d %d %d %d %d %d\n", f, vdp_view_x, vdp_debug_hscroll(120, 1), vdp_screen_w,
                        vdp_debug_vscroll(0), vdp_debug_vscroll(1));
            }
            if (getenv("SF2_DUMP_FRAME") && f == atoi(getenv("SF2_DUMP_FRAME"))) {
                void vdp_debug_dump(const char *);
                vdp_debug_dump(getenv("SF2_DUMP_PATH") ? getenv("SF2_DUMP_PATH") : "planes.ppm");
                { void vdp_debug_sat(void); vdp_debug_sat(); }
                if (shot) save_ppm(shot);
            }
            int n = resample();
            if (wav_f) { fwrite(host_buf, 4, (size_t)n, wav_f); wav_bytes += (u32)n * 4; }
            if (shot && getenv("SF2_SHOT_EVERY") && f % atoi(getenv("SF2_SHOT_EVERY")) == 0) {
                char p[512]; snprintf(p, sizeof p, "%s.%05d.ppm", shot, f); save_ppm(p);
                int ew, eh; const u32 *eo = enhance_frame(&ew, &eh);
                if (eo) {
                    snprintf(p, sizeof p, "%s.%05d.hi.ppm", shot, f);
                    FILE *o = fopen(p, "wb");
                    fprintf(o, "P6\n%d %d\n255\n", ew, eh);
                    for (int i = 0; i < ew * eh; i++) { u8 c[3] = { (u8)(eo[i] >> 16), (u8)(eo[i] >> 8), (u8)eo[i] }; fwrite(c, 1, 3, o); }
                    fclose(o);
                }
            }
        }
        if (shot) {
            fx_apply(vdp_framebuffer, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h);
            if (getenv("SF2_MENU_SHOT")) { menu_sel = 1; menu_page = atoi(getenv("SF2_MENU_SHOT")) - 1; menu_draw(vdp_framebuffer, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h); }
            save_ppm(shot);
        }
        wav_close();
        { extern long vdp_stat_limited_lines, vdp_stat_dot_lines; fprintf(stderr, "lines over the hardware sprite limit: count %ld, pixels %ld\n", vdp_stat_limited_lines, vdp_stat_dot_lines); }
        if (be == &backend_n64) n64_debug_print();
        fprintf(stderr, "frames %d  blocks %llu  interp-steps %llu  pc %06X\n", frames,
                (unsigned long long)(m68k_blocks_run + snes_blocks_run),
                (unsigned long long)(m68k_interp_run + snes_interp_run), cpu.pc);
        return 0;
    }

    signal(SIGTERM, on_signal);
    signal(SIGALRM, on_signal);
    SDL_Window *win = sel_win ? sel_win : SDL_CreateWindow("Genexia",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       (vdp_widescreen ? 398 : 320) * scale, 224 * scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (sel_win) SDL_SetWindowSize(win, (vdp_widescreen ? 398 : 320) * scale, 224 * scale);
    SDL_Renderer *ren = sel_ren ? sel_ren : SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VDP_FB_STRIDE, 240);
    SDL_RenderSetLogicalSize(ren, 320, 224);

    SDL_AudioSpec want = {0}, have;
    want.freq = HOST_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    SDL_GameController *gcs[2] = {0};
    /* players follow who presses first, not the order macOS lists devices:
     * the first controller to press a button is P1, the next is P2 */
    int pad_player[2] = { -1, -1 }, players_taken = 0;
    int ngc = 0;
    for (int i = 0; i < SDL_NumJoysticks() && ngc < 2; i++)
        if (SDL_IsGameController(i)) {
            gcs[ngc] = SDL_GameControllerOpen(i);
            fprintf(stderr, "Controller found: '%s'\n", SDL_GameControllerName(gcs[ngc]));
            ngc++;
        }

    const double frame_s = 1.0 / (53693175.0 / (3420.0 * 262.0));
    u64 freq = SDL_GetPerformanceFrequency();
    double next = (double)SDL_GetPerformanceCounter() / freq;
    int running = 1, paused = 0, fast = 0;
    u32 osd_until = getenv("SF2_WINSHOT_OSD") ? 0xFFFFFFFFu : 0;          /* speed slider visible until this tick */
    int dragging = 0, last_speed = -1;
    u64 t_start = SDL_GetPerformanceCounter(), nframes = 0;
    double frame_acc = 0, refresh_hz = display_refresh(win);
    double last_present = (double)SDL_GetPerformanceCounter() / freq;
    while (running) {
        SDL_Event e;
        if (quit_req) running = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERBUTTONDOWN)
                for (int g = 0; g < ngc; g++) {
                    if (!gcs[g] || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[g])) != e.cbutton.which) continue;
                    if (pad_player[g] < 0 && players_taken < 2) {
                        pad_player[g] = players_taken++;
                        combo_pad_hint[pad_player[g]] = 1;
                        fprintf(stderr, "Controller '%s' is Player %d\n", SDL_GameControllerName(gcs[g]), pad_player[g] + 1);
                    }
                }
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN && menu_open) {
                SDL_Scancode c = e.key.keysym.scancode;
                if (menu_input(c == SDL_SCANCODE_UP, c == SDL_SCANCODE_DOWN, c == SDL_SCANCODE_LEFT, c == SDL_SCANCODE_RIGHT,
                               c == SDL_SCANCODE_RETURN || c == SDL_SCANCODE_SPACE, c == SDL_SCANCODE_ESCAPE || c == SDL_SCANCODE_F10 || c == SDL_SCANCODE_F9)) {
                    menu_open = 0; settings_save();
                }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN && menu_open) {
                int b = e.cbutton.button;
                if (menu_input(b == SDL_CONTROLLER_BUTTON_DPAD_UP, b == SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                               b == SDL_CONTROLLER_BUTTON_DPAD_LEFT, b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                               b == SDL_CONTROLLER_BUTTON_A, b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START)) {
                    menu_open = 0; settings_save();
                }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN &&
                       (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK ||
                        ((e.cbutton.button == SDL_CONTROLLER_BUTTON_START || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) &&
                         SDL_GameControllerGetButton(SDL_GameControllerFromInstanceID(e.cbutton.which),
                             e.cbutton.button == SDL_CONTROLLER_BUTTON_START ? SDL_CONTROLLER_BUTTON_BACK : SDL_CONTROLLER_BUTTON_START)))) {
                menu_open = 1; menu_sel = 0; menu_page = 0;   /* R3, or Start + Back together */
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE: running = 0; break;
                case SDL_SCANCODE_P: paused = !paused; break;
                case SDL_SCANCODE_F1: power_on(); break;
                case SDL_SCANCODE_F2: if (be != &backend_n64) vdp_widescreen = !vdp_widescreen; break;
                case SDL_SCANCODE_F3: video_filter = (video_filter + 1) % FILTER_COUNT; break;
                case SDL_SCANCODE_F10: menu_open = 1; menu_sel = 0; menu_page = 0; break;
                case SDL_SCANCODE_F9: menu_open = 1; menu_sel = 0; menu_page = 1; break;
                case SDL_SCANCODE_F4: vdp_no_sprite_limit = !vdp_no_sprite_limit; break;
                case SDL_SCANCODE_Q: combo_pad_hint[0] = 0; combo_start(0, 0); break;
                case SDL_SCANCODE_W: combo_pad_hint[0] = 0; combo_start(0, 1); break;
                case SDL_SCANCODE_U: combo_pad_hint[1] = 0; combo_start(1, 0); break;
                case SDL_SCANCODE_O: combo_pad_hint[1] = 0; combo_start(1, 1); break;
                case SDL_SCANCODE_F6: runahead = (runahead + 1) % 3; last_speed = -1; break;
                case SDL_SCANCODE_F5: state_write_file(state_path); break;
                case SDL_SCANCODE_F7: state_read_file(state_path); break;
                case SDL_SCANCODE_MINUS: case SDL_SCANCODE_KP_MINUS:
                    set_speed(speed_pct - 10); osd_until = SDL_GetTicks() + 2500; break;
                case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_KP_PLUS:
                    set_speed(speed_pct + 10); osd_until = SDL_GetTicks() + 2500; break;
                case SDL_SCANCODE_0: case SDL_SCANCODE_KP_0:
                    set_speed(100); osd_until = SDL_GetTicks() + 2500; break;
                case SDL_SCANCODE_TAB: fast = 1; break;
                case SDL_SCANCODE_F11: {
                    Uint32 fl = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(win, (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break; }
                default: break;
                }
            } else if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_TAB) fast = 0;
            else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                int mx, my, ww, wh;
                SDL_GetMouseState(&mx, &my);
                SDL_GetWindowSize(win, &ww, &wh);
                OsdRect r = osd_layout(ww, wh);
                int on_bar = mx >= r.x - 12 && mx <= r.x + r.w + 12 && my >= r.y - 16 && my <= r.y + r.h + 16;
                if (my > wh - wh / 5) osd_until = SDL_GetTicks() + 2500;   /* hover near bottom shows it */
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && on_bar &&
                    SDL_GetTicks() < osd_until) dragging = 1;
                if (e.type == SDL_MOUSEBUTTONUP) dragging = 0;
                if (dragging) {
                    double t = (double)(mx - r.x) / r.w;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    set_speed((int)(50 + t * 150 + 0.5));
                    osd_until = SDL_GetTicks() + 2500;
                }
            }
            else if (e.type == SDL_WINDOWEVENT) refresh_hz = display_refresh(win);
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button != SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                for (int g = 0; g < ngc; g++)
                    if (gcs[g] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[g])) == e.cbutton.which && pad_player[g] >= 0) combo_pad_hint[pad_player[g]] = 1;
            }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                for (int g = 0; g < ngc; g++)
                    if (gcs[g] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[g])) == e.cbutton.which && pad_player[g] >= 0) { combo_pad_hint[pad_player[g]] = 1; combo_start(pad_player[g], 0); }
            }
            else if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && e.caxis.value > 20000) {
                for (int g = 0; g < ngc; g++)
                    if (gcs[g] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[g])) == e.caxis.which && pad_player[g] >= 0) { combo_pad_hint[pad_player[g]] = 1; combo_start(pad_player[g], 1); }
            }
            else if (e.type == SDL_CONTROLLERDEVICEADDED && ngc < 2) {
                /* SDL also announces pads already opened at start-up: opening
                 * one twice would make a single pad act as both players */
                SDL_GameController *added = SDL_GameControllerOpen(e.cdevice.which);
                int known = 0;
                for (int g = 0; g < ngc; g++) known |= gcs[g] == added;
                if (added && !known) {
                    gcs[ngc++] = added;
                    fprintf(stderr, "Controller connected: '%s'\n", SDL_GameControllerName(added));
                } else if (added) SDL_GameControllerClose(added);   /* drop the extra reference */
            }
        }
        const u8 *k = SDL_GetKeyboardState(NULL);
        u16 live_pad[2] = { keyboard_pad(k, 0), keyboard_pad(k, 1) };
        for (int g = 0; g < ngc; g++)
            if (pad_player[g] >= 0) live_pad[pad_player[g]] |= controller_pad(gcs[g]);
        n64_stick_x[0] = n64_stick_x[1] = n64_stick_y[0] = n64_stick_y[1] = 0;
        n64_c_buttons[0] = n64_c_buttons[1] = 0;
        for (int g = 0; g < ngc; g++) if (pad_player[g] >= 0) {  /* analog sticks for N64 */
            int pl = pad_player[g];
            int ax = SDL_GameControllerGetAxis(gcs[g], SDL_CONTROLLER_AXIS_LEFTX);
            int ay = SDL_GameControllerGetAxis(gcs[g], SDL_CONTROLLER_AXIS_LEFTY);
            int rx = SDL_GameControllerGetAxis(gcs[g], SDL_CONTROLLER_AXIS_RIGHTX);
            int ry = SDL_GameControllerGetAxis(gcs[g], SDL_CONTROLLER_AXIS_RIGHTY);
            if (abs(ax) > 3000) n64_stick_x[pl] = (u8)(s8)(ax * 80 / 32767);
            if (abs(ay) > 3000) n64_stick_y[pl] = (u8)(s8)(-ay * 80 / 32767);
            if (ry < -16000) n64_c_buttons[pl] |= 1;
            if (ry > 16000) n64_c_buttons[pl] |= 2;
            if (rx < -16000) n64_c_buttons[pl] |= 4;
            if (rx > 16000) n64_c_buttons[pl] |= 8;
        }
        pad_state[0] = live_pad[0];
        pad_state[1] = live_pad[1];

        /* Pacing is locked to the display: presenting is vsynced, and every
         * refresh runs (refresh period / game frame period) emulated frames,
         * accumulated fractionally. Ratios within 1% of n or 1/n (e.g. 100%
         * speed on a 60 or 120 Hz screen) are snapped so motion is perfectly
         * even; the tiny clock mismatch is absorbed by audio rate control. */
        double now = (double)SDL_GetPerformanceCounter() / freq;
        int todo = 0;
        if (fast && !menu_open) todo = 8;
        else if (!paused && !menu_open) {
            double ratio = (1.0 / refresh_hz) / (frame_s * 100.0 / speed_pct);
            if (ratio >= 1.0) {
                double n = floor(ratio + 0.5);
                if (fabs(ratio - n) < 0.01 * ratio) ratio = n;
            } else {
                double inv = 1.0 / ratio, n = floor(inv + 0.5);
                if (fabs(inv - n) < 0.01 * inv) ratio = 1.0 / n;
            }
            frame_acc += ratio;
            todo = (int)(frame_acc + 1e-9);
            frame_acc -= todo;
            if (todo > 8) { todo = 8; frame_acc = 0; }
        }
        { static int warm; if (++warm > 30 && todo < 9) todo_hist[todo]++; }
        for (int i = 0; i < todo; i++) {
            nframes++;
            /* combos drive the pad once per real frame */
            pad_state[0] = combo_input(0, live_pad[0]);
            pad_state[1] = combo_input(1, live_pad[1]);
            /* with run-ahead the shown picture comes from the future frame */
            vdp_skip_render = runahead > 0 && !fast;
            run_frame();
            vdp_skip_render = 0;
            if (dev && !fast) {
                /* dynamic rate control: steer the queue towards ~50 ms */
                double queued = SDL_GetQueuedAudioSize(dev) / 4.0, target = HOST_RATE * 0.05;
                double err = (queued - target) / target;
                if (err > 1) err = 1;
                if (err < -1) err = -1;
                audio_rate_adjust = 1.0 + 0.005 * err;
            }
            int n = resample();
            produced_samples += n;
            /* hard cap at ~120 ms (start-up burst, stalls): drop rather than lag */
            if (dev && !fast && SDL_GetQueuedAudioSize(dev) < (Uint32)(HOST_RATE * 4 * 0.12))
                SDL_QueueAudio(dev, host_buf, (Uint32)n * 4);
            if (wav_f) { fwrite(host_buf, 4, (size_t)n, wav_f); wav_bytes += (u32)n * 4; }
        }
        if (todo && !fast && !paused) run_ahead_frames();
        (void)now; (void)next;

        if (menu_dirty) { refresh_picture(); menu_dirty = 0; }
        if (todo) fx_apply(vdp_framebuffer, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h);
        if (menu_open) {
            static u32 menu_fb[VDP_FB_STRIDE * 240];
            memcpy(menu_fb, vdp_framebuffer, sizeof menu_fb);
            menu_draw(menu_fb, VDP_FB_STRIDE, vdp_screen_w, vdp_screen_h);
            SDL_UpdateTexture(tex, NULL, menu_fb, VDP_FB_STRIDE * 4);
        } else SDL_UpdateTexture(tex, NULL, vdp_framebuffer, VDP_FB_STRIDE * 4);
        /* the enhanced picture, in a texture sized for it */
        static SDL_Texture *etex;
        static int etw, eth, ew, eh;
        static const u32 *eout;
        if (todo || paused) eout = enhance_frame(&ew, &eh);
        if (eout && (ew != etw || eh != eth)) {
            if (etex) SDL_DestroyTexture(etex);
            etex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ew, eh);
            etw = ew; eth = eh;
        }
        const int enhanced_now = eout && etex && !menu_open;
        if (enhanced_now && todo) SDL_UpdateTexture(etex, NULL, eout, ew * 4);
        {
            SDL_Rect src = { 0, 0, vdp_screen_w, vdp_screen_h };
            SDL_Texture *show = tex;
            if (enhanced_now) { show = etex; src.w = ew; src.h = eh; }
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_RenderSetLogicalSize(ren, 0, 0);
            SDL_RenderSetViewport(ren, NULL);
            /* widescreen: TV-correct 16:9 (game width * 4/3 always fills it);
             * otherwise the original square-pixel picture */
            double aspect = be == &backend_n64 ? n64_aspects[enh_aspect] : vdp_widescreen ? 16.0 / 9.0 : pic_shape ? 4.0 / 3.0 : (double)vdp_screen_w / vdp_screen_h;
            SDL_Rect dst;
            if ((double)ow / oh > aspect) { dst.h = oh; dst.w = (int)(oh * aspect + 0.5); }
            else { dst.w = ow; dst.h = (int)(ow / aspect + 0.5); }
            dst.x = (ow - dst.w) / 2; dst.y = (oh - dst.h) / 2;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_SetTextureBlendMode(show, SDL_BLENDMODE_NONE);
            SDL_SetTextureAlphaMod(show, 255);
            SDL_SetTextureScaleMode(show, video_filter == FILTER_SMOOTH || enhanced_now ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            SDL_RenderCopy(ren, show, &src, &dst);
            if (video_filter >= FILTER_SCANLINES)
                crt_draw(ren, show, &src, &dst, src.w * vdp_screen_h / src.h, vdp_screen_h);
        }
        if (SDL_GetTicks() < osd_until || dragging) draw_osd(ren, win);
        if (speed_pct != last_speed) {
            char title[128];
            snprintf(title, sizeof title, "Genexia build %s  -  %s  -  F10 options%s", GENEXIA_BUILD, game_title, speed_pct == 100 ? "" : "  -  speed");
            if (speed_pct != 100) snprintf(title + strlen(title), sizeof title - strlen(title), " %d%%", speed_pct);
            if (runahead != 1)
                snprintf(title + strlen(title), sizeof title - strlen(title), "  -  run-ahead %d", runahead);
            SDL_SetWindowTitle(win, title);
            last_speed = speed_pct;
        }
        if (getenv("SF2_WINSHOT") && nframes == (u64)(getenv("SF2_WINSHOT_FRAME") ? atoi(getenv("SF2_WINSHOT_FRAME")) : 600)) {   /* dev: capture the composed window */
            int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_RenderSetLogicalSize(ren, 0, 0); SDL_RenderSetViewport(ren, NULL);
            u32 *px = malloc((size_t)ow * oh * 4);
            if (px && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, ow * 4) == 0) {
                FILE *f = fopen(getenv("SF2_WINSHOT"), "wb");
                if (f) { fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                    for (int i = 0; i < ow * oh; i++) { u8 c[3] = { (u8)(px[i] >> 16), (u8)(px[i] >> 8), (u8)px[i] }; fwrite(c, 1, 3, f); }
                    fclose(f); }
            }
            free(px);
        }
        SDL_RenderPresent(ren);
        {
            /* if vsync is not blocking (occluded window, no vsync), pace by timer */
            double t = (double)SDL_GetPerformanceCounter() / freq, period = 1.0 / refresh_hz;
            if (t - last_present < period * 0.7) {
                /* sleep almost all of it; spin only the last ~0.3 ms for precision */
                double until = last_present + period;
                for (;;) {
                    double left = until - (double)SDL_GetPerformanceCounter() / freq;
                    if (left <= 0) break;
                    if (left > 0.0006) { struct timespec ts = { 0, (long)((left - 0.0003) * 1e9) }; nanosleep(&ts, NULL); }
                }
                t = last_present + period;
            }
            last_present = t;
        }

    }
    if (getenv("SF2_FPS")) {
        fprintf(stderr, "refreshes by frames-run [0..3]: %ld %ld %ld %ld  rate_adj %.4f  audio queue %.0f ms  produced %.0f/s  ",
                todo_hist[0], todo_hist[1], todo_hist[2], todo_hist[3], audio_rate_adjust,
                dev ? SDL_GetQueuedAudioSize(dev) / 4.0 / HOST_RATE * 1000 : 0.0,
                produced_samples / ((double)(SDL_GetPerformanceCounter() - t_start) / freq));
        { struct rusage ru; getrusage(RUSAGE_SELF, &ru);
          double cpu_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
          fprintf(stderr, "speed %d refresh %.0f  cpu %.1f%% of one core  ", speed_pct, refresh_hz, 100.0 * cpu_s / ((double)(SDL_GetPerformanceCounter() - t_start) / freq)); }
        fprintf(stderr, "%.2f fps over %llu frames\n",
                nframes / ((double)(SDL_GetPerformanceCounter() - t_start) / freq), (unsigned long long)nframes);
    }
    settings_save();
    wav_close();
    SDL_Quit();
    return 0;
}
