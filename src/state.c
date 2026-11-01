/*
 * state.c - whole-machine save states.
 *
 * All emulated state is tagged EMU_STATE and lives in the __DATA,__emustate
 * section, so saving the machine is one memcpy of that section. This is what
 * run-ahead (input-lag reduction) and the save/load slot are built on.
 */
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state.h"

static u8 *sec;
static unsigned long sec_size;

static void state_init(void)
{
    if (!sec) sec = getsectiondata(&_mh_execute_header, "__DATA", "__emustate", &sec_size);
}

size_t state_size(void) { state_init(); return sec_size; }
void state_save(void *buf) { state_init(); memcpy(buf, sec, sec_size); }
void state_load(const void *buf) { state_init(); memcpy(sec, buf, sec_size); }

/* file slot: header + raw section; refused if this build's layout differs */
#define MAGIC 0x53463253u   /* "SF2S" */
int state_write_file(const char *path)
{
    state_init();
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    u32 hdr[2] = { MAGIC, (u32)sec_size };
    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(sec, sec_size, 1, f) == 1;
    fclose(f);
    return ok;
}
int state_read_file(const char *path)
{
    state_init();
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    u32 hdr[2];
    int ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == MAGIC && hdr[1] == sec_size;
    void *buf = ok ? malloc(sec_size) : NULL;
    if (buf && fread(buf, sec_size, 1, f) == 1) memcpy(sec, buf, sec_size); else ok = 0;
    free(buf);
    fclose(f);
    return ok;
}
