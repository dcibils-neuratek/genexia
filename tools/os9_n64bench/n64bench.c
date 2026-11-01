/*
 * n64bench.c - runs Mario 64's compact recompiled code with no RSP and no
 * drawing, pressing Start through the title, and writes how long the CPU part
 * of each 100 video frames took to n64bench.log. The real console shows 60
 * video frames a second and the game moves on every second one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n64_machine.h"
#include "m68k.h"
#include "bus.h"
#ifdef GENEXIA_OS9
#include <Multiverse.h>
pascal void Microseconds(void *t);
static double now_ms(void) { unsigned long t[2]; Microseconds(t); return (t[0] * 4294967296.0 + t[1]) / 1000.0; }
#else
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
long long r4300_native_cycles, r4300_interp_cycles;
#endif
extern long long r4300_native_cycles, r4300_interp_cycles;

/* what r4300.c and machine.c want from the rest of the program */
u8 rom[ROM_MAX]; u32 rom_size;
u16 pad_state[2];
void rsp_reset(void) {}
int  rsp_run(int c) { return c; }
void rsp_set_pc(u32 pc) {}
u32  rsp_get_pc(void) { return 0; }
void rdp_reset(void) {}
void rdp_run(u32 s, u32 e, int x) {}
void rdp_wide_frame(int d) {}
void rdp_set_hires(int n, int w) {}
void rdp_hires_resync(void) {}
int  rdp_hires_fetch(u32 o, int a, u32 *p, int w, int h) { return 0; }

void recomp_register_sm64(void);

#ifdef N64_GL
#include "f3d.h"
#define X(r, n, a) r (*n) a;
GL_FUNCTIONS
#undef X
extern void (*n64_gfx_hook)(u32);
typedef void *AGLPixelFormat, *AGLContext;
enum { AGL_NONE = 0, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5, AGL_DEPTH_SIZE = 12, AGL_ACCELERATED = 73 };
static CFragConnection *lib;
static AGLContext ctx;
static void (*aglSwapBuffers_)(AGLContext);
static double gfx_ms, idle_ms;
static int want_shot;

static void *sym(const char *name) {
    Str255 p; Ptr a = NULL; unsigned char c;
    p[0] = (unsigned char)strlen(name); memcpy(p + 1, name, p[0]);
    return FindSymbol(lib, p, &a, &c) == noErr ? a : NULL;
}

static void gfx(u32 dl) {
    const double t = now_ms();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    f3d_run(dl);
    if (want_shot) {                       /* what the card really drew */
        static unsigned char px[640 * 480 * 3];
        char name[32];
        sprintf(name, "n64shot%d.ppm", want_shot);
        want_shot = 0;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 640, 480, GL_RGB, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(name, "wb");
        fprintf(f, "P6 640 480 255\n");
        for (int y = 479; y >= 0; y--) fwrite(px + y * 640 * 3, 1, 640 * 3, f);
        fclose(f);
    }
    aglSwapBuffers_(ctx);
    gfx_ms += now_ms() - t;
}

