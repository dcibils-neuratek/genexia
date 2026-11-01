/*
 * os9_main.c - a small front end for Mac OS 9 on PowerPC (Retro68).
 *
 * Runs the same Genesis and SNES cores and recompiled games as the modern app,
 * without SDL: Toolbox window, Sound Manager and the keyboard. The game selector
 * lists every ROM next to the application or in a roms folder there.
 *
 * Keys: arrows, Z X C = A B C, A S D = X Y Z, Return = Start, Esc = quit. On a
 * SNES those are Z = B, X = A, A = Y, S = X, C = L, D = R, Tab = Select.
 * F1 = scanlines (faster), F2 = full screen at the display's own size,
 * F3 = set up a gamepad with InputSprocket, F8 = stats, F9 or F10 = options menu,
 * - and = = game speed, 0 = normal speed, or the bar: move the mouse to the
 * bottom of the picture and drag it.
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
#include "combo.h"
#include "enhance.h"
#include "machine.h"
#include "snes.h"
#include "version.h"

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
static u32 rs_step = (u32)(((u64)YM_SAMPLE_RATE << 16) / OUT_RATE);
static int snes_game, snd_rate = YM_SAMPLE_RATE, snd_gain = 4;   /* what the console puts out */
/* game speed in percent: sound follows it, like a tape running faster */
static int speed_pct = 100;
static void set_speed(int pct) {
    pct = (pct + 2) / 5 * 5;
    speed_pct = pct < 50 ? 50 : pct > 200 ? 200 : pct;
    rs_step = (u32)(((u64)snd_rate << 16) * speed_pct / 100 / OUT_RATE);
}

