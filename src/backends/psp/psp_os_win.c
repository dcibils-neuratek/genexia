/* psp_os.h for Windows. Kept apart so windows.h never meets the runtime's names. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <stdlib.h>
#include <wchar.h>
#include "psp_os.h"

static wchar_t *wide(const char *s, wchar_t *out, int room) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, room)) out[0] = 0;
    return out;
}

void *psp_reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

FILE *psp_fopen(const char *path, const char *mode) {
    wchar_t p[1024], m[8];
    return _wfopen(wide(path, p, 1024), wide(mode, m, 8));
}

int psp_mkdir(const char *path) { wchar_t p[1024]; return _wmkdir(wide(path, p, 1024)); }
int psp_chdir(const char *path) { wchar_t p[1024]; return _wchdir(wide(path, p, 1024)); }

int psp_replace(const char *from, const char *to) {
    wchar_t a[1024], b[1024];
    return MoveFileExW(wide(from, a, 1024), wide(to, b, 1024), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int psp_setenv(const char *name, const char *value) { return getenv(name) ? 0 : _putenv_s(name, value); }
