/*
 * ps1_cd.c - the CD-ROM drive: a cue sheet's tracks, the controller's
 * commands and their two-step answers, sector reading at single or double
 * speed, and the data FIFO.
 *
 * An answer waits until the game has acknowledged the one before it, as the
 * real controller's does.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include "ps1.h"

#define SECTOR 2352
#define PERIOD_1X (PS1_CLOCK / 75)

static struct { FILE *f; u32 start, file_lba; int audio; } tracks[100];
static int ntracks;
static u32 disc_end;
static char region = 'A';

static u8 index_reg, int_enable, int_flag, stat = 0x02, mode;
static u8 params[16], nparams;
static u8 resp[16], nresp, resp_at;
static u8 sector[SECTOR];
static u8 data[SECTOR];
static int data_len, data_at;
static u32 setloc, lba;
static int reading, playing, setloc_pending, sector_pending;
static u8 filter_file, filter_chan;

typedef struct { u8 type, n, b[12]; u32 delay; } Answer;
int cd_lost;
static Answer queue[4];
static int nqueue;
static int busy;

/* ---- the disc ---- */

static u32 msf_lba(int m, int s, int f) { return (u32)(m * 60 + s) * 75 + f; }
static u8 bcd(int v) { return (u8)(v / 10 * 16 + v % 10); }
static int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

int cd_open(const char *path) {
    const size_t n = strlen(path);
    if (n > 4 && strcasecmp(path + n - 4, ".cue") == 0) {
        FILE *cue = fopen(path, "r");
        if (!cue) return 0;
        char dir[1024], line[1024];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else dir[0] = 0;
        FILE *f = NULL;
        u32 file_base = 0, file_len = 0;
        int track = -1;
        while (fgets(line, sizeof line, cue)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "FILE", 4) == 0) {
                char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (!q1 || !q2) continue;
                *q2 = 0;
                char full[2048];
                snprintf(full, sizeof full, "%s%s", q1[0 + 1] == '/' ? "" : dir, q1 + 1);
                file_base += file_len;
                f = fopen(full, "rb");
                if (!f) { fprintf(stderr, "cd: can't open %s\n", full); fclose(cue); return 0; }
                fseek(f, 0, SEEK_END);
                file_len = (u32)(ftell(f) / SECTOR);
            } else if (strncmp(p, "TRACK", 5) == 0) {
                track = atoi(p + 5);
                if (track < 1 || track > 99) continue;
                tracks[track].f = f;
                tracks[track].file_lba = file_base;
                tracks[track].audio = strstr(p, "AUDIO") != NULL;
                if (track > ntracks) ntracks = track;
            } else if (strncmp(p, "INDEX 01", 8) == 0 && track > 0) {
                int m, s, fr;
                if (sscanf(p + 8, "%d:%d:%d", &m, &s, &fr) == 3) tracks[track].start = file_base + msf_lba(m, s, fr);
            }
        }
        fclose(cue);
        disc_end = file_base + file_len;
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        tracks[1].f = f;
        ntracks = 1;
        disc_end = (u32)(ftell(f) / SECTOR);
    }
    u8 lic[SECTOR];
    if (cd_read_sector(4, lic)) {
        if (memmem(lic, SECTOR, "Europe", 6)) region = 'E';
        else if (memmem(lic, SECTOR, "Inc.", 4)) region = 'I';
    }
    return ntracks > 0;
}

/* sector at disc address lba (0 is 00:02:00) */
int cd_read_sector(u32 at, u8 *out) {
    int t = 1;
    for (int i = 1; i <= ntracks; i++) if (tracks[i].f && at >= tracks[i].start) t = i;
    if (!tracks[t].f || at >= disc_end) { memset(out, 0, SECTOR); return 0; }
    fseeko(tracks[t].f, (off_t)(at - tracks[t].file_lba) * SECTOR, SEEK_SET);
    return fread(out, 1, SECTOR, tracks[t].f) == SECTOR;
}

/* ---- answers ---- */

static void irq_check(void) { if (int_flag & int_enable & 0x1f) irq_raise(2); }

