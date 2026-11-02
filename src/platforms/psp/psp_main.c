/*
 * psp_main.c - a packaged SNES game on the Sony PSP: game.rom beside the
 * EBOOT, the game's native code linked in, and the SNES chips from backends/.
 * Saves go beside the EBOOT, in game.srm. Timing goes to stats.txt, so a run
 * on a real PSP can be checked afterwards.
 */
#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include "snes.h"
#include "machine.h"
#include "m68k.h"
#include "bus.h"

PSP_MODULE_INFO("Genexia", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

u8 rom[ROM_MAX];
u32 rom_size;
u32 vdp_framebuffer[VDP_FB_STRIDE * 240];
int vdp_screen_w = 256, vdp_screen_h = 224, vdp_native_w = 256, vdp_skip_render;
int vdp_no_sprite_limit, vdp_widescreen;
u16 pad_state[2];
void genexia_game(const SnesEnv *e);

static volatile int running = 1;
static char dir[256];

static int exit_callback(int a, int b, void *c) { running = 0; return 0; }
static int callback_thread(SceSize n, void *p) {
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_callback, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

/* ---------------------------------------------------------------- picture */
#define BUF_W 512
#define SCREEN_H 272
static u16 __attribute__((aligned(64))) tex[BUF_W * 256];
static unsigned int __attribute__((aligned(64))) list[4096];
struct Vertex { u16 u, v; s16 x, y, z; };

static void video_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_W);
    sceGuDispBuffer(480, SCREEN_H, (void *)(BUF_W * SCREEN_H * 2), BUF_W);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, SCREEN_H);
    sceGuScissor(0, 0, 480, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClearColor(0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void video_draw(void) {
    const int w = vdp_screen_w, h = vdp_screen_h > 240 ? 240 : vdp_screen_h;
    for (int y = 0; y < h; y++) {
        const u32 *in = vdp_framebuffer + y * VDP_FB_STRIDE;
        u16 *out = tex + y * BUF_W;
        for (int x = 0; x < w; x++) {
            const u32 c = in[x];
            out[x] = (c << 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 19 & 0x001F);     /* the GE's 5650 has red in the low bits */
        }
    }
    sceKernelDcacheWritebackRange(tex, sizeof tex);
    sceGuStart(GU_DIRECT, list);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, BUF_W, 256, BUF_W, tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexFlush();
    struct Vertex *v = sceGuGetMemory(2 * sizeof *v);
    const int dw = SCREEN_H * 4 / 3, left = (480 - dw) / 2;    /* the console's 4:3 */
    v[0] = (struct Vertex){ 0, 0, left, 0, 0 };
    v[1] = (struct Vertex){ w, h, left + dw, SCREEN_H, 0 };
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuSwapBuffers();
}

/* ------------------------------------------------------------------ sound */
#define AUDIO_FRAMES 1024
#define RING 8192
static s16 ring[RING * 2];
static volatile u32 ring_r, ring_w;
static int audio_thread(SceSize n, void *p) {
    static s16 out[2][AUDIO_FRAMES * 2];
    int which = 0;
    sceAudioSRCChReserve(AUDIO_FRAMES, SNES_DSP_RATE, 2);
    while (running) {
        s16 *b = out[which ^= 1];
        for (int i = 0; i < AUDIO_FRAMES; i++) {
            if (ring_r != ring_w) { b[i * 2] = ring[ring_r % RING * 2]; b[i * 2 + 1] = ring[ring_r % RING * 2 + 1]; ring_r++; }
            else b[i * 2] = b[i * 2 + 1] = 0;
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, b);
    }
    sceAudioSRCChRelease();
    return 0;
}
static void audio_frame(void) {
    static s16 s[1024 * 2];
    const int n = snes_apu_take(s, 1024);
    for (int i = 0; i < n; i++) {
        if (ring_w - ring_r >= 4096) break;   /* the screen is a touch slower than the SNES: don't let sound lag */
        for (int c = 0; c < 2; c++) {
            int v = s[i * 2 + c] * 9 / 5;      /* the same level as the desktop */
            ring[ring_w % RING * 2 + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        ring_w++;
    }
}

/* ------------------------------------------------------------------ input */
static void read_pad(void) {
    static const struct { u32 psp; u16 pad; } map[] = {
        { PSP_CTRL_UP, PAD_UP }, { PSP_CTRL_DOWN, PAD_DOWN }, { PSP_CTRL_LEFT, PAD_LEFT }, { PSP_CTRL_RIGHT, PAD_RIGHT },
        { PSP_CTRL_CROSS, PAD_A }, { PSP_CTRL_CIRCLE, PAD_B }, { PSP_CTRL_SQUARE, PAD_X }, { PSP_CTRL_TRIANGLE, PAD_Y },
        { PSP_CTRL_LTRIGGER, PAD_C }, { PSP_CTRL_RTRIGGER, PAD_Z }, { PSP_CTRL_START, PAD_START }, { PSP_CTRL_SELECT, PAD_MODE },
    };
    SceCtrlData d;
    sceCtrlPeekBufferPositive(&d, 1);
    u16 b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (d.Buttons & map[i].psp) b |= map[i].pad;
    if (d.Lx < 64) b |= PAD_LEFT; else if (d.Lx > 192) b |= PAD_RIGHT;
    if (d.Ly < 64) b |= PAD_UP; else if (d.Ly > 192) b |= PAD_DOWN;
    pad_state[0] = b;
}

/* ------------------------------------------------------------------ files */
static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s%s", dir, name); }
static int read_file(const char *name, u8 *to, u32 max) {
    char p[300]; path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    const int n = (int)fread(to, 1, max, f);
    fclose(f);
    return n;
}
static void write_file(const char *name, const void *from, u32 size) {
    char p[300]; path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(from, 1, size, f); fclose(f); }
}

static void save_battery(void) {
    static u8 saved[0x20000];
    u32 size; u8 *s = snes_sram(&size);
    if (!s || !size || size > sizeof saved || !memcmp(saved, s, size)) return;
    memcpy(saved, s, size);
    write_file("game.srm", s, size);
}

static u64 ticks(void) { u64 t; sceRtcGetCurrentTick(&t); return t; }
static u64 t_game, t_game_skip, t_video;

static void write_stats(u32 frames, u32 skipped, u64 ticks) {
    char t[220];
    const u32 tps = sceRtcGetTickResolution();
    const u32 total = snes_native_cycles + snes_interp_cycles;
    const int n = snprintf(t, sizeof t, "frames %u\nskipped %u\nseconds %u\nnative %u%%\nus per frame: game %u, game without picture %u, draw %u\n",
                           (unsigned)frames, (unsigned)skipped, (unsigned)(ticks / tps), total ? (unsigned)((u64)snes_native_cycles * 100 / total) : 0,
                           (unsigned)(t_game / (frames - skipped + !(frames - skipped))), (unsigned)(t_game_skip / (skipped + !skipped)),
                           (unsigned)(t_video / (frames - skipped + !(frames - skipped))));
    write_file("stats.txt", t, n);
}

int main(int argc, char **argv) {
    scePowerSetClockFrequency(333, 333, 166);
    const int th = sceKernelCreateThread("exit", callback_thread, 0x11, 0x1000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    snprintf(dir, sizeof dir, "%s", argc ? argv[0] : "");
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;

    rom_size = read_file("game.rom", rom, ROM_MAX);
    if (!rom_size) { sceKernelExitGame(); return 0; }
    snes_load();
    snes_game_lib = genexia_game;
    snes_reset();
    { u32 size; u8 *s = snes_sram(&size); if (s && size) read_file("game.srm", s, size); }

    video_init();
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    const int at = sceKernelCreateThread("audio", audio_thread, 0x12, 0x4000, THREAD_ATTR_USER, NULL);
    if (at >= 0) sceKernelStartThread(at, 0, NULL);

    u64 start, now;
    sceRtcGetCurrentTick(&start);
    u32 frames = 0, skipped = 0, shown = sceDisplayGetVcount();
    while (running) {
        read_pad();
        const int late = sceDisplayGetVcount() - shown > 1;   /* behind: run the game, skip the picture */
        vdp_skip_render = late;
        const u64 t0 = ticks();
        snes_run_frame();
        const u64 t1 = ticks();
        audio_frame();
        frames++;
        if (late) { skipped++; shown++; t_game_skip += t1 - t0; }
        else {
            t_game += t1 - t0;
            video_draw();
            t_video += ticks() - t1;
            while (sceDisplayGetVcount() == shown) sceDisplayWaitVblankStart();   /* one game frame for each screen refresh */
            shown++;
        }
        if (frames % 600 == 0) { sceRtcGetCurrentTick(&now); save_battery(); write_stats(frames, skipped, now - start); }
    }
    save_battery();
    sceRtcGetCurrentTick(&now);
    write_stats(frames, skipped, now - start);
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
