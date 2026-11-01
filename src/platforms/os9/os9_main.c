/*
 * os9_main.c - a small front end for Mac OS 9 on PowerPC (Retro68).
 *
 * Runs the same Genesis core and recompiled game as the modern app, without
 * SDL or enhancements: Toolbox window, Sound Manager and the keyboard. The ROM
 * is read from a file called sf2.bin next to the application.
 *
 * Keys: arrows, Z X C = A B C, A S D = X Y Z, Return = Start, Esc = quit,
 * F1 = draw every other line (faster, looks like scanlines), F2 = full screen
 * at 640 x 480, F3 = set up a gamepad with InputSprocket.
 *
 * DrawSprocket and InputSprocket are found at run time, so the game still runs
 * without them.
 * The title bar shows game and drawn frames per second, and the share of time
 * in the emulation and in drawing: how to judge a machine. The same numbers go
 * to genexia.log next to the application every second. The file is opened and
 * closed each time, so File Sharing can read it while the game runs.
 */
#include <Multiverse.h>
#include <stdio.h>
#include <string.h>
#include "m68k.h"
#include "bus.h"
#include "version.h"

#define FB_W 320
#define FB_H 224
#define OUT_RATE 22050
#define SND_FRAMES 1024              /* per buffer; two of them */
#define RING 8192                    /* stereo frames, a power of two */
#define FRAME_SAMPLES 1100

/* ------------------------------------------------------------- timing */
static s64 line_start_cyc, z80_now68;
int sys_line_cycle(void) {
    s64 d = cpu.cyc - line_start_cyc;
    if (d < 0) d = 0;
    if (d >= M68K_CYCLES_PER_LINE) d = M68K_CYCLES_PER_LINE - 1;
    return (int)d;
}

/* -------------------------------------------------------------- sound */
static s16 ym_buf[FRAME_SAMPLES * 2], psg_buf[FRAME_SAMPLES];
static int aud_n;
static s64 aud_cyc;
static s16 ring[RING * 2];
static volatile u32 ring_w, ring_r;
static u32 rs_pos;                   /* 16.16 position in the native samples */
static const u32 rs_step = (u32)(((u64)YM_SAMPLE_RATE << 16) / OUT_RATE);

static void audio_render_to(s64 t) {
    s64 n = (t - aud_cyc) / 144;
    if (n <= 0) return;
    if (aud_n + n > FRAME_SAMPLES) n = FRAME_SAMPLES - aud_n;
    if (n > 0) {
        ym2612_render(ym_buf + aud_n * 2, (int)n);
        psg_render(psg_buf + aud_n, (int)n);
        aud_n += (int)n;
    }
    aud_cyc += ((t - aud_cyc) / 144) * 144;
}
void audio_sync_68k(void) { audio_render_to(cpu.cyc); }
void audio_sync_z80(void) { audio_render_to(z80_now68); }

