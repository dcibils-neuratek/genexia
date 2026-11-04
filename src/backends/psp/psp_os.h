/* The few things the runtime needs from the system that Windows does differently:
 * the PSP's memory, folders, and file names (UTF-8 everywhere; Windows wants them wide). */
#ifndef PSP_OS_H
#define PSP_OS_H
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>

#ifdef _WIN32
void *psp_reserve(size_t n);                        /* zeroed, read-write */
FILE *psp_fopen(const char *path, const char *mode);
int psp_mkdir(const char *path);
int psp_chdir(const char *path);
int psp_replace(const char *from, const char *to);  /* rename, over an existing file too */
int psp_setenv(const char *name, const char *value);
/* calls fn with each file in dir (which ends in a slash) until it returns nonzero */
int psp_each_file(const char *dir, int (*fn)(const char *path, void *ctx), void *ctx);
#else
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
static inline void *psp_reserve(size_t n) {
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    return m == MAP_FAILED ? NULL : m;
}
#define psp_fopen fopen
static inline int psp_mkdir(const char *path) { return mkdir(path, 0755); }
#define psp_chdir chdir
#define psp_replace rename
static inline int psp_setenv(const char *name, const char *value) { return setenv(name, value, 0); }
#include <dirent.h>
static inline int psp_each_file(const char *dir, int (*fn)(const char *path, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    const struct dirent *e; char path[2048]; int r = 0;
    while (!r && (e = readdir(d))) { snprintf(path, sizeof path, "%s%s", dir, e->d_name); r = fn(path, ctx); }
    closedir(d);
    return r;
}
#endif
#endif
