/*
 * gltest.c - this Mac's stand-in for the iMac: runs Mario 64's compact code
 * with RSP tasks finished at once and graphics tasks drawn by f3d.c into an
 * off-screen OpenGL 2.1 context, which has the same fixed pipeline. Saves a
 * picture every 300 frames: gltest ROM OUTBASE
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include "f3d.h"
#include "n64_machine.h"
#include "m68k.h"
#include "bus.h"

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
extern void (*n64_gfx_hook)(u32);

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double gfx_ms;
static void gfx(u32 dl) {
    double t = now_ms();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    f3d_run(dl);
    glFinish();
    gfx_ms += now_ms() - t;
}

static void shot(const char *base, int fr) {
    static unsigned char px[640 * 480 * 3];
    char name[512];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 640, 480, GL_RGB, GL_UNSIGNED_BYTE, px);
    snprintf(name, sizeof name, "%s.%05d.ppm", base, fr);
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6 640 480 255\n");
    for (int y = 479; y >= 0; y--) fwrite(px + y * 640 * 3, 1, 640 * 3, f);
    fclose(f);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    rom_size = (u32)fread(rom, 1, ROM_MAX, f);
    fclose(f);

    CGLPixelFormatAttribute at[] = { kCGLPFAAccelerated, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, 0 };
    CGLPixelFormatObj pf; GLint n;
    CGLContextObj ctx;
    if (CGLChoosePixelFormat(at, &pf, &n) || CGLCreateContext(pf, NULL, &ctx) || CGLSetCurrentContext(ctx)) { fprintf(stderr, "no GL\n"); return 1; }
    fprintf(stderr, "%s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint fb, col, dep;
    glGenFramebuffersEXT(1, &fb); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glGenRenderbuffersEXT(1, &col); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, col);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 640, 480);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, col);
    glGenRenderbuffersEXT(1, &dep); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, dep);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16, 640, 480);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, dep);
    f3d_init();

    n64_load();
    n64_register_game = recomp_register_sm64;
    n64_gfx_hook = gfx;
    n64_reset();
    for (int fr = 0; fr < 3600; fr++) {
        pad_state[0] = (fr >= 400 && fr < 410) || (fr >= 620 && fr < 630) || (fr >= 900 && fr < 910) || (fr >= 1200 && fr < 1210) ? PAD_START : 0;
        n64_run_frame();
        if (fr % 300 == 0) shot(argv[2], fr);
    }
    n64_debug_print();
    fprintf(stderr, "f3d: %ld frames, %ld draws, %ld tris, %ld uploads, %ld approx; %.2f ms a frame\n", f3d_stats.frames,
            f3d_stats.draws, f3d_stats.tris, f3d_stats.tex_uploads, f3d_stats.cc_approx, gfx_ms / f3d_stats.frames);
    return 0;
}
