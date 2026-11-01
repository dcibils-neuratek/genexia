/*
 * os9_main.c - a small front end for Mac OS 9 on PowerPC (Retro68).
 *
 * Runs the same Genesis core and recompiled game as the modern app, without
 * SDL or enhancements: Toolbox window, Sound Manager and the keyboard. The ROM
 * is read from a file called sf2.bin next to the application.
 *
 * Keys: arrows, Z X C = A B C, A S D = X Y Z, Return = Start, Esc = quit,
 * F1 = draw every other line (faster, looks like scanlines).
 * The title bar shows game and drawn frames per second, and the share of time
 * in the emulation and in drawing: how to judge a machine.
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
    pad_state[0] = b;
    return key_down(km, 0x35);       /* Esc */
}

/* -------------------------------------------------------------- video */
static WindowPtr win;
static GWorldPtr gw;
static Rect pic;

static int in_front = 1, every_other;

static inline u32 timebase(void) { u32 t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }

/* Straight into video memory, when the whole picture is on the main screen at
 * 16 or 32 bits and nothing can cover it. Every second line is left out in
 * every_other mode. Returns 0 if the system has to do it instead. */
static int show_direct(void) {
    if (!in_front || FrontWindow() != win) return 0;
    GDHandle gd = GetMainDevice();
    PixMapHandle pm = (**gd).gdPMap;
    const int depth = (**pm).pixelSize;
    if (depth != 32 && depth != 16) return 0;
    Point o = { 0, 0 };
    SetPort((GrafPtr)win);
    LocalToGlobal(&o);
    const Rect sr = (**gd).gdRect;
    if (o.h < sr.left || o.v < sr.top + GetMBarHeight() || o.h + FB_W * 2 > sr.right || o.v + FB_H * 2 > sr.bottom) return 0;
    Rect r = { o.v, o.h, (short)(o.v + FB_H * 2), (short)(o.h + FB_W * 2) };
    const Point none = { 0, 0 };
    ShieldCursor(&r, none);
    const int row = (**pm).rowBytes & 0x3FFF;
    u8 *line = (u8 *)(**pm).baseAddr + (size_t)(o.v - (**pm).bounds.top) * row + (size_t)(o.h - (**pm).bounds.left) * (depth / 8);
    for (int y = 0; y < FB_H; y++, line += row * 2) {
        const u32 *src = &vdp_framebuffer[y * VDP_FB_STRIDE];
        for (int rep = 0; rep < (every_other ? 1 : 2); rep++) {
            if (depth == 32) {
                volatile u32 *d = (volatile u32 *)(line + (size_t)rep * row);
                for (int x = 0; x < FB_W; x++) { const u32 c = src[x]; d[0] = c; d[1] = c; d += 2; }
            } else {
                volatile u32 *d = (volatile u32 *)(line + (size_t)rep * row);
                for (int x = 0; x < FB_W; x++) {
                    const u32 c = src[x], p = (c >> 9 & 0x7C00) | (c >> 6 & 0x3E0) | (c >> 3 & 0x1F);
                    *d++ = p << 16 | p;
                }
            }
        }
    }
    ShowCursor();
    return 1;
}

static void show_frame(void) {
    if (show_direct()) return;
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
    CopyBits((BitMap *)*pm, &((GrafPtr)win)->portBits, &pic, &pic, srcCopy, NULL);
    UnlockPixels(pm);
}

static void fail(ConstStr255Param msg) {
    SetPort((GrafPtr)win);
    MoveTo(20, 40);
    DrawString(msg);
    while (!Button()) { }
    ExitToShell();
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

    Rect bounds = { 0, 0, FB_H * 2, FB_W * 2 };
    pic = bounds;
    OffsetRect(&bounds, (qd.screenBits.bounds.right - FB_W * 2) / 2, (qd.screenBits.bounds.bottom - FB_H * 2) / 2 + 10);
    win = NewCWindow(NULL, &bounds, "\pGenexia build " GENEXIA_BUILD, true, noGrowDocProc, (WindowPtr)-1, false, 0);
    SetPort((GrafPtr)win);

    FILE *f = fopen("sf2.bin", "rb");
    if (!f) fail("\pPut the ROM next to this application as sf2.bin, then restart.");
    rom_size = (u32)fread(rom, 1, sizeof rom, f);
    fclose(f);
    if (NewGWorld(&gw, 32, &pic, NULL, NULL, 0) != noErr) fail("\pNot enough memory for the picture.");

    bus_reset();
    vdp_init();
    z80_reset();
    ym2612_reset();
    psg_reset();
    m68k_reset();
    sound_start();

    unsigned long next = TickCount(), mark = next;
    int shown = 0, skipped = 0, ran = 0, f1_held = 0;
    u32 t_emu = 0, t_vid = 0, t_mark = timebase();
    for (;;) {
        EventRecord ev;
        if (WaitNextEvent(everyEvent, &ev, 0, NULL)) {
            if (ev.what == mouseDown) {
                WindowPtr w;
                if (FindWindow(ev.where, &w) == inDrag) DragWindow(w, ev.where, &qd.screenBits.bounds);
            } else if (ev.what == osEvt && (ev.message >> 24 & 0xFF) == 1) {
                in_front = ev.message & 1;   /* suspend or resume */
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
                SetPort((GrafPtr)win);
                EraseRect(&pic);
            }
            f1_held = f1;
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
        while (TickCount() < next) { }
        if (TickCount() > next + 30) next = TickCount();
        if (TickCount() - mark >= 120) {
            char t[80];
            const u32 ticks = TickCount() - mark, all = timebase() - t_mark;
            const int n = snprintf(t + 1, sizeof t - 1, "Game %d  Drawn %d  CPU %d%%  Draw %d%%", (int)(ran * 60 / ticks), (int)(shown * 60 / ticks),
                                   (int)(t_emu / (all / 100 + 1)), (int)(t_vid / (all / 100 + 1)));
            t[0] = (char)n;
            SetWTitle(win, (ConstStr255Param)t);
            shown = ran = 0;
            t_emu = t_vid = 0;
            t_mark = timebase();
            mark = TickCount();
        }
    }
    if (snd_chan) SndDisposeChannel(snd_chan, true);
    return 0;
}
