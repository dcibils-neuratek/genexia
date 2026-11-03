/*
 * ps1_main.c - plays a PlayStation disc: the BIOS boots it in a window, or
 * with PS1_FAST=1 the game's program is loaded straight from the disc.
 *
 *   ps1 BIOS.bin GAME.cue
 *
 *   PS1_FRAMES=n     stop after n frames, without a window
 *   PS1_SHOTS=dir    save a picture there every PS1_EVERY frames (60)
 *   PS1_TTY=1        print what the BIOS and game print
 *   PS1_PRESS=f=hex,...  hold those pad bits for 8 frames from frame f, for tests
 *   arrows D-pad, Z Cross, X Circle, A Square, S Triangle, Q/W L1/R1, E/R L2/R2,
 *   Return Start, Tab Select; Cmd-F fullscreen
 */
#include <stdlib.h>
#include <ctype.h>
#include <zlib.h>
#include "ps1.h"

extern int ps1_frame_done;
void bus_reset(void);
extern int cpu_tty;

static int fast;
static const char *disc;

/* ---- loading the program off the disc, for a fast boot ---- */

static int iso_find(u32 dir_lba, u32 dir_len, const char *name, u32 *lba, u32 *len) {
    u8 s[2352];
    for (u32 off = 0; off < dir_len; ) {
        if (!cd_read_sector(dir_lba + off / 2048, s)) return 0;
        const u8 *d = s + 24 + off % 2048;
        if (d[0] == 0) { off = (off / 2048 + 1) * 2048; continue; }
        char n[256];
        const int nl = d[32];
        memcpy(n, d + 33, nl); n[nl] = 0;
        char *semi = strchr(n, ';'); if (semi) *semi = 0;
        if (strcasecmp(n, name) == 0) { memcpy(lba, d + 2, 4); memcpy(len, d + 10, 4); return 1; }
        off += d[0];
    }
    return 0;
}

static int iso_path(const char *path, u32 *lba, u32 *len) {
    u8 pvd[2352];
    if (!cd_read_sector(16, pvd)) return 0;
    u32 dl, dn;
    memcpy(&dl, pvd + 24 + 156 + 2, 4); memcpy(&dn, pvd + 24 + 156 + 10, 4);
    char part[256];
    while (*path == '\\' || *path == '/') path++;
    for (;;) {
        const char *e = path;
        while (*e && *e != '\\' && *e != '/') e++;
        snprintf(part, sizeof part, "%.*s", (int)(e - path), path);
        if (!iso_find(dl, dn, part, lba, len)) return 0;
        if (!*e) return 1;
        dl = *lba; dn = *len;
        path = e + 1;
    }
}

static void read_file(u32 lba, u32 len, u8 *out) {
    u8 s[2352];
    for (u32 i = 0; i < len; i += 2048) {
        cd_read_sector(lba + i / 2048, s);
        memcpy(out + i, s + 24, len - i < 2048 ? len - i : 2048);
    }
}

static void fast_boot(void) {
    cpu_hook = NULL;
    u32 lba, len;
    char boot[256] = "PSX.EXE";
    if (iso_path("SYSTEM.CNF", &lba, &len) && len < 4096) {
        char cnf[4097] = { 0 };
        read_file(lba, len, (u8 *)cnf);
        char *b = strstr(cnf, "BOOT");
        if (b && (b = strchr(b, ':'))) {
            b++;
            int n = 0;
            while (*b && *b != ';' && !isspace((unsigned char)*b) && n < 255) boot[n++] = *b++;
            boot[n] = 0;
        }
    }
    if (!iso_path(boot, &lba, &len)) { fprintf(stderr, "ps1: no %s on the disc\n", boot); return; }
    u8 *exe = malloc(len + 2048);
    read_file(lba, len, exe);
    u32 pc, gp, dst, size, sp, spn;
    memcpy(&pc, exe + 0x10, 4); memcpy(&gp, exe + 0x14, 4); memcpy(&dst, exe + 0x18, 4); memcpy(&size, exe + 0x1c, 4);
    u32 bss, bss_n;
    memcpy(&bss, exe + 0x28, 4); memcpy(&bss_n, exe + 0x2c, 4);
    memcpy(&sp, exe + 0x30, 4); memcpy(&spn, exe + 0x34, 4);
    if (size > len - 0x800) size = len - 0x800;
    memcpy(ps1_ram + (dst & 0x1fffff), exe + 0x800, size);
    if (bss_n && (bss & 0x1fffff) + bss_n <= sizeof ps1_ram) memset(ps1_ram + (bss & 0x1fffff), 0, bss_n);
    free(exe);
    cpu.pc = pc; cpu.next_pc = pc + 4;
    cpu.r[28] = gp;
    if (sp) { cpu.r[29] = cpu.r[30] = sp + spn; }
    fprintf(stderr, "ps1: started %s at %08x\n", boot, pc);
}