static inline u32 timebase(void) { u32 t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
static u32 t_vdp, t_68k, t_z80, t_ym;     /* where a frame goes, for the log */

static void audio_render_to(s64 t) {
    s64 n = (t - aud_cyc) / 144;
    if (n <= 0) return;
    const u32 t0 = timebase();
    if (aud_n + n > FRAME_SAMPLES) n = FRAME_SAMPLES - aud_n;
    if (n > 0) {
        ym2612_render(ym_buf + aud_n * 2, (int)n);
        psg_render(psg_buf + aud_n, (int)n);
        aud_n += (int)n;
    }
    aud_cyc += ((t - aud_cyc) / 144) * 144;
    t_ym += timebase() - t0;
}
void audio_sync_68k(void) { audio_render_to(cpu.cyc); }
void audio_sync_z80(void) { audio_render_to(z80_now68); }

static void audio_flush(void) {
    u32 w = ring_w;
    while ((int)(rs_pos >> 16) < aud_n) {
        const int i = (int)(rs_pos >> 16);
        if (w - ring_r < RING) {
            for (int ch = 0; ch < 2; ch++) {
                int v = (ym_buf[i * 2 + ch] + psg_buf[i]) * snd_gain;
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
    if (snes_game) {
        const u32 t0 = timebase();
        snes_run_frame();
        t_68k += timebase() - t0 - snes_t_ppu - snes_t_apu;
        t_vdp += snes_t_ppu;
        t_ym += snes_t_apu;
        snes_t_ppu = snes_t_apu = 0;
        aud_n = snes_apu_take(ym_buf, FRAME_SAMPLES);
        memset(psg_buf, 0, (size_t)aud_n * sizeof(s16));
        audio_flush();
        return;
    }
    if (sched < 0 || cpu.cyc - sched > 4 * M68K_CYCLES_PER_LINE) sched = cpu.cyc;
    for (int line = 0; line < LINES_PER_FRAME; line++) {
        const u32 t0 = timebase();
        vdp_begin_line(line);
        const u32 t1 = timebase();
        t_vdp += t1 - t0;
        z80_set_irq(vdp_zint_pending());
        line_start_cyc = sched;
        const s64 line_end = sched + M68K_CYCLES_PER_LINE;
        const s64 from = cpu.cyc < line_end ? cpu.cyc : line_end - 1;
        const u32 ym0 = t_ym;
        m68k_run(line_end);
        const u32 t2 = timebase(), ym1 = t_ym;
        t_68k += t2 - t1 - (ym1 - ym0);
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
        t_z80 += timebase() - t2 - (t_ym - ym1);
        audio_render_to(sched);
    }
    audio_flush();
}

/* -------------------------------------------------------------- input */
static int isp_on;
static u16 isp_read(void);
static unsigned long keys_now[4];
static u8 keys_held[128];
static int key_down(const unsigned long *km, int code) {
    return (((const u8 *)km)[code >> 3] >> (code & 7)) & 1;
}
static void read_keys(void) {
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
}
/* once per loop, then press() says what went down since the last one */
static void keys_poll(void) { GetKeys((void *)keys_now); }
static int press(int code) {
    const int down = key_down(keys_now, code), was = keys_held[code];
    keys_held[code] = (u8)down;
    return down && !was;
}

/* -------------------------------------------------------------- video */
static WindowPtr win;
static GWorldPtr gw;
static Rect pic, gw_rect;            /* pic: where the picture goes, inside the area */
static int area_w = 640, area_h = 480;   /* the window, or the whole screen */

static int in_front = 1, every_other, video_path;   /* video_path: last screen depth drawn to, 0 = system copy */
static int enh_upscale, enh_color, enh_shape = 1;   /* shape: 1 fills 4:3, 0 square pixels */

#define SRC_W 852                    /* widest picture: 426 columns after Scale2x */
#define SRC_H 480
#define OUT_W 1600                   /* widest screen drawn to directly */
static CGrafPtr dsp_port;            /* the screen, while DrawSprocket owns it */
static u32 last_frame[SRC_H][SRC_W];
static int last_valid;
static u8 *last_line;
static u16 x_src[OUT_W + 1];          /* picture column under each screen pixel */
static u16 x_at[SRC_W + 1];          /* first screen pixel of each picture column */
static short y_at[SRC_H + 1];        /* first screen line of each picture line */
static int map_sw, map_sh, map_dw, map_dh;
static void clear_screen(void);

/* Fit an an:ad picture into the area, and work out which screen pixels each
 * picture pixel covers. x stays on a multiple of 4 for the bursts below. */
static void layout(int sw, int sh, int an, int ad) {
    int dw = area_w, dh = dw * ad / an;
    if (dh > area_h) { dh = area_h; dw = dh * an / ad; }
    dw &= ~3;
    const short x = (short)(((area_w - dw) / 2) & ~3), y = (short)((area_h - dh) / 2);
    if (pic.left != x || pic.top != y || pic.right != x + dw || pic.bottom != y + dh) {
        SetRect(&pic, x, y, (short)(x + dw), (short)(y + dh));
        clear_screen();
    }
    if (sw == map_sw && sh == map_sh && dw == map_dw && dh == map_dh) return;
    map_sw = sw; map_sh = sh; map_dw = dw; map_dh = dh;
    for (int i = 0; i <= sw; i++) x_at[i] = (u16)(i * dw / sw);
    for (int i = 0; i < sw; i++) for (int x = x_at[i]; x < x_at[i + 1]; x++) x_src[x] = (u16)i;
    for (int i = 0; i <= sh; i++) y_at[i] = (short)(i * dh / sh);
    last_valid = 0;
}

/* Lines that did not change are left alone, and the rest go out 8 bytes at a
 * time: the video bus takes those as bursts, about twice as fast as single
 * words. Floating-point stores must be aligned there. With every_other the
 * last screen line of each picture line stays black. */
static void blit(u8 *top, int row, int depth, const u32 *src, int stride, int sw, int sh) {
    static union { double d[OUT_W / 2 + 2]; u32 w[OUT_W + 4]; u16 h[OUT_W * 2 + 8]; } buf;
    if (top != last_line) last_valid = 0;
    last_line = top;
    const int aligned = !((size_t)top & 3) && !(row & 7), px = depth / 8;
    for (int y = 0; y < sh; y++, src += stride) {
        /* only the part of the line that changed */
        int from_x = 0, to_x = sw;
        if (last_valid) {
            const u32 *old = last_frame[y];
            while (from_x < sw && old[from_x] == src[from_x]) from_x++;
            if (from_x == sw) continue;
            while (old[to_x - 1] == src[to_x - 1]) to_x--;
        }
        memcpy(last_frame[y] + from_x, src + from_x, (size_t)(to_x - from_x) * 4);
        u8 *line = top + (size_t)y_at[y] * row;
        const int skew = aligned ? (int)((size_t)line & 7) : 0;
        /* buffer offsets, on 8-byte boundaries of the screen */
        int first = aligned ? (skew + x_at[from_x] * px) & ~7 : skew, end = aligned ? (skew + x_at[to_x] * px + 7) & ~7 : skew + map_dw * px;
        if (first < skew) first = skew;
        if (end > skew + map_dw * px) end = skew + map_dw * px;
        const int d0 = (first - skew) / px, d1 = (end - skew) / px;
        if (depth == 32) {
            u32 *p = buf.w + (first >> 2);
            for (int x = d0; x < d1; x++) *p++ = src[x_src[x]];
        } else {
            static u16 conv[SRC_W];
            const int s0 = x_src[d0], s1 = x_src[d1 - 1];
            for (int x = s0; x <= s1; x++) {
                const u32 c = src[x];
                conv[x] = (u16)((c >> 9 & 0x7C00) | (c >> 6 & 0x3E0) | (c >> 3 & 0x1F));
            }
            u16 *p = buf.h + (first >> 1);
            for (int x = d0; x < d1; x++) *p++ = conv[x_src[x]];
        }
        int reps = y_at[y + 1] - y_at[y];
        if (every_other && reps > 1) reps--;
        for (int rep = 0; rep < reps; rep++) {
            volatile u8 *d = line + (size_t)rep * row + (first - skew);
            const u8 *from = (const u8 *)buf.w + first;
            int left = end - first;
            if (!aligned) {              /* 16-bit picture on an odd pixel */
                for (int i = 0; i < left; i += 2) *(volatile u16 *)(d + i) = *(const u16 *)(from + i);
                continue;
            }
            if (first & 7) { *(volatile u32 *)d = *(const u32 *)from; d += 4; from += 4; left -= 4; }
            for (; left >= 8; left -= 8, d += 8, from += 8) *(volatile double *)d = *(const double *)from;
            if (left >= 4) *(volatile u32 *)d = *(const u32 *)from;
        }
    }
    last_valid = 1;
}

/* Straight into video memory, when the whole picture is on the main screen at
 * 16 or 32 bits and nothing can cover it. Returns 0 if the system has to do it
 * instead. */
static int show_direct(const u32 *src, int stride, int sw, int sh) {
    PixMapHandle pm;
    Point o;
    if (map_dw > OUT_W) return 0;
    o.v = pic.top; o.h = pic.left;
    if (dsp_port) {
        pm = dsp_port->portPixMap;
        o.v += (**pm).bounds.top;
        o.h += (**pm).bounds.left;
    } else {
        if (!in_front || FrontWindow() != win) return 0;
        GDHandle gd = GetMainDevice();
        pm = (**gd).gdPMap;
        SetPort((GrafPtr)win);
        LocalToGlobal(&o);
        const Rect sr = (**gd).gdRect;
        if (o.h < sr.left || o.v < sr.top + GetMBarHeight() || o.h + map_dw > sr.right || o.v + map_dh > sr.bottom) return 0;
    }
    const int depth = (**pm).pixelSize;
    if (depth != 32 && depth != 16) return 0;
    if (!dsp_port) {
        Rect r = { o.v, o.h, (short)(o.v + map_dh), (short)(o.h + map_dw) };
        const Point none = { 0, 0 };
        ShieldCursor(&r, none);
    }
    const int row = (**pm).rowBytes & 0x3FFF;
    blit((u8 *)(**pm).baseAddr + (size_t)(o.v - (**pm).bounds.top) * row + (size_t)(o.h - (**pm).bounds.left) * (depth / 8),
         row, depth, src, stride, sw, sh);
    if (!dsp_port) ShowCursor();
    video_path = depth;
    return 1;
}

static void show_picture(const u32 *src, int stride, int sw, int sh) {
    if (show_direct(src, stride, sw, sh)) return;
    video_path = 0;
    PixMapHandle pm = GetGWorldPixMap(gw);
    LockPixels(pm);
    blit((u8 *)GetPixBaseAddr(pm), (**pm).rowBytes & 0x3FFF, 32, src, stride, sw, sh);
    Rect from;
    SetRect(&from, 0, 0, (short)map_dw, (short)map_dh);
    SetPort((GrafPtr)win);
    CopyBits((BitMap *)*pm, &((GrafPtr)win)->portBits, &from, &pic, srcCopy, NULL);
    UnlockPixels(pm);
}

/* the console's picture, or the menu drawn on a copy of it, with enhancements */
static void show_frame(const u32 *src) {
    static u32 work[SRC_H * SRC_W];
    const int sw = vdp_screen_w, sh = vdp_screen_h;
    const int w = enh_upscale ? sw * 2 : sw, h = enh_upscale ? sh * 2 : sh;
    int stride = VDP_FB_STRIDE;
    if (vdp_widescreen) layout(w, h, 16, 9);
    else if (enh_shape) layout(w, h, 4, 3);
    else layout(w, h, w, h);
    if (enh_upscale) {
        enhance_scale2x(src, stride, sw, sh, work);
        src = work; stride = w;
    }
    if (enh_color) {
        if (src != work) {
            for (int y = 0; y < h; y++) memcpy(work + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
            src = work; stride = w;
        }
        enhance_vivid(work, w * h);
    }
    show_picture(src, stride, w, h);
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
        area_w = sb.right - sb.left; area_h = sb.bottom - sb.top;
    } else {
        area_w = 640; area_h = 480;
        SetRect(&bounds, 0, 0, 640, 480);
        OffsetRect(&bounds, (short)(((sb.right - 640) / 2) & ~3), (short)((sb.bottom - 480) / 2 + 10));
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

/* ---------------------------------------------------------- USB devices
 * What the USB Manager really has on the bus, driver or not: logged at start. */
static OSStatus (*usb_next)(u32 *, ConnectionID *, u16, u16, u16);
static OSStatus (*usb_descriptor)(u32 *, void *, u32);

static void usb_open(void) {
    static const unsigned char *const names[] = { "\pUSBServicesLib", "\pUSBFamilyExpertLib", "\pUSBManagerLib" };
    for (int i = 0; i < 3 && !usb_next; i++) {
        ConnectionID lib;
        Ptr entry;
        Str255 why;
        char l[80];
        const OSErr e = GetSharedLibrary(names[i], 'pwpc', 1, &lib, &entry, why);
        snprintf(l, sizeof l, "USB: library %.*s %d", names[i][0], names[i] + 1, e);
        log_line(l);
        if (e != noErr) continue;
        usb_next = isp_find(lib, "\pUSBGetNextDeviceByClass");
        usb_descriptor = isp_find(lib, "\pUSBGetDeviceDescriptor");
        if (!usb_descriptor) usb_next = NULL;
    }
}

static void usb_scan(void) {
    static char lines[16][96];
    if (!usb_next) return;
    u32 ref = 0xFFFFFFFFu;
    ConnectionID conn;
    int n = 0;
    while (n < 16 && usb_next(&ref, &conn, 0xFFFF, 0xFFFF, 0xFFFF) == noErr) {
        u8 d[18];
        memset(d, 0, sizeof d);
        const OSStatus e = usb_descriptor(&ref, d, sizeof d);
        snprintf(lines[n], sizeof lines[n], "USB: device %08lX vendor %02X%02X product %02X%02X class %02X/%02X/%02X usb %02X%02X conf %d err %ld",
                 (unsigned long)ref, d[9], d[8], d[11], d[10], d[4], d[5], d[6], d[3], d[2], d[17], (long)e);
        n++;
    }
    char l[40];
    snprintf(l, sizeof l, "USB: %d devices", n);
    log_line(l);
    for (int i = 0; i < n; i++) log_line(lines[i]);
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

static void clear_screen(void) {
    if (dsp_port) dsp_clear(); else black_window();
}

/* At the display's own size, because an LCD can't fill the screen from anything
 * smaller, and 16 bits, because that is half the data to send. */
static int dsp_enter(void) {
    if (!dsp_best) return 0;
    DspAttrs a;
    memset(&a, 0, sizeof a);
    const Rect sb = qd.screenBits.bounds;
    a.width = (u32)(sb.right - sb.left); a.height = (u32)(sb.bottom - sb.top);
    a.color_needs = 2;              /* require */
    a.back_depths = a.display_depths = 1u << 5 | 1u << 4;
    a.back_best = a.display_best = 16;
    a.pages = 1;
    char l[100];
    OSStatus s = dsp_best(&a, &dsp_ctx);
    snprintf(l, sizeof l, "DrawSprocket: find %lu x %lu %ld", (unsigned long)a.width, (unsigned long)a.height, (long)s);
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
    area_w = (**pm).bounds.right - (**pm).bounds.left;
    area_h = (**pm).bounds.bottom - (**pm).bounds.top;
    dsp_clear();
    HideCursor();
    return 1;
}

static void dsp_leave(void) {
    if (!dsp_port) return;
    dsp_port = NULL;
    dsp_state(dsp_ctx, 2);          /* inactive */
    dsp_release(dsp_ctx);
    ShowCursor();
    area_w = 640; area_h = 480;
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

/* ------------------------------------------------------------- events */
static void pump(void) {
    EventRecord ev;
    if (!WaitNextEvent(everyEvent, &ev, 0, NULL)) return;
    unsigned char taken = 0;
    if (dsp_port && dsp_event) dsp_event(&ev, &taken);
    if (taken) return;
    if (ev.what == mouseDown) {
        WindowPtr w;
        if (FindWindow(ev.where, &w) == inDrag) DragWindow(w, ev.where, &qd.screenBits.bounds);
    } else if (ev.what == osEvt && (ev.message >> 24 & 0xFF) == 1) {
        in_front = ev.message & 1;   /* suspend or resume */
        if (isp_on && isp_suspend && isp_resume) { if (in_front) isp_resume(); else isp_suspend(); }
        last_valid = 0;
    } else if (ev.what == updateEvt) {
        BeginUpdate((WindowPtr)ev.message);
        EndUpdate((WindowPtr)ev.message);
        last_valid = 0;
    }
}

static void toggle_full(void) {
    if (dsp_port) dsp_leave();
    else if (full_screen) open_window(0);
    else if (!dsp_enter()) open_window(1);
}

/* ------------------------------------------------------------ settings */
#define SETTINGS "genexia.cfg"
static int settings_get(const char *key, int def) {
    FILE *f = fopen(SETTINGS, "r");
    if (!f) return def;
    char k[32];
    int v;
    while (fscanf(f, "%31s %d", k, &v) == 2) if (!strcmp(k, key)) { fclose(f); return v; }
    fclose(f);
    return def;
}
static int stats_on;
static void settings_save(void) {
    FILE *f = fopen(SETTINGS, "w");
    if (!f) return;
    fprintf(f, "wide %d\nupscale %d\nscanlines %d\ncolor %d\nshape %d\nlimit %d\nspeed %d\nstats %d\n", vdp_widescreen, enh_upscale, every_other,
            enh_color, enh_shape, vdp_no_sprite_limit, speed_pct, stats_on);
    fclose(f);
}

/* ---------------------------------------------------------- options menu */
enum { M_FULL, M_SPEED, M_ENHANCE, M_CLOSE, M_WIDE, M_UPSCALE, M_FILTER, M_COLOR, M_SHAPE, M_LIMIT, M_BACK, M_COUNT };
static const int menu_pages[2][8] = {
    { M_FULL, M_SPEED, M_ENHANCE, M_CLOSE, -1 },
    { M_WIDE, M_UPSCALE, M_FILTER, M_COLOR, M_SHAPE, M_LIMIT, M_BACK, -1 },
};
static const char *const menu_names[M_COUNT] = { "Full screen", "Game speed", "Enhancements", "Close", "Widescreen", "Pixel art upscaler",
                                                 "Video filter", "Colour", "Picture shape", "Sprite limit", "Back" };
static const char *menu_value(int i) {
    switch (i) {
    case M_FULL:    return dsp_port || full_screen ? "On" : "Off";
    case M_SPEED: { static char b[8]; snprintf(b, sizeof b, "%d%%", speed_pct); return b; }
    case M_ENHANCE: return ">";
    case M_WIDE:    return vdp_widescreen ? "16:9" : "Off";
    case M_UPSCALE: return enh_upscale ? "Scale2x" : "Off";
    case M_FILTER:  return every_other ? "Scanlines" : "Sharp";
    case M_COLOR:   return enh_color ? "Vivid" : "Original";
    case M_SHAPE:   return vdp_widescreen ? "-" : enh_shape ? "TV 4:3" : "Square pixels";
    case M_LIMIT:   return vdp_no_sprite_limit ? "Removed" : "Original";
    default:        return "";
    }
}
static void menu_change(int i, int dir) {
    switch (i) {
    case M_FULL:    toggle_full(); break;
    case M_SPEED:   set_speed(speed_pct + dir * 10); break;
    case M_WIDE:    vdp_widescreen = !vdp_widescreen; break;
    case M_UPSCALE: enh_upscale = !enh_upscale; break;
    case M_FILTER:  every_other = !every_other; clear_screen(); break;
    case M_COLOR:   enh_color = !enh_color; break;
    case M_SHAPE:   if (!vdp_widescreen) enh_shape = !enh_shape; break;
    case M_LIMIT:   vdp_no_sprite_limit = !vdp_no_sprite_limit; break;
    default: break;
    }
}
static void menu_draw(u32 *fb, int page, int sel, int count) {
    const int w = vdp_screen_w, h = vdp_screen_h, st = VDP_FB_STRIDE, PW = 248, RH = 15, PH = 54 + count * RH;
    const int x0 = (w - PW) / 2, y0 = (h - PH) / 2;
    fx_box(fb, st, w, h, 0, 0, w, h, 0x000010, 0.62f);
    fx_box(fb, st, w, h, x0, y0, x0 + PW, y0 + PH, 0x10102C, 0.92f);
    fx_box(fb, st, w, h, x0, y0, x0 + PW, y0 + 1, 0x5060FF, 1.0f);
    fx_box(fb, st, w, h, x0, y0 + PH - 1, x0 + PW, y0 + PH, 0x5060FF, 1.0f);
    const char *t = "BUILD " GENEXIA_BUILD;
    fx_text(fb, st, w, h, t, w - fx_text_width(t, 1) - 4, 4, 1, 0xA0B0D0, 0xA0B0D0);
    t = page ? "ENHANCEMENTS" : "GENEXIA OPTIONS";
    fx_text(fb, st, w, h, t, (w - fx_text_width(t, 2)) / 2, y0 + 8, 2, 0xFFFFFF, 0x80A0FF);
    for (int i = 0; i < count; i++) {
        const int item = menu_pages[page][i], y = y0 + 32 + i * RH, on = i == sel;
        if (on) fx_box(fb, st, w, h, x0 + 6, y - 3, x0 + PW - 6, y + 10, 0x3040A0, 0.9f);
        const char *v = menu_value(item);
        const u32 c = on ? 0xFFFFFF : 0xB0B8D0;
        fx_text(fb, st, w, h, menu_names[item], x0 + 12, y, 1, c, c);
        if (*v) {
            char shown[40];
            snprintf(shown, sizeof shown, on && v[0] != '-' ? "< %s >" : "%s", v);
            fx_text(fb, st, w, h, shown, x0 + PW - 12 - fx_text_width(shown, 1), y, 1, on ? 0xFFE060 : 0xD0D8F0, on ? 0xFFA020 : 0xA0B0D0);
        }
    }
    t = "UP DOWN SELECT   LEFT RIGHT CHANGE";
    fx_text(fb, st, w, h, t, (w - fx_text_width(t, 1)) / 2, y0 + PH - 14, 1, 0x8090B0, 0x8090B0);
}
/* The game waits while this runs. */
static void menu_run(void) {
    static u32 fb[VDP_FB_STRIDE * 240];
    int page = 0, sel = 0, open = 1;
    while (open) {
        pump();
        keys_poll();
        int count = 0;
        while (count < 8 && menu_pages[page][count] >= 0) count++;
        const int item = menu_pages[page][sel];
        if (press(0x7E)) sel = (sel + count - 1) % count;
        if (press(0x7D)) sel = (sel + 1) % count;
        if (press(0x7B)) menu_change(item, -1);
        if (press(0x7C)) menu_change(item, 1);
        if (press(0x24)) {
            if (item == M_CLOSE) open = 0;
            else if (item == M_ENHANCE) { page = 1; sel = 0; }
            else if (item == M_BACK) { page = 0; sel = 0; }
            else menu_change(item, 1);
        }
        if (press(0x35)) { if (page) { page = 0; sel = 0; } else open = 0; }
        if (press(0x65) | press(0x6D)) open = 0;
        if (!open) break;
        count = 0;
        while (count < 8 && menu_pages[page][count] >= 0) count++;
        memcpy(fb, vdp_framebuffer, sizeof fb);
        menu_draw(fb, page, sel, count);
        show_frame(fb);
        const unsigned long until = TickCount() + 2;
        while (TickCount() < until) { }
    }
    settings_save();
}

/* ---------------------------------------------------------- speed bar
 * Like the modern app's: it shows when the mouse nears the bottom of the
 * picture or the speed changes, and dragging it sets the speed. */
static unsigned long bar_until;
static int bar_drag;
static void bar_show(void) { bar_until = TickCount() + 150; }

static void bar_mouse(void) {
    const int w = vdp_screen_w, h = vdp_screen_h;
    Point at = LMGetMouseLocation(), o = { pic.top, pic.left };
    if (!dsp_port) { SetPort((GrafPtr)win); LocalToGlobal(&o); }
    if (!map_dw || !map_dh) return;
    const int x = (at.h - o.h) * w / map_dw, y = (at.v - o.v) * h / map_dh;
    const int bx = w / 4, bw = w / 2, by = h - h / 10;
    if (y > h - h / 5 && y < h && x >= 0 && x < w) bar_show();
    const int down = Button();
    if (!down) bar_drag = 0;
    else if (TickCount() < bar_until && x >= bx - 8 && x <= bx + bw + 8 && y >= by - 10 && y <= by + 12) bar_drag = 1;
    if (bar_drag) {
        const int t = x < bx ? 0 : x > bx + bw ? bw : x - bx;
        set_speed(50 + t * 150 / bw);
        bar_show();
    }
}

static void bar_draw(u32 *fb) {
    const int w = vdp_screen_w, h = vdp_screen_h, st = VDP_FB_STRIDE;
    const int bx = w / 4, bw = w / 2, by = h - h / 10, pos = bw * (speed_pct - 50) / 150;
    char label[16];
    fx_box(fb, st, w, h, bx - 6, by - 12, bx + bw + 6, by + 8, 0, 0.66f);
    fx_box(fb, st, w, h, bx, by, bx + bw, by + 4, 0x5A5A6E, 1.0f);
    fx_box(fb, st, w, h, bx, by, bx + pos, by + 4, 0xFFC828, 1.0f);
    fx_box(fb, st, w, h, bx + bw / 3, by - 2, bx + bw / 3 + 1, by + 6, 0xFFFFFF, 0.8f);
    fx_box(fb, st, w, h, bx + pos - 1, by - 2, bx + pos + 2, by + 6, 0xFFFFFF, 1.0f);
    snprintf(label, sizeof label, "SPEED %d%%", speed_pct);
    fx_text(fb, st, w, h, label, bx, by - 10, 1, 0xFFFFFF, 0xFFFFFF);
}

/* What is running and how fast, on top of the picture. F8. */
static char stats[3][40];
static void stats_draw(u32 *fb) {
    const int w = vdp_screen_w, h = vdp_screen_h;
    int tw = 0;
    for (int i = 0; i < 3; i++) { const int n = fx_text_width(stats[i], 1); if (n > tw) tw = n; }
    fx_box(fb, VDP_FB_STRIDE, w, h, 4, 4, 12 + tw, 4 + 34, 0, 0.66f);
    for (int i = 0; i < 3; i++) fx_text(fb, VDP_FB_STRIDE, w, h, stats[i], 8, 8 + i * 10, 1, 0xFFFFFF, 0xFFFFFF);
}

/* ------------------------------------------------------- game selector
 * Every Genesis ROM next to the application or in its roms folder. Recompiled
 * code only belongs to the exact ROM it was made from, so the header says
 * which it is, and anything else runs on the interpreter. */
typedef struct { FSSpec file; char name[64]; int code; } Game;
#define MAX_GAMES 64
static Game games[MAX_GAMES];
static int ngames;

static int rom_code(const u8 *r) {
    const u16 hdr = (u16)(r[0x18E] << 8 | r[0x18F]);
    if (hdr == 0xFB5E && !memcmp(r + 0x180, "GM T-12033", 10)) return 1;
    if (hdr == 0xFC4C && !memcmp(r + 0x180, "GM T-097146", 11)) return 2;
    return 0;
}
enum { CODE_SMK = 3, CODE_SNES = 4 };      /* the other values: rom_code() */

/* the whole file into rom[], because a SNES header can be in several places */
static int rom_read(const FSSpec *spec) {
    short ref;
    long n = sizeof rom;
    if (FSpOpenDF(spec, fsRdPerm, &ref) != noErr) return 0;
    const OSErr e = FSRead(ref, &n, rom);
    FSClose(ref);
    if ((e != noErr && e != eofErr) || n < 0x200) return 0;
    rom_size = (u32)n;
    return 1;
}
static void snes_name(Game *g) {
    const char *title = snes_load();
    g->code = !strcmp(title, "SUPER MARIO KART") && rom[0xFFDE] == 0x44 && rom[0xFFDF] == 0xEB ? CODE_SMK : CODE_SNES;
    snprintf(g->name, sizeof g->name, "%s", g->code == CODE_SMK ? "Super Mario Kart" : title);
}

static void game_probe(const FSSpec *spec) {
    if (ngames == MAX_GAMES || !rom_read(spec)) return;
    const u8 *r = rom;
    Game *g = &games[ngames];
    g->file = *spec;
    if (memcmp(r + 0x100, "SEGA", 4)) {
        if (!snes_detect()) return;
        snes_name(g);
        for (int i = 0; i < ngames; i++) if (!strcmp(games[i].name, g->name)) return;
        ngames++;
        return;
    }
    g->code = rom_code(r);
    const char *t = (const char *)r + 0x150;
    int len = 48, j = 0;
    while (len > 0 && (t[len - 1] == ' ' || !t[len - 1])) len--;
    if (!len) { t = (const char *)r + 0x120; len = 48; while (len > 0 && (t[len - 1] == ' ' || !t[len - 1])) len--; }
    for (int i = 0; i < len && j < 63; i++) {   /* collapse padding, drop non-ASCII */
        const char c = t[i];
        if (c < 32 || c > 126 || (c == ' ' && j && g->name[j - 1] == ' ')) continue;
        g->name[j++] = c;
    }
    g->name[j] = 0;
    if (g->code == 1) strcpy(g->name, "Street Fighter II' Champion Edition");
    for (int i = 0; i < ngames; i++) if (!strcmp(games[i].name, g->name)) return;
    ngames++;
}

static void game_scan(short vol, long dir, int deep) {
    static const char *const exts[] = { ".bin", ".md", ".gen", ".sfc", ".smc" };
    for (short i = 1;; i++) {
        Str255 name;
        CInfoPBRec pb;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vol;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) return;
        char c[64];
        const int n = name[0] > 63 ? 63 : name[0];
        for (int k = 0; k < n; k++) c[k] = (char)(name[k + 1] >= 'A' && name[k + 1] <= 'Z' ? name[k + 1] + 32 : name[k + 1]);
        c[n] = 0;
        if (pb.hFileInfo.ioFlAttrib & 0x10) {
            if (deep && !strcmp(c, "roms")) game_scan(vol, pb.dirInfo.ioDrDirID, 0);
            continue;
        }
        for (int k = 0; k < 5; k++) {
            const int x = (int)strlen(exts[k]);
            FSSpec spec;
            if (n > x && !strcmp(c + n - x, exts[k]) && FSMakeFSSpec(vol, dir, name, &spec) == noErr) { game_probe(&spec); break; }
        }
    }
}

static void select_draw(u32 *fb, int sel, int top) {
    enum { W = 320, H = 240, ST = VDP_FB_STRIDE, RH = 18, TOP = 50, ROWS = 9 };
    fx_box(fb, ST, W, H, 0, 0, W, H, 0x0A0A1E, 1.0f);
    fx_box(fb, ST, W, H, 0, 34, W, 35, 0x5060FF, 1.0f);
    const char *t = "GENEXIA";
    fx_text(fb, ST, W, H, t, (W - fx_text_width(t, 3)) / 2, 8, 3, 0xFFFFFF, 0x80A0FF);
    t = "BUILD " GENEXIA_BUILD;
    fx_text(fb, ST, W, H, t, W - fx_text_width(t, 1) - 4, 4, 1, 0xA0B0D0, 0xA0B0D0);
    if (!ngames) {
        t = "NO GAMES FOUND. PUT ROMS NEXT TO THIS APP.";
        fx_text(fb, ST, W, H, t, (W - fx_text_width(t, 1)) / 2, 100, 1, 0xD0D8F0, 0xA0B0D0);
    }
    for (int i = top; i < ngames && i < top + ROWS; i++) {
        const int y = TOP + (i - top) * RH, on = i == sel;
        if (on) fx_box(fb, ST, W, H, 6, y - 4, W - 6, y + 12, 0x3040A0, 0.9f);
        const int fast = games[i].code && games[i].code != CODE_SNES;
        const char *tag = fast ? "RECOMPILED" : "INTERPRETED";
        const u32 tc = fast ? 0x60E090 : 0x8090B0, c = on ? 0xFFFFFF : 0xB0B8D0;
        const int tagw = fx_text_width(tag, 1);
        char name[64];
        snprintf(name, sizeof name, "%s", games[i].name);
        for (size_t n = strlen(name); n && fx_text_width(name, 1) > W - 36 - tagw; n--) name[n - 1] = 0;
        fx_text(fb, ST, W, H, name, 12, y, 1, c, c);
        fx_text(fb, ST, W, H, tag, W - 12 - tagw, y, 1, tc, tc);
    }
    t = "UP DOWN CHOOSE   RETURN PLAY   ESC QUIT";
    fx_text(fb, ST, W, H, t, (W - fx_text_width(t, 1)) / 2, H - 14, 1, 0x8090B0, 0x8090B0);
}

/* Returns the chosen game, or NULL to quit. */
static const Game *game_select(void) {
    static u32 fb[VDP_FB_STRIDE * 240];
    WDPBRec here;
    memset(&here, 0, sizeof here);
    if (PBHGetVolSync(&here) == noErr) game_scan(here.ioWDVRefNum, here.ioWDDirID, 1);
    int sel = 0, top = 0;
    for (;;) {
        pump();
        keys_poll();
        if (press(0x35)) return NULL;
        if (ngames) {
            if (press(0x7E)) sel = (sel + ngames - 1) % ngames;
            if (press(0x7D)) sel = (sel + 1) % ngames;
            if (press(0x24)) return &games[sel];
        }
        if (press(0x78)) toggle_full();
        {   /* a click on a row plays it */
            static int was_down;
            const int down = Button();
            if (down && !was_down && map_dh) {
                Point at = LMGetMouseLocation(), o = { pic.top, pic.left };
                if (!dsp_port) { SetPort((GrafPtr)win); LocalToGlobal(&o); }
                const int y = (at.v - o.v) * 240 / map_dh, row = y < 46 ? -1 : (y - 46) / 18;
                if (row >= 0 && row < 9 && top + row < ngames) return &games[top + row];
            }
            was_down = down;
        }
        if (sel < top) top = sel;
        if (sel >= top + 9) top = sel - 8;
        select_draw(fb, sel, top);
        layout(320, 240, 4, 3);
        show_picture(fb, VDP_FB_STRIDE, 320, 240);
        const unsigned long until = TickCount() + 2;
        while (TickCount() < until) { }
    }
}

static int game_load(const Game *g) {
    char l[100];
    if (!rom_read(&g->file)) return 0;
    snprintf(l, sizeof l, "Game: %s (%s)", g->name, g->code && g->code != CODE_SNES ? "recompiled" : "interpreter");
    log_line(l);
    if (g->code >= CODE_SMK) {
        Game again = *g;
        snes_name(&again);
        snes_register_game = g->code == CODE_SMK ? recomp_register_smk : NULL;
        snes_force_interp = !snes_register_game;
        snes_game = 1;
        snd_rate = SNES_DSP_RATE;
        snd_gain = 2;
        game_is_sf2 = 0;
        vdp_hud_rules = 0;
        return 1;
    }
    m68k_register_game = g->code == 1 ? recomp_register : g->code == 2 ? recomp_register_umk3 : NULL;
    m68k_force_interp = !m68k_register_game;
    game_is_sf2 = g->code == 1;
    vdp_hud_rules = g->code;
    /* console region from the header: new-style hex digit or J/U/E letters */
    int jp = 0, us = 0, eu = 0;
    for (int i = 0x1F0; i < 0x1F3; i++) {
        const char c = (char)rom[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
        if (c == 'J') jp = 1; else if (c == 'U') us = 1; else if (c == 'E') eu = 1;
        else { if (v & 1) jp = 1; if (v & 4) us = 1; if (v & 8) eu = 1; }
    }
    region_version = us ? 0xA0 : jp ? 0x20 : eu ? 0xE0 : 0xA0;
    return 1;
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

    gw_rect = qd.screenBits.bounds;
    OffsetRect(&gw_rect, (short)-gw_rect.left, (short)-gw_rect.top);
    open_window(0);
    if (NewGWorld(&gw, 32, &gw_rect, NULL, NULL, 0) != noErr) fail("\pNot enough memory for the picture.");

    log_line("--- Genexia build " GENEXIA_BUILD);
    isp_open();
    dsp_open();
    usb_open();
    usb_scan();
    long cpu_features = 0;
    vdp_altivec = Gestalt('ppcf', &cpu_features) == noErr && (cpu_features >> 4 & 1);
    log_line(vdp_altivec ? "AltiVec: on" : "AltiVec: off");
    vdp_widescreen = settings_get("wide", 0);
    enh_upscale = settings_get("upscale", 0);
    every_other = settings_get("scanlines", 0);
    enh_color = settings_get("color", 0);
    enh_shape = settings_get("shape", 1);
    vdp_no_sprite_limit = settings_get("limit", 1);
    set_speed(settings_get("speed", 100));
    stats_on = settings_get("stats", 1);

    const Game *game = game_select();
    if (game) {
        if (!game_load(game)) fail("\pThat game could not be read.");
        set_speed(speed_pct);
        vdp_init();
        if (snes_game) snes_reset();
        else {
            bus_reset();
            z80_reset();
            ym2612_reset();
            psg_reset();
            m68k_reset();
        }
        sound_start();
    }

    /* The timebase runs at a rate only the machine knows: measure it. */
    unsigned long mark = TickCount();
    while (TickCount() == mark) { }
    mark = TickCount();
    const u32 t_cal = timebase();
    while (TickCount() < mark + 30) { }
    const u32 tb_sec = (u32)((u64)(timebase() - t_cal) * 60147 / 30000);
    const u32 frame_tb = (u32)((u64)tb_sec * 1000 / (snes_game ? 60099 : 59923));   /* each console's own frame rate */
    char cal[60];
    snprintf(cal, sizeof cal, "Timebase: %lu per second", (unsigned long)tb_sec);
    log_line(cal);

    int shown = 0, skipped = 0, ran = 0, queue_avg = 2000 * 16, loops = 0;
    u32 t_emu = 0, t_vid = 0, t_events = 0, t_other = 0, t_worst = 0, t_mark = timebase(), deadline = t_mark, t_loop = t_mark;
    mark = TickCount();
    while (game) {
        const u32 t_top = timebase();
        if (t_top - t_loop > t_worst) t_worst = t_top - t_loop;
        t_loop = t_top;
        /* the system runs other programs in here, sometimes for 1/4 second */
        if (!(++loops & 31) || Button()) pump();
        const u32 t_pumped = timebase();
        t_events += t_pumped - t_top;
        read_keys();
        keys_poll();
        if (press(0x35)) break;              /* Esc */
        if (press(0x7A)) {
            every_other = !every_other;
            clear_screen();
        }
        if (press(0x78)) {
            toggle_full();
            deadline = timebase();
        }
        if (press(0x63) && isp_on && isp_configure) {
            dsp_leave();
            if (full_screen) open_window(0);
            const OSStatus s = isp_configure(NULL);
            isp_log("configure", s);
            black_window();
            deadline = timebase();
        }
        if (press(0x1B)) { set_speed(speed_pct - 10); bar_show(); settings_save(); }
        if (press(0x18)) { set_speed(speed_pct + 10); bar_show(); settings_save(); }
        if (press(0x1D)) { set_speed(100); bar_show(); settings_save(); }
        {
            const int dragged = bar_drag;
            bar_mouse();
            if (dragged && !bar_drag) settings_save();
        }
        if (press(0x64)) { stats_on = !stats_on; settings_save(); }
        if (press(0x65) | press(0x6D)) {
            menu_run();
            deadline = timebase();
        }

        /* more than a frame late: run the game but don't draw */
        const u32 t0 = timebase();
        t_other += t0 - t_pumped;
        const u32 period = (u32)((u64)frame_tb * 100 / (u32)speed_pct);
        const int behind = (s32)(t0 - deadline) > (s32)period && skipped < 3;
        vdp_skip_render = behind;
        run_frame();
        const u32 t1 = timebase();
        t_emu += t1 - t0;
        ran++;
        if (behind) skipped++;
        else {
            if (stats_on) stats_draw(vdp_framebuffer);    /* the next frame paints over it */
            if (TickCount() < bar_until) {
                static u32 fb[VDP_FB_STRIDE * 240];
                memcpy(fb, vdp_framebuffer, sizeof fb);
                bar_draw(fb);
                show_frame(fb);
            } else show_frame(vdp_framebuffer);
            t_vid += timebase() - t1;
            shown++;
            skipped = 0;
        }

        /* The Sound Manager's clock is the master. It takes samples in big
         * steps, so follow its average, and lean on the frame time by a hair
         * instead of ever holding a frame back. */
        queue_avg += (int)(ring_w - ring_r) - queue_avg / 16;
        const int excess = queue_avg / 16 - 2000;
        deadline += period + (u32)((s32)(frame_tb >> 16) * (excess > 6000 ? 6000 : excess < -6000 ? -6000 : excess) / 8);
        while ((s32)(timebase() - deadline) < 0) { }
        /* stalled: carry on from now, because racing to catch up floods the
         * sound queue and draws nothing */
        if ((s32)(timebase() - deadline) > (s32)(period * 3)) deadline = timebase();
        if (TickCount() - mark >= 60) {
            char t[80];
            const u32 ticks = TickCount() - mark, all = timebase() - t_mark;
            const int n = snprintf(t + 1, sizeof t - 1, "Game %d  Drawn %d  CPU %d%%  Draw %d%%", (int)(ran * 60 / ticks), (int)(shown * 60 / ticks),
                                   (int)(t_emu / (all / 100 + 1)), (int)(t_vid / (all / 100 + 1)));
            t[0] = (char)n;
            if (!full_screen && !dsp_port) SetWTitle(win, (ConstStr255Param)t);
            {
                char l[140];
                snprintf(l, sizeof l, "%s  Events %d%%  Other %d%%  worst %d ms  speed %d  video %d%s%s  sound %s  queued %d  pad %04X  ticks %lu", t + 1,
                         (int)(t_events / (all / 100 + 1)), (int)(t_other / (all / 100 + 1)), (int)((u64)t_worst * 1000 / tb_sec), speed_pct, video_path,
                         every_other ? " alt-lines" : "", dsp_port ? " full640" : full_screen ? " full" : "", snd_chan ? "on" : "off",
                         (int)(ring_w - ring_r), pad_state[0], TickCount());
                log_line(l);
                const u32 p = all / 100 + 1;
                if (snes_game) snprintf(l, sizeof l, "Split  65816 %d%%  PPU %d%%  APU %d%%", (int)(t_68k / p), (int)(t_vdp / p), (int)(t_ym / p));
                else snprintf(l, sizeof l, "Split  68k %d%%  Z80 %d%%  VDP %d%%  YM %d%%  AltiVec lines %u", (int)(t_68k / p), (int)(t_z80 / p), (int)(t_vdp / p), (int)(t_ym / p), vdp_altivec_lines);
                vdp_altivec_lines = 0;
                log_line(l);
                {
                    const u32 nat = snes_game ? snes_native_cycles : m68k_native_cycles, emu = snes_game ? snes_interp_cycles : m68k_interp_cycles;
                    const u32 sum = nat / 100 + emu / 100 + 1;
                    const int comp = (int)(nat / 100 * 100 / sum);
                    snprintf(stats[0], sizeof stats[0], "FPS %d  GAME %d  SPEED %d%%", (int)(shown * 60 / ticks), (int)(ran * 60 / ticks), speed_pct);
                    snprintf(stats[1], sizeof stats[1], "%s COMPILED %d%% EMULATED %d%%", snes_game ? "65816" : "68000", comp, 100 - comp);
                    if (snes_game) snprintf(stats[2], sizeof stats[2], "CPU %d PPU %d APU %d DRAW %d", (int)(t_68k / p), (int)(t_vdp / p), (int)(t_ym / p), (int)(t_vid / p));
                    else snprintf(stats[2], sizeof stats[2], "CPU %d VDP %d SND %d DRAW %d", (int)(t_68k / p), (int)(t_vdp / p), (int)((t_ym + t_z80) / p), (int)(t_vid / p));
                    snes_native_cycles = snes_interp_cycles = m68k_native_cycles = m68k_interp_cycles = 0;
                }
                t_68k = t_z80 = t_vdp = t_ym = 0;
            }
            shown = ran = 0;
            t_emu = t_vid = t_events = t_other = t_worst = 0;
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
