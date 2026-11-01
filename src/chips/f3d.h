/* f3d.h - Fast3D display lists drawn with OpenGL; see f3d.c */
#ifndef F3D_H
#define F3D_H
#include "f3d_gl.h"
#include "n64.h"

typedef struct { long frames, draws, tris, tex_uploads, cc_approx, cc_log; } f3d_stats_t;
extern f3d_stats_t f3d_stats;

void f3d_init(void);           /* GL context must be current */
void f3d_run(u32 dl);          /* one graphics task's display list */
#endif