/* ---- pictures ---- */

static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const u8 *d, u32 n) {
    u8 h[8];
    put32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uLong crc = crc32(0, (const u8 *)type, 4);
    crc = crc32(crc, d, n);
    put32(h, (u32)crc);
    fwrite(h, 1, 4, f);
}

static void save_png(const char *path, const u32 *px, int w, int h) {
    const size_t raw_n = (size_t)(w * 3 + 1) * h;
    u8 *raw = malloc(raw_n), *z = malloc(compressBound(raw_n));
    for (int y = 0; y < h; y++) {
        u8 *r = raw + (size_t)y * (w * 3 + 1);
        *r++ = 0;
        for (int x = 0; x < w; x++) { const u32 p = px[y * w + x]; *r++ = p; *r++ = p >> 8; *r++ = p >> 16; }
    }
    uLongf zn = compressBound(raw_n);
    compress2(z, &zn, raw, raw_n, 6);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        u8 ihdr[13];
        put32(ihdr, w); put32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (u32)zn);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(z);
}

/* ---- the window ---- */

#ifndef PS1_HEADLESS
#include <SDL.h>
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;

static const struct { SDL_Scancode key; u16 button; } keys[] = {
    { SDL_SCANCODE_UP, 0x0010 }, { SDL_SCANCODE_RIGHT, 0x0020 }, { SDL_SCANCODE_DOWN, 0x0040 }, { SDL_SCANCODE_LEFT, 0x0080 },
    { SDL_SCANCODE_Z, 0x4000 }, { SDL_SCANCODE_X, 0x2000 }, { SDL_SCANCODE_A, 0x8000 }, { SDL_SCANCODE_S, 0x1000 },
    { SDL_SCANCODE_Q, 0x0400 }, { SDL_SCANCODE_W, 0x0800 }, { SDL_SCANCODE_E, 0x0100 }, { SDL_SCANCODE_R, 0x0200 },
    { SDL_SCANCODE_RETURN, 0x0008 }, { SDL_SCANCODE_TAB, 0x0001 },
};
static const struct { SDL_GameControllerButton pad; u16 button; } pad_keys[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0010 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0020 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0040 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0080 },
    { SDL_CONTROLLER_BUTTON_A, 0x4000 }, { SDL_CONTROLLER_BUTTON_B, 0x2000 }, { SDL_CONTROLLER_BUTTON_X, 0x8000 }, { SDL_CONTROLLER_BUTTON_Y, 0x1000 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0400 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0800 },
    { SDL_CONTROLLER_BUTTON_START, 0x0008 }, { SDL_CONTROLLER_BUTTON_BACK, 0x0001 },
};
static SDL_GameController *gamepad;

static void window_open(void) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    win = SDL_CreateWindow("PlayStation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, 640, 480);
    SDL_RaiseWindow(win);
}