static void answer(int type, u32 delay, int n, ...) {
    if (nqueue == 4) return;
    Answer *a = &queue[nqueue++];
    a->type = type; a->n = n; a->delay = delay;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a->b[i] = (u8)va_arg(ap, int);
    va_end(ap);
    if (nqueue == 1) event_at(EV_CD, ps1_now + delay);
}

static void deliver_sector(void) {
    sector_pending = 0;
    nresp = 1; resp_at = 0; resp[0] = stat;
    int_flag = (int_flag & ~7) | 1;
    irq_check();
}

void cd_event(void) {
    if (!nqueue) return;
    if (int_flag & 7) { event_at(EV_CD, ps1_now + 2000); return; }    /* wait for the last one to be acknowledged */
    Answer a = queue[0];
    memmove(queue, queue + 1, sizeof(Answer) * --nqueue);
    if (a.type == 3) busy = 0;
    if (a.type == 0) {          /* not an answer: a state change when its time comes */
        if (a.b[0] == 1) { reading = 1; event_at(EV_CD_SECTOR, ps1_now + (mode & 0x80 ? PERIOD_1X / 2 : PERIOD_1X)); }
    } else {
        memcpy(resp, a.b, a.n);
        nresp = a.n; resp_at = 0;
        int_flag = (int_flag & ~7) | a.type;
        irq_check();
    }
    if (nqueue) event_at(EV_CD, ps1_now + queue[0].delay);
}

void cd_sector_event(void) {
    if (!reading) return;
    cd_read_sector(lba, sector);
    lba++;
    event_at(EV_CD_SECTOR, ps1_now + (mode & 0x80 ? PERIOD_1X / 2 : PERIOD_1X));
    const u8 submode = sector[18];
    if ((mode & 0x40) && sector[15] == 2 && (submode & 0x04)) {   /* XA audio: for the SPU, not the game */
        return;
    }
    if (int_flag & 7) { if (sector_pending) cd_lost++; sector_pending = 1; return; }
    deliver_sector();
}

/* ---- commands ---- */

static void start_read(void) {
    if (setloc_pending) { lba = setloc; setloc_pending = 0; }
    reading = 0;
    playing = 0;
    stat = (stat & ~0xe0) | 0x22;
    event_off(EV_CD_SECTOR);
    answer(0, 60000, 1, 1);     /* start once it has seeked */
}

