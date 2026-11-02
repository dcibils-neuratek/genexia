/* Saving: the system's save dialog, without the dialog. Each save is a folder under
 * saves/ beside the game, named as the PSP names it, holding the game's data as is.
 * Included by psp_runtime.c. */
#include <sys/stat.h>
#include <dirent.h>

#define SAVE_NO_DATA    0x80110307u
#define SAVE_LOAD_BROKEN 0x80110306u
#define SAVE_WRITE_FAIL 0x80110385u

/* offsets into the game's request */
#define SAVE_RESULT    28
#define SAVE_MODE      48
#define SAVE_GAME      60
#define SAVE_NAME      76
#define SAVE_LIST      96
#define SAVE_FILE      100
#define SAVE_BUF       116
#define SAVE_BUF_SIZE  120
#define SAVE_SIZE      124

static int save_status = -1;

/* a dialog steps from starting to shown to finished; closing it reads 0 once, then asking is an error */
static u32 dialog_status(int *status) {
    const int now = *status;
    if (now < 0) return 0x80110005u;
    *status = now == 1 ? 2 : now == 4 ? 0 : now == 0 ? -1 : now;
    return (u32)now;
}

/* folder and file names come from the game, so nothing but plain characters reach the path */
static void save_clean(char *out, size_t room, const u8 *name, size_t most) {
    size_t n = 0;
    for (size_t i = 0; i < most && name[i] && n + 1 < room; i++)
        out[n++] = isalnum(name[i]) || name[i] == '_' || name[i] == '-' || (name[i] == '.' && i) ? (char)name[i] : '_';
    out[n] = 0;
}

static void save_folder(u32 q, const u8 *name, char *out, size_t room) {
    char game[16], save[24];
    save_clean(game, sizeof game, M(q + SAVE_GAME), 13);
    save_clean(save, sizeof save, name, 20);
    snprintf(out, room, "saves/%s%s", game, save);
}

static int save_exists(u32 q, const u8 *name) {
    char dir[64]; struct stat st;
    save_folder(q, name, dir, sizeof dir);
    return !stat(dir, &st);
}

/* when the game offers a list of names: for loading, the first that exists; else the first */
static const u8 *save_pick(u32 q, int loading) {
    const u32 list = RD32(q + SAVE_LIST);
    if (!list) return M(q + SAVE_NAME);
    for (u32 at = list; *M(at); at += 20) if (!loading || save_exists(q, M(at))) return M(at);
    return loading ? M(q + SAVE_NAME) : M(list);
}

static u32 save_write(u32 q, const u8 *name) {
    char dir[64], file[16], path[96];
    save_folder(q, name, dir, sizeof dir);
    save_clean(file, sizeof file, M(q + SAVE_FILE), 13);
    mkdir("saves", 0755); mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    char temporary[104];
    snprintf(temporary, sizeof temporary, "%s.new", path);    /* a crash mid-write leaves the old save whole */
    FILE *f = fopen(temporary, "wb");
    if (!f) return SAVE_WRITE_FAIL;
    const u32 size = RD32(q + SAVE_SIZE);
    const size_t wrote = fwrite(M(RD32(q + SAVE_BUF)), 1, size, f);
    if (fclose(f) || wrote != size || rename(temporary, path)) return SAVE_WRITE_FAIL;
    return 0;
}

static u32 save_read(u32 q, const u8 *name) {
    char dir[64], file[16], path[96];
    save_folder(q, name, dir, sizeof dir);
    save_clean(file, sizeof file, M(q + SAVE_FILE), 13);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return SAVE_NO_DATA;
    const size_t got = fread(M(RD32(q + SAVE_BUF)), 1, RD32(q + SAVE_BUF_SIZE), f);
    fclose(f);
    WR32(q + SAVE_SIZE, (u32)got);
    return 0;
}

static void save_start(u32 q) {
    const u32 mode = RD32(q + SAVE_MODE);
    const u8 *name = save_pick(q, mode == 0 || mode == 2 || mode == 4);
    u32 result = 0;
    if (mode == 0 || mode == 2 || mode == 4) result = save_read(q, name);
    else if (mode == 1 || mode == 3 || mode == 5) result = save_write(q, name);
    else fprintf(lg, "save mode %u not done\n", mode);
    if (name != M(q + SAVE_NAME)) memmove(M(q + SAVE_NAME), name, 20);
    WR32(q + SAVE_RESULT, result);
    fprintf(lg, "%.1f save mode %u %.13s%.20s %s %u bytes: %08X\n", now_us / 1e6, mode, M(q + SAVE_GAME), name, M(q + SAVE_FILE), RD32(q + SAVE_SIZE), result);
    save_status = 1;
}

/* the game looks for its saves by listing the Memory Stick's save folder */
#define SAVE_ROOT "PSP/SAVEDATA"
static DIR *listings[4];
static char listing_paths[4][64];

static u32 save_dir_open(const char *path) {
    char dir[64] = "saves";
    fprintf(lg, "dopen %s\n", path);
    if (strncmp(path, "ms0:", 4)) return 0x80010002u;
    path += path[4] == '/' ? 5 : 4;
    if (strncmp(path, SAVE_ROOT, strlen(SAVE_ROOT))) return 0x80010002u;
    const char *below = path + strlen(SAVE_ROOT);
    if (*below == '/') below++;
    if (*below) { char name[24]; save_clean(name, sizeof name, (const u8 *)below, strcspn(below, "/")); snprintf(dir, sizeof dir, "saves/%s", name); }
    for (int i = 0; i < 4; i++) if (!listing_paths[i][0]) {
        listings[i] = opendir(dir);
        if (!listings[i] && strcmp(dir, "saves")) return 0x80010002u;
        strcpy(listing_paths[i], dir);
        return 0x7000u + (u32)i;
    }
    return 0x80020320u;
}

static u32 save_dir_read(u32 fd, u32 out) {
    const u32 i = fd - 0x7000u;
    if (i >= 4 || !listing_paths[i][0]) return 0x80020323u;
    const struct dirent *e = listings[i] ? readdir(listings[i]) : NULL;
    if (!e) return 0;
    char path[320]; struct stat st = {0};
    snprintf(path, sizeof path, "%s/%s", listing_paths[i], e->d_name);
    stat(path, &st);
    memset(M(out), 0, 0x58 + 256);
    const int folder = S_ISDIR(st.st_mode);
    WR32(out, folder ? 0x11FF : 0x21FF); WR32(out + 4, folder ? 0x10 : 0x20); WR32(out + 8, (u32)st.st_size);
    strncpy((char *)M(out + 0x58), e->d_name, 255);
    return 1;
}

static u32 save_dir_close(u32 fd) {
    const u32 i = fd - 0x7000u;
    if (i >= 4 || !listing_paths[i][0]) return 0x80020323u;
    if (listings[i]) closedir(listings[i]);
    listings[i] = NULL; listing_paths[i][0] = 0;
    return 0;
}