static int window_show(const u32 *px, int w, int h) {
    if (w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
    }
    SDL_UpdateTexture(tex, NULL, px, w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);       /* stretched to 4:3, as a TV would */
    SDL_RenderPresent(ren);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_CONTROLLERDEVICEADDED && !gamepad) gamepad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f && (e.key.keysym.mod & KMOD_GUI))
            SDL_SetWindowFullscreen(win, SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    u16 b = 0;
    const Uint8 *held = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (held[keys[i].key]) b |= keys[i].button;
    if (gamepad) {
        for (size_t i = 0; i < sizeof pad_keys / sizeof *pad_keys; i++) if (SDL_GameControllerGetButton(gamepad, pad_keys[i].pad)) b |= pad_keys[i].button;
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= 0x0100;
        if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= 0x0200;
    }
    pad_buttons = b;
    return 1;
}
#endif

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: ps1 BIOS.bin GAME.cue\n"); return 1; }
    FILE *bf = fopen(argv[1], "rb");
    if (!bf || fread(ps1_bios, 1, sizeof ps1_bios, bf) != sizeof ps1_bios) { fprintf(stderr, "ps1: can't read the BIOS %s\n", argv[1]); return 1; }
    fclose(bf);
    disc = argv[2];
    if (!cd_open(disc)) { fprintf(stderr, "ps1: can't open the disc %s\n", disc); return 1; }
    fast = getenv("PS1_FAST") != NULL;
    cpu_tty = getenv("PS1_TTY") != NULL;
    const long frames = getenv("PS1_FRAMES") ? atol(getenv("PS1_FRAMES")) : 0;
    const char *shots = getenv("PS1_SHOTS");
    const int every = getenv("PS1_EVERY") ? atoi(getenv("PS1_EVERY")) : 60;

    cpu_reset();
    gpu_reset();
    cd_reset();
    bus_reset();
    if (fast) { cpu_hook_pc = 0x80030000; cpu_hook = fast_boot; }

    static u32 px[640 * 512];
#ifndef PS1_HEADLESS
    if (!frames) window_open();
    u64 t0 = SDL_GetPerformanceCounter();
    const double tick = (double)SDL_GetPerformanceFrequency() * FRAME_LINES * LINE_CYCLES / PS1_CLOCK;
#endif
    const char *press = getenv("PS1_PRESS");
    for (long f = 0; !frames || f < frames; f++) {
        if (press && frames) {
            pad_buttons = 0;
            for (const char *p = press; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
                long at; unsigned b;
                if (sscanf(p, "%ld=%x", &at, &b) == 2 && f >= at && f < at + 8) pad_buttons |= b;
            }
        }
        { extern int gpu_trace; gpu_trace = getenv("PS1_TRACE_AT") && f == atol(getenv("PS1_TRACE_AT")); }
        ps1_frame_done = 0;
        while (!ps1_frame_done) { cpu_run(); events_run(); }
        int w, h;
        const int shown = gpu_frame(px, &w, &h);
        if (shots && (f + 1) % every == 0) {
            char p[1024];
            snprintf(p, sizeof p, "%s/frame%05ld.png", shots, f + 1);
            save_png(p, px, w, h);
            if (getenv("PS1_VRAM")) {
                static u32 all[1024 * 512];
                for (int i = 0; i < 1024 * 512; i++) {
                    const u16 v = vram[i];
                    all[i] = (v & 31) << 3 | ((v >> 5) & 31) << 11 | ((v >> 10) & 31) << 19;
                }
                snprintf(p, sizeof p, "%s/vram%05ld.png", shots, f + 1);
                save_png(p, all, 1024, 512);
            }
            if (!shown) fprintf(stderr, "frame %ld: display off\n", f + 1);
        }
#ifndef PS1_HEADLESS
        if (!frames) {
            if (!window_show(px, w, h)) break;
            const double due = t0 + tick * (f + 1);
            const u64 now = SDL_GetPerformanceCounter();
            if (now < due) SDL_Delay((Uint32)((due - now) * 1000 / SDL_GetPerformanceFrequency()));
            else if (now > due + tick * 10) t0 = now - (u64)(tick * (f + 1));
        }
#endif
    }
    { extern int cd_lost; if (cd_lost) fprintf(stderr, "ps1: %d sectors lost\n", cd_lost); }
    fprintf(stderr, "ps1: stopped at pc %08x after %.1f s of console time\n", cpu.pc, (double)ps1_now / PS1_CLOCK);
    return 0;
}