static const char *gl_start(void) {
    static char why[80];
    Ptr mainp; Str255 err;
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    if (GetSharedLibrary("\pOpenGLLibrary", kPowerPCArch, kReferenceCFrag, &lib, &mainp, err)) return "no OpenGLLibrary";
#define X(r, n, a) if (!(n = (r (*) a)sym(#n))) { sprintf(why, "no %s", #n); return why; }
    GL_FUNCTIONS
#undef X
    AGLPixelFormat (*choose)(const void *, long, const long *) = sym("aglChoosePixelFormat");
    AGLContext (*create)(AGLPixelFormat, AGLContext) = sym("aglCreateContext");
    unsigned char (*drawable)(AGLContext, CGrafPtr) = sym("aglSetDrawable");
    unsigned char (*current)(AGLContext) = sym("aglSetCurrentContext");
    aglSwapBuffers_ = sym("aglSwapBuffers");
    static const long attr[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_ACCELERATED, AGL_NONE };
    AGLPixelFormat pf = choose(NULL, 0, attr);
    if (!pf) return "no accelerated format";
    ctx = create(pf, NULL);
    Rect r; SetRect(&r, 192, 100, 192 + 640, 100 + 480);
    WindowPtr w = NewCWindow(NULL, &r, "\pGenexia N64", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    drawable(ctx, (CGrafPtr)w);
    current(ctx);
    f3d_init();
    n64_gfx_hook = gfx;
    return NULL;
}
#endif
#define FRAMES 7200

extern u8 n64_rdram[];
static unsigned ram_sum(void) {
    unsigned h = 0;
    for (int i = 0; i < 0x800000; i += 4)
        h = (h << 1 | h >> 31) ^ ((unsigned)n64_rdram[i] << 24 | n64_rdram[i + 1] << 16 | n64_rdram[i + 2] << 8 | n64_rdram[i + 3]);
    return h;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "sm64.z64", "rb");
    FILE *log = fopen("n64bench.log", "w");
    freopen("n64bench.err", "w", stderr);
    if (!log) return 1;
    if (!f) { fprintf(log, "no sm64.z64\n"); fclose(log); return 1; }
    fseek(f, 0, SEEK_END); rom_size = (u32)ftell(f); fseek(f, 0, SEEK_SET);
    fprintf(log, "rom %u, read %u\n", rom_size, (u32)fread(rom, 1, rom_size, f));
    fflush(log); fclose(f);
#ifdef N64_GL
    const char *why = gl_start();
    if (why) { fprintf(log, "GL: %s\n", why); fclose(log); return 1; }
#endif
    n64_load();
    n64_register_game = recomp_register_sm64;
    n64_reset();
    double t0 = now_ms(), worst = 0, last = t0;
    const double start = t0;
#ifdef N64_GL
    f3d_stats_t st0 = f3d_stats;
#endif
    long long n0 = 0, i0 = 0;
    for (int fr = 1; fr <= FRAMES; fr++) {
        /* the same presses that reach the first course on the host */
        pad_state[0] = (fr >= 400 && fr < 410) || (fr >= 620 && fr < 630) || (fr >= 900 && fr < 910) || (fr >= 1200 && fr < 1210) ? PAD_START : 0;
        if (fr > 3600 && fr % 40 < 4) pad_state[0] = PAD_A;   /* through the dialog */
        if (fr > 5200) pad_state[0] = PAD_UP | (fr % 80 < 10 ? PAD_A : 0);
#ifdef N64_GL
        if (fr == 3300 || fr == 5400 || fr == 6600) want_shot = fr / 1000;
#endif
        n64_run_frame();
        double t = now_ms();
#ifdef N64_GL
        /* real speed, so it can be watched; the wait doesn't count as work */
        const double due = start + fr * 16.683;
        if (t < due) { const double w = t; while ((t = now_ms()) < due) {} idle_ms += t - w; t0 += t - w; last += t - w; }
#endif
        if (t - last > worst) worst = t - last;
        last = t;
        if (fr % 100 == 0) {
            fprintf(log, "frame %4d: %6.1f ms per video frame, worst %6.1f, native %ld M interp %ld K cycles, ram %08X\n",
                    fr, (t - t0) / 100, worst, (long)((r4300_native_cycles - n0) / 1000000), (long)((r4300_interp_cycles - i0) / 1000), 0u);
            fflush(log);
#ifdef N64_GL
            const long d = f3d_stats.frames - st0.frames;
            fprintf(log, "   %ld pictures, %.1f fps possible, gl %.1f ms each, %ld tris, %ld draws, %ld uploads each; %ld approx\n", d,
                    d * 1000.0 / (t - t0), d ? gfx_ms / d : 0.0, d ? (f3d_stats.tris - st0.tris) / d : 0, d ? (f3d_stats.draws - st0.draws) / d : 0,
                    d ? (f3d_stats.tex_uploads - st0.tex_uploads) / d : 0, f3d_stats.cc_approx);
            fflush(log);
            st0 = f3d_stats; gfx_ms = 0;
#endif
            n0 = r4300_native_cycles; i0 = r4300_interp_cycles; t0 = t; worst = 0;
        }
    }
    n64_debug_print();
    fprintf(log, "done\n");
    fclose(log);
    return 0;
}