static void command(u8 c) {
    const u32 ack = 25000;
    int_flag &= ~0x18;
    busy = 1;
    if (getenv("PS1_TRACE_CD")) { fprintf(stderr, "cd %02x", c); for (int i = 0; i < nparams; i++) fprintf(stderr, " %02x", params[i]); fprintf(stderr, " at %.2f s\n", (double)ps1_now / PS1_CLOCK); }
    switch (c) {
    case 0x01: answer(3, ack, 1, stat); stat &= ~0x10; break;
    case 0x02: setloc = msf_lba(unbcd(params[0]), unbcd(params[1]), unbcd(params[2])) - 150; setloc_pending = 1; answer(3, ack, 1, stat); break;
    case 0x03:
        if (setloc_pending) { lba = setloc; setloc_pending = 0; }
        playing = 1; stat = (stat & ~0xe0) | 0x82;
        answer(3, ack, 1, stat);
        break;
    case 0x06: case 0x1b: answer(3, ack, 1, stat | 0x02); start_read(); break;
    case 0x07: stat |= 0x02; answer(3, ack, 1, stat); answer(2, 100000, 1, stat); break;
    case 0x08:
        reading = playing = 0; event_off(EV_CD_SECTOR);
        answer(3, ack, 1, stat); stat &= ~0xe2; answer(2, 100000, 1, stat);
        break;
    case 0x09:
        answer(3, ack, 1, stat);
        reading = playing = 0; event_off(EV_CD_SECTOR); sector_pending = 0;
        stat &= ~0xe0;
        answer(2, mode & 0x80 ? 70000 : 140000, 1, stat);
        break;
    case 0x0a:
        reading = playing = 0; event_off(EV_CD_SECTOR);
        mode = 0x20; stat = 0x02;
        answer(3, 80000, 1, stat); answer(2, 120000, 1, stat);
        break;
    case 0x0b: case 0x0c: answer(3, ack, 1, stat); break;
    case 0x0d: filter_file = params[0]; filter_chan = params[1]; answer(3, ack, 1, stat); break;
    case 0x0e: mode = params[0]; answer(3, ack, 1, stat); break;
    case 0x0f: answer(3, ack, 5, stat, mode, 0, filter_file, filter_chan); break;
    case 0x10: answer(3, ack, 8, sector[12], sector[13], sector[14], sector[15], sector[16], sector[17], sector[18], sector[19]); break;
    case 0x11: {
        const u32 a = lba + 150, r = lba - tracks[1].start;
        answer(3, ack, 8, 1, 1, bcd(r / 4500), bcd(r / 75 % 60), bcd(r % 75), bcd(a / 4500), bcd(a / 75 % 60), bcd(a % 75));
        break;
    }
    case 0x13: answer(3, ack, 3, stat, 1, bcd(ntracks)); break;
    case 0x14: {
        const int t = unbcd(params[0]);
        const u32 a = (t == 0 || t > ntracks ? disc_end : tracks[t].start) + 150;
        answer(3, ack, 3, stat, bcd(a / 4500), bcd(a / 75 % 60));
        break;
    }
    case 0x15: case 0x16:
        if (setloc_pending) { lba = setloc; setloc_pending = 0; }
        reading = playing = 0; event_off(EV_CD_SECTOR);
        stat = (stat & ~0xe0) | 0x42;
        answer(3, ack, 1, stat);
        stat &= ~0x40;
        answer(2, 100000, 1, stat);
        break;
    case 0x19:
        if (params[0] == 0x20) answer(3, ack, 4, 0x94, 0x09, 0x19, 0xc0);
        else answer(3, ack, 1, stat);
        break;
    case 0x1a:
        answer(3, ack, 1, stat);
        if (!ntracks) answer(5, 30000, 8, 0x08, 0x40, 0, 0, 0, 0, 0, 0);
        else answer(2, 30000, 8, 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', region);
        break;
    case 0x1c: answer(3, ack, 1, stat); break;
    case 0x1e: answer(3, ack, 1, stat); answer(2, 500000, 1, stat); break;
    default:
        fprintf(stderr, "cd: unknown command %02x\n", c);
        answer(5, ack, 2, stat | 1, 0x40);
        break;
    }
    nparams = 0;
}

static void request(u8 v) {
    if (v & 0x80) {
        if (data_at < data_len) return;
        const int whole = mode & 0x20;
        data_len = whole ? 0x924 : 0x800;
        memcpy(data, sector + (whole ? 12 : 24), data_len);
        data_at = 0;
    } else data_len = data_at = 0;
}

u8 cd_read(int reg) {
    switch (reg) {
    case 0:
        return index_reg | (nparams == 0) << 3 | (nparams < 16) << 4 | (resp_at < nresp) << 5 | (data_at < data_len) << 6 | busy << 7;
    case 1: return resp_at < nresp ? resp[resp_at++] : 0;
    case 2: return data_at < data_len ? data[data_at++] : 0;
    case 3: return index_reg & 1 ? int_flag | 0xe0 : int_enable | 0xe0;
    }
    return 0;
}

void cd_write(int reg, u8 v) {
    if (reg == 0) { index_reg = v & 3; return; }
    switch (reg << 2 | index_reg) {
    case 1 << 2 | 0: command(v); break;
    case 2 << 2 | 0: if (nparams < 16) params[nparams++] = v; break;
    case 2 << 2 | 1: int_enable = v & 0x1f; irq_check(); break;
    case 3 << 2 | 0: request(v); break;
    case 3 << 2 | 1:
        int_flag &= ~(v & 0x1f);
        if (v & 0x40) nparams = 0;
        if (!(int_flag & 7)) {
            nresp = resp_at = 0;
            if (sector_pending) deliver_sector();
        }
        break;
    default: break;     /* volumes, for later */
    }
}

u32 cd_dma_word(void) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) v |= (u32)cd_read(2) << (8 * i);
    return v;
}

void cd_reset(void) {
    stat = 0x02; mode = 0; int_flag = 0; int_enable = 0x1f; nqueue = 0; busy = 0;
    reading = playing = 0;
}
