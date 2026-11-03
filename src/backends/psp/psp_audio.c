/* Sound: the game's output channels and the SAS voice mixer, whose voices play
 * the PSP's ADPCM samples. Not here yet: reverb, the exact shape of ADSR envelopes.
 * Included by psp_runtime.c; the window drains the channels into the Mac's output. */
#define AUDIO_CHANNELS 8
#define AUDIO_RING (1 << 16)
#define AUDIO_READY 12288      /* 70 ms of stereo samples */
#define AUDIO_LATE 32768
#define SAS_VOICES 32

static struct { int reserved, samples, mono, playing; u64 due; s16 ring[AUDIO_RING]; volatile u32 head, tail; } chans[AUDIO_CHANNELS];

static int audio_reserve(int want, int samples, int format) {
    if (want < 0 || want >= AUDIO_CHANNELS) for (want = AUDIO_CHANNELS - 1; want >= 0 && chans[want].reserved; want--) {}
    if (want < 0) return (int)0x80260002u;
    chans[want].reserved = 1; chans[want].samples = samples; chans[want].mono = format == 0x10;
    return want;
}

/* returns how long the game should wait: counted from when this channel's last
 * sound ends, not from now, or a late wake-up would lose sound for good */
static u32 audio_output(int c, int left, int right, u32 buf) {
    if (c < 0 || c >= AUDIO_CHANNELS || !chans[c].reserved) return 0;
    const int n = chans[c].samples, step = chans[c].mono ? 2 : 4;
    const u32 head = chans[c].head;
    if (head - chans[c].tail + (u32)n * 2 <= AUDIO_RING) {
        for (int i = 0; i < n; i++) {
            const int l = (s16)RD16(buf + (u32)(i * step)), r = chans[c].mono ? l : (s16)RD16(buf + (u32)(i * step) + 2);
            chans[c].ring[(head + (u32)i * 2) & (AUDIO_RING - 1)] = (s16)(l * left / 0x8000);
            chans[c].ring[(head + (u32)i * 2 + 1) & (AUDIO_RING - 1)] = (s16)(r * right / 0x8000);
        }
        chans[c].head = head + (u32)n * 2;
    }
    if (chans[c].due + 100000 < now_us) chans[c].due = now_us;
    chans[c].due += n * 1000000ull / 44100;
    return chans[c].due > now_us ? (u32)(chans[c].due - now_us) : 0;
}

/* adds what's waiting into the Mac's buffer of stereo samples */
static void audio_drain(s16 *out, int frames) {
    memset(out, 0, (size_t)frames * 4);
    for (int c = 0; c < AUDIO_CHANNELS; c++) {
        u32 tail = chans[c].tail;
        const u32 head = chans[c].head;
        if (head - tail > AUDIO_LATE) tail = head - AUDIO_READY;       /* fallen behind: catch up */
        if (!chans[c].playing && head - tail < AUDIO_READY) continue;  /* gather a little before playing, so bursts come out smooth */
        chans[c].playing = 1;
        if (head - tail < (u32)frames * 2) chans[c].playing = 0;
        for (int i = 0; i < frames * 2 && tail != head; i++, tail++) {
            const int v = out[i] + chans[c].ring[tail & (AUDIO_RING - 1)];
            out[i] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        chans[c].tail = tail;
    }
}

typedef struct {
    u32 vag, size, at, loop_at; int loop, on, ended, pitch, left, right;
    int hist1, hist2, count; u32 frac; s16 block[28]; int have; float level; int releasing;
} SasVoice;
static SasVoice voices[SAS_VOICES];
static int sas_grain = 1024;

static int vag_block(SasVoice *v) {
    static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    if (v->at + 16 > v->size) return 0;
    const u32 a = v->vag + v->at;
    const int head = RD8(a), flags = RD8(a + 1), shift = head & 15, filter = (head >> 4) & 7;
    if (flags == 7) return 0;
    if (flags == 6) v->loop_at = v->at;     /* a sample loops only where its own markers say, whatever the voice asks */
    v->at += 16;
    if (filter > 4) return 0;
    for (int i = 0; i < 28; i++) {
        const int byte = RD8(a + 2 + (u32)(i / 2)), nib = i & 1 ? byte >> 4 : byte & 15;
        int s = (s16)(nib << 12) >> (shift > 12 ? 12 : shift);
        s += (v->hist1 * f0[filter] + v->hist2 * f1[filter]) >> 6;
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        v->block[i] = (s16)s; v->hist2 = v->hist1; v->hist1 = s;
    }
    if (flags == 3 && v->loop) v->at = v->loop_at;
    else if (flags & 1) v->size = v->at;
    v->have = 28; v->count = 0;
    return 1;
}

static void sas_mix(u32 out) {
    static int mix[4096];
    const int n = sas_grain > 2048 ? 2048 : sas_grain;
    memset(mix, 0, (size_t)n * 2 * sizeof *mix);
    for (int k = 0; k < SAS_VOICES; k++) {
        SasVoice *v = &voices[k];
        if (!v->on) continue;
        for (int i = 0; i < n; i++) {
            while (v->frac >= 0x1000) {
                v->frac -= 0x1000;
                if (++v->count >= v->have && !vag_block(v)) { v->on = 0; v->ended = 1; break; }
            }
            if (!v->on) break;
            if (v->releasing) { v->level -= 1 / 1500.f; if (v->level <= 0) { v->on = 0; v->ended = 1; break; } }
            const int s = (int)(v->block[v->count] * v->level);
            mix[i * 2] += s * v->left / 0x1000; mix[i * 2 + 1] += s * v->right / 0x1000;
            v->frac += (u32)v->pitch;
        }
    }
    for (int i = 0; i < n * 2; i++) {
        const int s = mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i];
        WR16(out + (u32)i * 2, (u16)s);
    }
}

static void sas_key_on(int k) {
    SasVoice *v = &voices[k];
    v->at = v->loop_at = 0; v->hist1 = v->hist2 = 0; v->frac = 0; v->count = 0; v->have = 0; v->level = 1; v->releasing = 0; v->ended = 0;
    v->on = vag_block(v);
    if (!v->on) v->ended = 1;
}

static u32 sas_end_flags(void) {
    u32 flags = 0;
    for (int k = 0; k < SAS_VOICES; k++) if (!voices[k].on) flags |= 1u << k;
    return flags;
}