static void audio_flush(void) {
    u32 w = ring_w;
    while ((int)(rs_pos >> 16) < aud_n) {
        const int i = (int)(rs_pos >> 16);
        if (w - ring_r < RING) {
            for (int ch = 0; ch < 2; ch++) {
                int v = (ym_buf[i * 2 + ch] + psg_buf[i]) * 4;
                ring[(w & (RING - 1)) * 2 + ch] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            w++;
        }
        rs_pos += rs_step;
    }
    rs_pos -= (u32)aud_n << 16;
    aud_n = 0;
    ring_w = w;
}

/* Runs at interrupt time: touches only the ring and the buffer it is given. */
static pascal void snd_back(SndChannelPtr chan, SndDoubleBufferPtr buf) {
    s16 *out = (s16 *)buf->dbSoundData;
    u32 r = ring_r;
    for (int i = 0; i < SND_FRAMES; i++) {
        if (r != ring_w) { out[i * 2] = ring[(r & (RING - 1)) * 2]; out[i * 2 + 1] = ring[(r & (RING - 1)) * 2 + 1]; r++; }
        else { out[i * 2] = 0; out[i * 2 + 1] = 0; }
    }
    ring_r = r;
    buf->dbNumFrames = SND_FRAMES;
    buf->dbFlags |= dbBufferReady;
}

static SndChannelPtr snd_chan;
static SndDoubleBufferHeader snd_hdr;
static void sound_start(void) {
    if (SndNewChannel(&snd_chan, sampledSynth, 0x00C0 /* initStereo */, NULL) != noErr) { snd_chan = NULL; return; }
    snd_hdr.dbhNumChannels = 2;
    snd_hdr.dbhSampleSize = 16;
    snd_hdr.dbhCompressionID = 0;
    snd_hdr.dbhPacketSize = 0;
    snd_hdr.dbhSampleRate = (UnsignedFixed)((u32)OUT_RATE << 16);
    snd_hdr.dbhDoubleBack = NewSndDoubleBackUPP(snd_back);
    for (int i = 0; i < 2; i++) {
        SndDoubleBufferPtr b = (SndDoubleBufferPtr)NewPtrClear(sizeof(SndDoubleBuffer) + SND_FRAMES * 4);
        b->dbNumFrames = SND_FRAMES;
        b->dbFlags = dbBufferReady;
        snd_hdr.dbhBufferPtr[i] = b;
    }
    if (SndPlayDoubleBuffer(snd_chan, &snd_hdr) != noErr) { SndDisposeChannel(snd_chan, true); snd_chan = NULL; }
}

/* -------------------------------------------------------------- frame */
static void run_frame(void) {
    static s64 z80_debt, sched = -1;
    if (sched < 0 || cpu.cyc - sched > 4 * M68K_CYCLES_PER_LINE) sched = cpu.cyc;
    for (int line = 0; line < LINES_PER_FRAME; line++) {
        vdp_begin_line(line);
        z80_set_irq(vdp_zint_pending());
        line_start_cyc = sched;
        const s64 line_end = sched + M68K_CYCLES_PER_LINE;
        const s64 from = cpu.cyc < line_end ? cpu.cyc : line_end - 1;
        m68k_run(line_end);
        z80_debt += Z80_CYCLES_PER_LINE;
        if (!z80_busreq && !z80_reset_line) {
            const s64 total = z80_debt;
            s64 done = 0;
            while (z80_debt > 0) {
                z80_now68 = from + (line_end - from) * done / (total ? total : 1);
                const int ran = z80_execute(z80_debt < 64 ? (int)z80_debt : 64);
                z80_debt -= ran;
                done += ran;
            }
        } else {
            z80_debt = 0;
        }
        z80_now68 = line_end;
        sched = line_end;
        audio_render_to(sched);
    }
    audio_flush();
}

/* -------------------------------------------------------------- input */
static int isp_on;
static u16 isp_read(void);
static int key_down(const unsigned long *km, int code) {
    return (((const u8 *)km)[code >> 3] >> (code & 7)) & 1;
}
static int read_keys(void) {
    static const struct { u8 code; u16 pad; } map[] = {
        { 0x7E, PAD_UP }, { 0x7D, PAD_DOWN }, { 0x7B, PAD_LEFT }, { 0x7C, PAD_RIGHT },
        { 0x06, PAD_A }, { 0x07, PAD_B }, { 0x08, PAD_C },
        { 0x00, PAD_X }, { 0x01, PAD_Y }, { 0x02, PAD_Z },
        { 0x24, PAD_START }, { 0x30, PAD_MODE },
    };
    unsigned long km[4];
    GetKeys((void *)km);
    u16 b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (key_down(km, map[i].code)) b |= map[i].pad;
    if (isp_on) b |= isp_read();
    pad_state[0] = b;
    return key_down(km, 0x35);       /* Esc */
}

/* -------------------------------------------------------------- video */
static WindowPtr win;
static GWorldPtr gw;
static Rect pic, gw_rect;

static int in_front = 1, every_other, video_path;   /* video_path: last screen depth drawn to, 0 = system copy */

static inline u32 timebase(void) { u32 t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }

/* Straight into video memory, when the whole picture is on the main screen at
 * 16 or 32 bits and nothing can cover it. Lines that did not change are left
 * alone, and the rest go out 8 bytes at a time: the video bus takes those as
 * bursts, about twice as fast as single words. Floating-point stores must be
 * aligned there. Every second line is left out in every_other mode. Returns 0
 * if the system has to do it instead. */
static CGrafPtr dsp_port;           /* the screen, while DrawSprocket owns it */
static u32 last_frame[FB_H][FB_W];
static int last_valid;
static Point last_at;

static int show_direct(void) {
    PixMapHandle pm;
    Point o;
    if (dsp_port) {
        pm = dsp_port->portPixMap;
        o.v = (short)((**pm).bounds.top + (480 - FB_H * 2) / 2);
        o.h = (**pm).bounds.left;
    } else {
        if (!in_front || FrontWindow() != win) return 0;
        GDHandle gd = GetMainDevice();
        pm = (**gd).gdPMap;
        o.v = pic.top; o.h = pic.left;
        SetPort((GrafPtr)win);
        LocalToGlobal(&o);
        const Rect sr = (**gd).gdRect;
        if (o.h < sr.left || o.v < sr.top + GetMBarHeight() || o.h + FB_W * 2 > sr.right || o.v + FB_H * 2 > sr.bottom) return 0;
    }
    const int depth = (**pm).pixelSize;
    if (depth != 32 && depth != 16) return 0;
    if (!dsp_port) {
        Rect r = { o.v, o.h, (short)(o.v + FB_H * 2), (short)(o.h + FB_W * 2) };
        const Point none = { 0, 0 };
        ShieldCursor(&r, none);
    }
    if (o.v != last_at.v || o.h != last_at.h) last_valid = 0;
    last_at = o;
    static union { double d[FB_W + 1]; u32 w[FB_W * 2 + 2]; u16 h[FB_W * 2 + 4]; } buf;
    const int row = (**pm).rowBytes & 0x3FFF;
    u8 *line = (u8 *)(**pm).baseAddr + (size_t)(o.v - (**pm).bounds.top) * row + (size_t)(o.h - (**pm).bounds.left) * (depth / 8);
    const int bytes = FB_W * 2 * (depth / 8), aligned = !((size_t)line & 3) && !(row & 7);
    for (int y = 0; y < FB_H; y++, line += row * 2) {
        const u32 *src = &vdp_framebuffer[y * VDP_FB_STRIDE];
        if (last_valid && !memcmp(last_frame[y], src, FB_W * 4)) continue;
        memcpy(last_frame[y], src, FB_W * 4);
        const int skew = aligned ? (int)((size_t)line & 7) / 4 : 0;
        if (depth == 32) {
            u32 *p = buf.w + skew;
            for (int x = 0; x < FB_W; x++, p += 2) p[0] = p[1] = src[x];
        } else {
            u32 *p = buf.w + skew;
            for (int x = 0; x < FB_W; x++) {
                const u32 c = src[x], q = (c >> 9 & 0x7C00) | (c >> 6 & 0x3E0) | (c >> 3 & 0x1F);
                *p++ = q << 16 | q;
            }
        }
        for (int rep = 0; rep < (every_other ? 1 : 2); rep++) {
            volatile u8 *d = line + (size_t)rep * row;
            const u8 *from = (const u8 *)buf.w + skew * 4;
            int left = bytes;
            if (!aligned) {              /* 16-bit picture on an odd pixel */
                for (int i = 0; i < left; i += 2) *(volatile u16 *)(d + i) = *(const u16 *)(from + i);
                continue;
            }
            if (skew) { *(volatile u32 *)d = *(const u32 *)from; d += 4; from += 4; left -= 4; }
            for (; left >= 8; left -= 8, d += 8, from += 8) *(volatile double *)d = *(const double *)from;
            if (left >= 4) *(volatile u32 *)d = *(const u32 *)from;
        }
    }
    last_valid = 1;
    if (!dsp_port) ShowCursor();
    video_path = depth;
    return 1;
}

static void show_frame(void) {
    if (show_direct()) return;
    video_path = 0;
    PixMapHandle pm = GetGWorldPixMap(gw);
    LockPixels(pm);
    u8 *base = (u8 *)GetPixBaseAddr(pm);
    const int row = (**pm).rowBytes & 0x3FFF;
    for (int y = 0; y < FB_H; y++) {
        const u32 *src = &vdp_framebuffer[y * VDP_FB_STRIDE];
        u32 *a = (u32 *)(base + (size_t)y * 2 * row);
        for (int x = 0; x < FB_W; x++) a[x * 2] = a[x * 2 + 1] = src[x];
        memcpy(base + ((size_t)y * 2 + 1) * row, a, FB_W * 2 * 4);
    }
    SetPort((GrafPtr)win);
    CopyBits((BitMap *)*pm, &((GrafPtr)win)->portBits, &gw_rect, &pic, srcCopy, NULL);
    UnlockPixels(pm);
    last_valid = 0;
}

/* The window, or the whole screen with the menu bar out of the way. */
static int full_screen;
static RgnHandle gray_saved;
static short mbar_saved;

static void black_window(void) {
    static const RGBColor black = { 0, 0, 0 };
    SetPort((GrafPtr)win);
    RGBBackColor(&black);
    EraseRect(&((GrafPtr)win)->portRect);
    last_valid = 0;
}

static void menu_bar_show(int show) {
    RgnHandle gray = GetGrayRgn();
    if (show) {
        if (!gray_saved) return;
        LMSetMBarHeight(mbar_saved);
        CopyRgn(gray_saved, gray);
        DisposeRgn(gray_saved);
        gray_saved = NULL;
        DrawMenuBar();
    } else {
        gray_saved = NewRgn();
        CopyRgn(gray, gray_saved);
        mbar_saved = GetMBarHeight();
        RgnHandle all = NewRgn();
        RectRgn(all, &qd.screenBits.bounds);
        UnionRgn(gray, all, gray);
        DisposeRgn(all);
        LMSetMBarHeight(0);
    }
}

static void open_window(int full) {
    if (win) DisposeWindow(win);
    menu_bar_show(!full);
    const Rect sb = qd.screenBits.bounds;
    Rect bounds;
    if (full) {
        bounds = sb;
        win = NewCWindow(NULL, &bounds, "\pGenexia", true, plainDBox, (WindowPtr)-1, false, 0);
        const short x = (short)(((sb.right - sb.left - FB_W * 2) / 2) & ~3), yy = (short)((sb.bottom - sb.top - FB_H * 2) / 2);
        SetRect(&pic, x, yy, (short)(x + FB_W * 2), (short)(yy + FB_H * 2));
    } else {
        SetRect(&bounds, 0, 0, FB_W * 2, FB_H * 2);
        pic = bounds;
        OffsetRect(&bounds, (short)(((sb.right - FB_W * 2) / 2) & ~3), (short)((sb.bottom - FB_H * 2) / 2 + 10));
        win = NewCWindow(NULL, &bounds, "\pGenexia build " GENEXIA_BUILD, true, noGrowDocProc, (WindowPtr)-1, false, 0);
    }
    full_screen = full;
    black_window();
}

static void fail(ConstStr255Param msg) {
    SetPort((GrafPtr)win);
    MoveTo(20, 40);
    DrawString(msg);
    while (!Button()) { }
    ExitToShell();
}

static void log_line(const char *t) {
    FILE *f = fopen("genexia.log", "a");
    if (!f) return;
    fputs(t, f);
    fputc('\r', f);
    fclose(f);
}

/* ------------------------------------------------------- InputSprocket
 * Declared here from Apple's definitions; functions come from the system's
 * InputSprocketLib, so building needs no SDK. */
typedef struct {
    unsigned char name[64];
    short icon;
    u8 player, group;
    u32 kind, label, flags, reserved[3];
} IspNeed;
typedef void *IspRef;
enum { N_DPAD, N_A, N_B, N_C, N_X, N_Y, N_Z, N_START, N_MODE, N_COUNT };
#define NEED(n, k, l) { n, 0, 1, 0, k, l, 4 /* polled */, { 0, 0, 0 } }
#define BTN(n) NEED(n, 'butn', 'none')
static IspNeed isp_needs[N_COUNT] = {
    NEED("\pMove", 'dpad', 'move'), BTN("\pA"), BTN("\pB"), BTN("\pC"),
    BTN("\pX"), BTN("\pY"), BTN("\pZ"), BTN("\pStart"), BTN("\pMode"),
};
static IspRef isp_refs[N_COUNT];

typedef OSStatus (*Isp0)(void);
static Isp0 isp_startup, isp_shutdown, isp_stop, isp_suspend, isp_resume, isp_tickle;
static OSStatus (*isp_virtual)(u32, IspNeed *, IspRef *, u32);
static OSStatus (*isp_init)(u32, IspNeed *, IspRef *, u32, u32, u32, short, u32);
static OSStatus (*isp_configure)(void *);
static OSStatus (*isp_extract)(u32, u32 *, IspRef *);
static OSStatus (*isp_activate)(u32, IspRef *);
static OSStatus (*isp_definition)(IspRef, u32, void *);
static OSStatus (*isp_state)(IspRef, u32 *);

static void *isp_find(ConnectionID lib, ConstStr255Param name) {
    Ptr p = NULL;
    SymClass c;
    return FindSymbol(lib, name, &p, &c) == noErr ? p : NULL;
}

static void isp_log(const char *what, long v) {
    char l[100];
    snprintf(l, sizeof l, "InputSprocket: %s %ld", what, v);
    log_line(l);
}

static void isp_open(void) {
    ConnectionID lib;
    Ptr entry;
    Str255 why;
    const OSErr e = GetSharedLibrary("\pInputSprocketLib", 'pwpc', 1, &lib, &entry, why);
    if (e != noErr) { isp_log("library not found, error", e); return; }
    isp_startup = isp_find(lib, "\pISpStartup");
    isp_shutdown = isp_find(lib, "\pISpShutdown");
    isp_stop = isp_find(lib, "\pISpStop");
    isp_suspend = isp_find(lib, "\pISpSuspend");
    isp_resume = isp_find(lib, "\pISpResume");
    isp_tickle = isp_find(lib, "\pISpTickle");
    isp_virtual = isp_find(lib, "\pISpElement_NewVirtualFromNeeds");
    isp_init = isp_find(lib, "\pISpInit");
    isp_configure = isp_find(lib, "\pISpConfigure");
    isp_extract = isp_find(lib, "\pISpDevices_Extract");
    isp_activate = isp_find(lib, "\pISpDevices_Activate");
    isp_definition = isp_find(lib, "\pISpDevice_GetDefinition");
    isp_state = isp_find(lib, "\pISpElement_GetSimpleState");
    if (!isp_virtual || !isp_init || !isp_state || !isp_extract || !isp_activate || !isp_definition) {
        isp_log("functions missing in this version", 0);
        return;
    }
    if (isp_startup) { const OSStatus s = isp_startup(); isp_log("startup", s); }
    OSStatus s = isp_virtual(N_COUNT, isp_needs, isp_refs, 0);
    isp_log("virtual elements", s);
    if (s != noErr) return;
    s = isp_init(N_COUNT, isp_needs, isp_refs, 'GNXA', '0001', 0, 0, 0);
    isp_log("init", s);
    if (s != noErr) return;

    /* every game controller, but not the keyboard and mouse the game needs */
    static IspRef dev[32];
    u32 n = 0, wanted = 0;
    s = isp_extract(32, &n, dev);
    isp_log("devices", (long)n);
    if (n > 32) n = 32;
    for (u32 i = 0; i < n; i++) {
        static u8 def[512];
        char l[120];
        if (isp_definition(dev[i], sizeof def, def) != noErr) continue;
        const u32 cls = (u32)def[64] << 24 | def[65] << 16 | def[66] << 8 | def[67];
        snprintf(l, sizeof l, "InputSprocket: device %.*s class %.4s", def[0], def + 1, def + 64);
        log_line(l);
        if (cls != 'keyd' && cls != 'mous') dev[wanted++] = dev[i];
    }
    if (wanted) { s = isp_activate(wanted, dev); isp_log("activated", (long)wanted); }
    isp_on = 1;
}

/* ------------------------------------------------------- DrawSprocket */
typedef struct {
    u32 frequency, width, height, reserved1, reserved2, color_needs;
    void *color_table;
    u32 options, back_depths, display_depths, back_best, display_best, pages;
    char filler[3];
    unsigned char must_confirm;
    u32 reserved3[4];
} DspAttrs;
static void *dsp_ctx;
static OSStatus (*dsp_startup)(void);
static OSStatus (*dsp_best)(DspAttrs *, void **);
static OSStatus (*dsp_reserve)(void *, DspAttrs *);
static OSStatus (*dsp_release)(void *);
static OSStatus (*dsp_state)(void *, u32);
static OSStatus (*dsp_front)(void *, CGrafPtr *);
static OSStatus (*dsp_event)(EventRecord *, unsigned char *);

static void dsp_open(void) {
    ConnectionID lib;
    Ptr entry;
    Str255 why;
    const OSErr e = GetSharedLibrary("\pDrawSprocketLib", 'pwpc', 1, &lib, &entry, why);
    if (e != noErr) { snprintf((char *)why, sizeof why, "DrawSprocket: library not found, error %d", e); log_line((char *)why); return; }
    dsp_startup = isp_find(lib, "\pDSpStartup");
    dsp_best = isp_find(lib, "\pDSpFindBestContext");
    dsp_reserve = isp_find(lib, "\pDSpContext_Reserve");
    dsp_release = isp_find(lib, "\pDSpContext_Release");
    dsp_state = isp_find(lib, "\pDSpContext_SetState");
    dsp_front = isp_find(lib, "\pDSpContext_GetFrontBuffer");
    dsp_event = isp_find(lib, "\pDSpProcessEvent");
    if (!dsp_startup || !dsp_best || !dsp_reserve || !dsp_release || !dsp_state || !dsp_front) { dsp_best = NULL; return; }
    char l[80];
    snprintf(l, sizeof l, "DrawSprocket: startup %ld", (long)dsp_startup());
    log_line(l);
}

static void dsp_clear(void) {
    PixMapHandle pm = dsp_port->portPixMap;
    const int row = (**pm).rowBytes & 0x3FFF;
    for (int y = 0; y < (**pm).bounds.bottom - (**pm).bounds.top; y++) {
        volatile u32 *d = (volatile u32 *)((**pm).baseAddr + (size_t)y * row);
        for (int x = 0; x < row / 4; x++) d[x] = 0;
    }
    last_valid = 0;
}

static int dsp_enter(void) {
    if (!dsp_best) return 0;
    DspAttrs a;
    memset(&a, 0, sizeof a);
    a.width = 640; a.height = 480;
    a.color_needs = 2;              /* require */
    a.back_depths = a.display_depths = 1u << 5 | 1u << 4;
    a.back_best = a.display_best = 32;
    a.pages = 1;
    char l[100];
    OSStatus s = dsp_best(&a, &dsp_ctx);
    snprintf(l, sizeof l, "DrawSprocket: find 640x480 %ld", (long)s);
    log_line(l);
    if (s != noErr) return 0;
    s = dsp_reserve(dsp_ctx, &a);
    if (s == noErr) s = dsp_state(dsp_ctx, 0);
    if (s == noErr) s = dsp_front(dsp_ctx, &dsp_port);
    snprintf(l, sizeof l, "DrawSprocket: active %ld", (long)s);
    log_line(l);
    if (s != noErr || !dsp_port) { dsp_port = NULL; dsp_release(dsp_ctx); return 0; }
    PixMapHandle pm = dsp_port->portPixMap;
    snprintf(l, sizeof l, "DrawSprocket: screen %d x %d, %d bits", (**pm).bounds.right - (**pm).bounds.left,
             (**pm).bounds.bottom - (**pm).bounds.top, (**pm).pixelSize);
    log_line(l);
    dsp_clear();
    HideCursor();
    last_valid = 0;
    return 1;
}

static void dsp_leave(void) {
    if (!dsp_port) return;
    dsp_port = NULL;
    dsp_state(dsp_ctx, 2);          /* inactive */
    dsp_release(dsp_ctx);
    ShowCursor();
    black_window();
}

static u16 isp_read(void) {
    static const u16 pads[9] = {
        0, PAD_LEFT, PAD_UP | PAD_LEFT, PAD_UP, PAD_UP | PAD_RIGHT,
        PAD_RIGHT, PAD_DOWN | PAD_RIGHT, PAD_DOWN, PAD_DOWN | PAD_LEFT,
    };
    static const u16 buttons[N_COUNT] = { 0, PAD_A, PAD_B, PAD_C, PAD_X, PAD_Y, PAD_Z, PAD_START, PAD_MODE };
    if (isp_tickle) isp_tickle();
    u32 v = 0;
    u16 b = 0;
    if (isp_state(isp_refs[N_DPAD], &v) == noErr && v < 9) b = pads[v];
    for (int i = N_A; i < N_COUNT; i++)
        if (isp_state(isp_refs[i], &v) == noErr && v) b |= buttons[i];
    return b;
}

static void isp_close(void) {
    if (!isp_on) return;
    if (isp_stop) isp_stop();
    if (isp_shutdown) isp_shutdown();
}

/* --------------------------------------------------------------- main */
int main(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    SetRect(&gw_rect, 0, 0, FB_W * 2, FB_H * 2);
    open_window(0);

    FILE *f = fopen("sf2.bin", "rb");
    if (!f) fail("\pPut the ROM next to this application as sf2.bin, then restart.");
    rom_size = (u32)fread(rom, 1, sizeof rom, f);
    fclose(f);
    if (NewGWorld(&gw, 32, &gw_rect, NULL, NULL, 0) != noErr) fail("\pNot enough memory for the picture.");

    bus_reset();
    vdp_init();
    z80_reset();
    ym2612_reset();
    psg_reset();
    m68k_reset();
    sound_start();
    log_line("--- Genexia build " GENEXIA_BUILD);
    isp_open();
    dsp_open();

    unsigned long next = TickCount(), mark = next;
    int shown = 0, skipped = 0, ran = 0, f1_held = 0, f2_held = 0, f3_held = 0;
    u32 t_emu = 0, t_vid = 0, t_mark = timebase();
    for (;;) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 0, NULL)) {
            unsigned char taken = 0;
            if (dsp_port && dsp_event) dsp_event(&ev, &taken);
            if (taken) { }
            else if (ev.what == mouseDown) {
                WindowPtr w;
                if (FindWindow(ev.where, &w) == inDrag) DragWindow(w, ev.where, &qd.screenBits.bounds);
            } else if (ev.what == osEvt && (ev.message >> 24 & 0xFF) == 1) {
                in_front = ev.message & 1;   /* suspend or resume */
                if (isp_on && isp_suspend && isp_resume) { if (in_front) isp_resume(); else isp_suspend(); }
                last_valid = 0;
            } else if (ev.what == updateEvt) {
                BeginUpdate((WindowPtr)ev.message);
                EndUpdate((WindowPtr)ev.message);
            }
        }
        if (read_keys()) break;
        {
            unsigned long km[4];
            GetKeys((void *)km);
            const int f1 = key_down(km, 0x7A);
            if (f1 && !f1_held) {
                every_other = !every_other;
                if (dsp_port) dsp_clear(); else black_window();
            }
            f1_held = f1;
            const int f2 = key_down(km, 0x78), f3 = key_down(km, 0x63);
            if (f2 && !f2_held) {
                if (dsp_port) dsp_leave();
                else if (full_screen) open_window(0);
                else if (!dsp_enter()) open_window(1);
                next = TickCount();
            }
            f2_held = f2;
            if (f3 && !f3_held && isp_on && isp_configure) {
                dsp_leave();
                if (full_screen) open_window(0);
                const OSStatus s = isp_configure(NULL);
                isp_log("configure", s);
                black_window();
                next = TickCount();
            }
            f3_held = f3;
        }

        /* late by more than a tick: run the game but don't draw */
        const int behind = TickCount() > next + 1 && skipped < 3;
        vdp_skip_render = behind;
        const u32 t0 = timebase();
        run_frame();
        const u32 t1 = timebase();
        t_emu += t1 - t0;
        ran++;
        if (behind) skipped++;
        else { show_frame(); t_vid += timebase() - t1; shown++; skipped = 0; }

        next++;
        /* the Sound Manager's clock is the master: don't run ahead of it */
        if (ring_w - ring_r > 2600) next++;
        while (TickCount() < next) { }
        if (TickCount() > next + 30) next = TickCount();
        if (TickCount() - mark >= 60) {
            char t[80];
            const u32 ticks = TickCount() - mark, all = timebase() - t_mark;
            const int n = snprintf(t + 1, sizeof t - 1, "Game %d  Drawn %d  CPU %d%%  Draw %d%%", (int)(ran * 60 / ticks), (int)(shown * 60 / ticks),
                                   (int)(t_emu / (all / 100 + 1)), (int)(t_vid / (all / 100 + 1)));
            t[0] = (char)n;
            if (!full_screen && !dsp_port) SetWTitle(win, (ConstStr255Param)t);
            {
                char l[120];
                snprintf(l, sizeof l, "%s  video %d%s%s  sound %s  queued %d  pad %04X  ticks %lu", t + 1, video_path,
                         every_other ? " alt-lines" : "", dsp_port ? " full640" : full_screen ? " full" : "", snd_chan ? "on" : "off",
                         (int)(ring_w - ring_r), pad_state[0], TickCount());
                log_line(l);
            }
            shown = ran = 0;
            t_emu = t_vid = 0;
            t_mark = timebase();
            mark = TickCount();
        }
    }
    dsp_leave();
    isp_close();
    if (snd_chan) SndDisposeChannel(snd_chan, true);
    if (full_screen) menu_bar_show(1);
    return 0;
}
