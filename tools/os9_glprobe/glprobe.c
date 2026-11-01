/*
 * glprobe.c - loads Apple's OpenGL 1.2 for Mac OS 9 by name, so no import
 * library is needed, then writes what it finds to glprobe.log next to itself:
 * version, renderer, limits, and how fast it draws textured triangles.
 */
#include <Multiverse.h>
#include <stdio.h>
#include <string.h>

typedef void *AGLPixelFormat, *AGLContext;
enum { AGL_NONE = 0, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5, AGL_DEPTH_SIZE = 12, AGL_ACCELERATED = 73 };
enum { GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_TEXTURE_UNITS_ARB = 0x84E2,
       GL_DEPTH_BUFFER_BIT = 0x100, GL_COLOR_BUFFER_BIT = 0x4000, GL_TRIANGLES = 4, GL_TEXTURE_2D = 0x0DE1, GL_DEPTH_TEST = 0x0B71,
       GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_LINEAR = 0x2601,
       GL_FLOAT = 0x1406, GL_VERTEX_ARRAY = 0x8074, GL_TEXTURE_COORD_ARRAY = 0x8078, GL_COLOR_ARRAY = 0x8076, GL_SMOOTH = 0x1D01 };

static CFragConnection *lib;

static void *sym(const char *name) {
    Str255 p; Ptr a = NULL; unsigned char c;
    p[0] = (unsigned char)strlen(name); memcpy(p + 1, name, p[0]);
    return FindSymbol(lib, p, &a, &c) == noErr ? a : NULL;
}
static FILE *out;
#define LOG(...) do { fprintf(out, __VA_ARGS__); fflush(out); } while (0)

#define V 3000
static float xyz[V][3], st[V][2];
static unsigned char rgba[V][4], tex[256 * 256 * 4];

int main(void) {
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    out = fopen("glprobe.log", "w");
    if (!out) return 1;
    Ptr mainp; Str255 err;
    OSErr e = GetSharedLibrary("\pOpenGLLibrary", kPowerPCArch, kReferenceCFrag, &lib, &mainp, err);
    LOG("OpenGLLibrary: err %d\n", e);
    if (e) { fclose(out); return 0; }
    AGLPixelFormat (*aglChoosePixelFormat)(const void *, long, const long *) = sym("aglChoosePixelFormat");
    AGLContext (*aglCreateContext)(AGLPixelFormat, AGLContext) = sym("aglCreateContext");
    unsigned char (*aglSetDrawable)(AGLContext, CGrafPtr) = sym("aglSetDrawable");
    unsigned char (*aglSetCurrentContext)(AGLContext) = sym("aglSetCurrentContext");
    void (*aglSwapBuffers)(AGLContext) = sym("aglSwapBuffers");
    unsigned long (*aglGetError)(void) = sym("aglGetError");
    const unsigned char *(*glGetString)(unsigned) = sym("glGetString");
    void (*glGetIntegerv)(unsigned, long *) = sym("glGetIntegerv");
    void (*glClear)(unsigned) = sym("glClear");
    void (*glEnable)(unsigned) = sym("glEnable");
    void (*glEnableClientState)(unsigned) = sym("glEnableClientState");
    void (*glVertexPointer)(long, unsigned, long, const void *) = sym("glVertexPointer");
    void (*glTexCoordPointer)(long, unsigned, long, const void *) = sym("glTexCoordPointer");
    void (*glColorPointer)(long, unsigned, long, const void *) = sym("glColorPointer");
    void (*glDrawArrays)(unsigned, long, long) = sym("glDrawArrays");
    void (*glTexImage2D)(unsigned, long, long, long, long, long, unsigned, unsigned, const void *) = sym("glTexImage2D");
    void (*glTexParameteri)(unsigned, unsigned, long) = sym("glTexParameteri");
    void (*glFinish)(void) = sym("glFinish");
    LOG("agl %d gl %d\n", aglChoosePixelFormat != NULL, glGetString != NULL);
    if (!aglChoosePixelFormat || !glGetString) { fclose(out); return 0; }

    const long attr[] = { AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_ACCELERATED, AGL_NONE };
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attr);
    LOG("accelerated pixel format %p, err %lu\n", pf, aglGetError ? aglGetError() : 0);
    if (!pf) { fclose(out); return 0; }
    AGLContext ctx = aglCreateContext(pf, NULL);
    Rect r; SetRect(&r, 40, 60, 40 + 640, 60 + 480);
    WindowPtr w = NewCWindow(NULL, &r, "\pGL probe", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    aglSetDrawable(ctx, (CGrafPtr)w);
    aglSetCurrentContext(ctx);
    LOG("vendor   %s\nrenderer %s\nversion  %s\next      %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_EXTENSIONS));
    long n = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &n); LOG("max texture %ld\n", n);
    n = 0; glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &n); LOG("texture units %ld\n", n);

    for (int i = 0; i < 256 * 256 * 4; i++) tex[i] = (unsigned char)(i * 7 >> 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEnable(GL_TEXTURE_2D); glEnable(GL_DEPTH_TEST);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, xyz); glTexCoordPointer(2, GL_FLOAT, 0, st); glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
    for (int i = 0; i < V; i++) {            /* a grid of small triangles across the window */
        const int t = i / 3, c = i % 3;
        const float x = -1 + (t % 100) * 0.02f, y = -1 + (t / 100) * 0.2f, s = 0.05f;
        xyz[i][0] = x + (c == 1 ? s : 0); xyz[i][1] = y + (c == 2 ? s : 0); xyz[i][2] = 0;
        st[i][0] = c == 1; st[i][1] = c == 2;
        rgba[i][0] = rgba[i][1] = rgba[i][2] = rgba[i][3] = 255;
    }
    unsigned long t0 = TickCount();
    int frames = 0;
    while (TickCount() - t0 < 240) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, V);
        aglSwapBuffers(ctx);
        frames++;
    }
    LOG("%d frames of %d triangles in 4 s: %d fps, %d triangles/s\n", frames, V / 3, frames / 4, frames / 4 * (V / 3));
    long ram = 0; Gestalt(gestaltPhysicalRAMSize, &ram); LOG("RAM %ld MB\n", ram >> 20);
    fclose(out);
    return 0;
}
