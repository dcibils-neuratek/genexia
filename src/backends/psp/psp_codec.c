/* Music and movies: ATRAC3+ and H.264, decoded by FFmpeg's libavcodec. Included by
 * psp_runtime.c. A movie arrives as a stream of 2048-byte packets, which are split
 * here into video and sound the moment the game has fetched them. */
#include <libavcodec/avcodec.h>

#define MPEG_NO_DATA 0x80618001u

static AVPacket *packet;
static AVFrame *frame;

static AVCodecContext *codec_open(enum AVCodecID id, int block) {
    if (!packet) { packet = av_packet_alloc(); frame = av_frame_alloc(); av_log_set_level(AV_LOG_FATAL); }
    const AVCodec *decoder = avcodec_find_decoder(id);
    AVCodecContext *c = decoder ? avcodec_alloc_context3(decoder) : NULL;
    if (c && block) { c->sample_rate = 44100; c->block_align = block; av_channel_layout_default(&c->ch_layout, 2); }
    if (c && avcodec_open2(c, decoder, NULL) < 0) avcodec_free_context(&c);
    return c;
}

/* one block of ATRAC3+ into 16-bit stereo; how many samples came out */
static int atrac_block(AVCodecContext *c, const u8 *in, int size, s16 *out) {
    packet->data = (u8 *)in; packet->size = size;
    if (!c || avcodec_send_packet(c, packet) < 0 || avcodec_receive_frame(c, frame) < 0) return 0;
    const float *left = (const float *)frame->extended_data[0];
    const float *right = (const float *)frame->extended_data[frame->ch_layout.nb_channels > 1];
    for (int i = 0; i < frame->nb_samples; i++) {
        const float l = left[i] * 32767.f, r = right[i] * 32767.f;
        out[i * 2] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[i * 2 + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    return frame->nb_samples;
}

/* sceAtrac: the game streams a file through a buffer of its own. Everything it adds is
 * also kept here, in file order, so a loop never has to fetch anything again. */
#define ATRAC_IDS 6
#define ATRAC_DECODED 0x80630024u
#define ATRAC_EMPTY 0x80630023u
static struct Atrac {
    int used, block, loops;
    u32 buf, size, length, filled, at, start, end, loop_at;
    u8 *file;
    AVCodecContext *c;
} atracs[ATRAC_IDS];

static struct Atrac *atrac_of(u32 id) { return id < ATRAC_IDS && atracs[id].used ? &atracs[id] : NULL; }

static u32 le32(const u8 *b) { return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }

static u32 atrac_set_data(u32 buf, u32 size) {
    int id = 0;
    while (id < ATRAC_IDS && atracs[id].used) id++;
    if (id == ATRAC_IDS) return 0x80630009u;
    const u8 *h = M(buf);
    if (size < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0x80630006u;
    struct Atrac a = { .used = 1, .buf = buf, .size = size, .length = le32(h + 4) + 8, .loop_at = ~0u };
    for (u32 at = 12; at + 8 <= size;) {
        const u32 chunk = le32(h + at + 4);
        if (!memcmp(h + at, "fmt ", 4)) a.block = h[at + 20] | h[at + 21] << 8;
        else if (!memcmp(h + at, "smpl", 4) && le32(h + at + 36)) a.loop_at = le32(h + at + 8 + 36 + 8);
        else if (!memcmp(h + at, "data", 4)) { a.start = at + 8; a.end = at + 8 + chunk; break; }
        at += 8 + chunk + (chunk & 1);
    }
    if (!a.start || !a.block) return 0x80630006u;
    if (a.loop_at != ~0u) a.loop_at = a.start + a.loop_at / 2048 * a.block;
    if (a.end > a.length) a.end = a.length;
    a.file = malloc(a.length + AV_INPUT_BUFFER_PADDING_SIZE);
    a.filled = size < a.length ? size : a.length;
    memcpy(a.file, h, a.filled);
    a.at = a.start;
    a.c = codec_open(AV_CODEC_ID_ATRAC3P, a.block);
    atracs[id] = a;
    return (u32)id;
}

static void atrac_release(struct Atrac *a) {
    avcodec_free_context(&a->c);
    free(a->file);
    memset(a, 0, sizeof *a);
}

/* how much the game may write now, at which place in its buffer, from where in the file */
static void atrac_stream_info(struct Atrac *a, u32 *write, u32 *room, u32 *from) {
    const u32 pos = a->filled % a->size;
    *write = a->buf + pos; *from = a->filled;
    *room = a->size - (a->filled - a->at < a->size ? a->filled - a->at : a->size);
    if (*room > a->size - pos) *room = a->size - pos;
    if (*room > a->length - a->filled) *room = a->length - a->filled;
}

static void atrac_add(struct Atrac *a, u32 bytes) {
    u32 write, room, from;
    atrac_stream_info(a, &write, &room, &from);
    if (bytes > room) bytes = room;
    memcpy(a->file + a->filled, M(write), bytes);
    a->filled += bytes;
}

static u32 atrac_remain(const struct Atrac *a) {
    if (a->size >= a->length) return 0xFFFFFFFFu;
    if (a->filled >= a->length) return a->loops ? 0xFFFFFFFDu : 0xFFFFFFFEu;
    return (a->filled - a->at) / a->block;
}

static u32 atrac_decode(struct Atrac *a, u32 out, u32 *finished) {
    *finished = 0;
    if (a->at + a->block > a->end) {
        *finished = 1;
        return ATRAC_DECODED;
    }
    if (a->at + a->block > a->filled) return ATRAC_EMPTY;
    const int samples = atrac_block(a->c, a->file + a->at, a->block, (s16 *)M(out));
    if (!samples) memset(M(out), 0, 2048 * 4);
    a->at += a->block;
    if (a->at + a->block > a->end) {
        if (a->loops && a->loop_at < a->end) { a->at = a->loop_at; if (a->loops > 0) a->loops--; }
        else *finished = 1;
    }
    return samples ? (u32)samples : 2048;
}

/* sceMpeg */
static struct {
    u32 ring, packets, data, wrote, free;
    u8 *video, *audio;
    size_t video_size, video_len, video_at, video_scan, audio_size, audio_len, audio_at;
    u32 *ends, oldest, held;          /* where in the video each packet held in the ring ends */
    u32 video_pts, audio_pts;
    int got_video, got_audio, width, height, decoded;
    struct { u32 at, len; } aus[32];
    int au_first, au_count;
    AVCodecContext *avc, *at3;
} mp;

static void mpeg_reset(u32 ring, u32 packets, u32 data) {
    avcodec_free_context(&mp.avc);
    avcodec_free_context(&mp.at3);
    free(mp.video); free(mp.audio); free(mp.ends);
    memset(&mp, 0, sizeof mp);
    mp.ring = ring; mp.packets = mp.free = packets; mp.data = data;
    mp.ends = calloc(packets + 1, sizeof *mp.ends);
    mp.avc = codec_open(AV_CODEC_ID_H264, 0);
}

static void es_add(u8 **es, size_t *size, size_t *len, const u8 *from, size_t n) {
    if (*len + n + AV_INPUT_BUFFER_PADDING_SIZE > *size) {
        *size = (*size ? *size * 2 : 1 << 20) + n;
        *es = realloc(*es, *size);
    }
    memcpy(*es + *len, from, n);
    *len += n;
}

static u32 pes_pts(const u8 *b) { return (u32)(b[0] & 0x0E) << 29 | b[1] << 22 | (b[2] & 0xFE) << 14 | b[3] << 7 | b[4] >> 1; }

static void mpeg_split(const u8 *pack) {
    if (pack[0] || pack[1] || pack[2] != 1 || pack[3] != 0xBA) return;
    for (int j = 14 + (pack[13] & 7); j + 9 <= 2048 && !pack[j] && !pack[j + 1] && pack[j + 2] == 1;) {
        const int id = pack[j + 3], len = pack[j + 4] << 8 | pack[j + 5], end = j + 6 + len < 2048 ? j + 6 + len : 2048, body = j + 9 + pack[j + 8];
        if (id == 0xE0 && body < end) {
            if (!mp.got_video && pack[j + 7] & 0x80) { mp.got_video = 1; mp.video_pts = pes_pts(pack + j + 9); }
            es_add(&mp.video, &mp.video_size, &mp.video_len, pack + body, (size_t)(end - body));
        } else if (id == 0xBD && body + 4 < end) {
            if (!mp.got_audio && pack[j + 7] & 0x80) { mp.got_audio = 1; mp.audio_pts = pes_pts(pack + j + 9); }
            es_add(&mp.audio, &mp.audio_size, &mp.audio_len, pack + body + 4, (size_t)(end - body - 4));
        }
        j += 6 + len;
    }
}

/* the game's callback has written some packets into the ring */
static void mpeg_put_done(s32 n) {
    if (!mp.ends) return;
    for (int i = 0; i < n; i++) {
        mpeg_split(M(mp.data + (mp.wrote + (u32)i) % mp.packets * 2048));
        mp.ends[(mp.oldest + mp.held) % mp.packets] = (u32)mp.video_len;
        if (mp.held < mp.packets) { mp.held++; mp.free--; }
    }
    if (n > 0) mp.wrote = (mp.wrote + (u32)n) % mp.packets;
    WR32(mp.ring + 8, mp.wrote); WR32(mp.ring + 12, mp.free);
}

static void mpeg_release(void) {
    while (mp.held && mp.ends[mp.oldest] <= mp.video_at) { mp.oldest = (mp.oldest + 1) % mp.packets; mp.held--; mp.free++; }
    WR32(mp.ring + 12, mp.free);
}

/* each frame opens with the marker 00 00 01 09 */
static int au_marker(const u8 *b) { return !b[0] && !b[1] && b[2] == 1 && b[3] == 9; }

static u32 mpeg_next_video(u32 au) {
    size_t scan = mp.video_scan > mp.video_at + 4 ? mp.video_scan : mp.video_at + 4;
    while (scan + 4 <= mp.video_len && !au_marker(mp.video + scan)) scan++;
    mp.video_scan = scan;
    if (scan + 4 > mp.video_len || mp.au_count == 32) {
        if (mp.au_count < 32) {        /* all fetched has been read: the ring is empty */
            mp.oldest = (mp.oldest + mp.held) % mp.packets; mp.free += mp.held; mp.held = 0;
            WR32(mp.ring + 12, mp.free);
        }
        return MPEG_NO_DATA;
    }
    if (scan > 0 && !mp.video[scan - 1]) scan--;      /* its leading zero, when there is one */
    const int slot = (mp.au_first + mp.au_count++) % 32;
    mp.aus[slot].at = (u32)mp.video_at; mp.aus[slot].len = (u32)(scan - mp.video_at);
    mp.video_at = scan;
    WR32(au, 0); WR32(au + 4, mp.video_pts); WR32(au + 8, 0); WR32(au + 12, mp.video_pts);
    mp.video_pts += 3003;
    mpeg_release();
    return 0;
}

/* to the PSP's 32-bit pixels, one frame into the game's buffer */
static void yuv_to_rgba(const AVFrame *f, u32 *out, int stride) {
    for (int y = 0; y < f->height && y < 272; y++) {
        const u8 *ly = f->data[0] + y * f->linesize[0];
        const u8 *lu = f->data[1] + (y / 2) * f->linesize[1];
        const u8 *lv = f->data[2] + (y / 2) * f->linesize[2];
        for (int x = 0; x < f->width && x < stride; x++) {
            const int c = 298 * (ly[x] - 16), d = lu[x / 2] - 128, e = lv[x / 2] - 128;
            int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            out[y * stride + x] = 0xFF000000u | (u32)b << 16 | (u32)g << 8 | (u32)r;
        }
    }
}

static int mpeg_decode_video(int stride, u32 out) {
    if (!mp.au_count || !mp.avc) return 0;
    packet->data = mp.video + mp.aus[mp.au_first].at; packet->size = (int)mp.aus[mp.au_first].len;
    mp.au_first = (mp.au_first + 1) % 32; mp.au_count--;
    int shown = 0;
    avcodec_send_packet(mp.avc, packet);
    while (avcodec_receive_frame(mp.avc, frame) >= 0) {
        mp.width = frame->width; mp.height = frame->height; mp.decoded++;
        yuv_to_rgba(frame, (u32 *)M(out), stride);
        shown = 1;
    }
    return shown;
}

/* sound frames carry 8 bytes of header, which says how long they are */
static int audio_frame_size(void) {
    while (mp.audio_at + 8 <= mp.audio_len && !(mp.audio[mp.audio_at] == 0x0F && mp.audio[mp.audio_at + 1] == 0xD0)) mp.audio_at++;
    if (mp.audio_at + 8 > mp.audio_len) return 0;
    const u8 *h = mp.audio + mp.audio_at;
    const int size = ((h[2] & 3) << 8 | h[3]) * 8 + 8;
    return mp.audio_at + 8 + size <= mp.audio_len ? size : 0;
}

static u32 mpeg_next_audio(u32 au) {
    if (!audio_frame_size()) return MPEG_NO_DATA;
    WR32(au, 0); WR32(au + 4, mp.audio_pts); WR32(au + 8, 0); WR32(au + 12, mp.audio_pts);
    return 0;
}

static void mpeg_decode_audio(u32 out) {
    const int size = audio_frame_size();
    if (size && !mp.at3) mp.at3 = codec_open(AV_CODEC_ID_ATRAC3P, size);
    if (!size || !atrac_block(mp.at3, mp.audio + mp.audio_at + 8, size, (s16 *)M(out))) memset(M(out), 0, 2048 * 4);
    if (size) mp.audio_at += 8 + (size_t)size;
    mp.audio_pts += 4180;
}
