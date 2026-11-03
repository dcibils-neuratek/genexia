/*
 * psp_gl.c - draws the PSP's graphics with OpenGL 3.3, for systems without Metal.
 * The same calls as psp_metal.m (psp_metal.h), carried out the same way: each
 * video memory address drawn to becomes a render target several times the size
 * of the original. Targets are drawn upside down, so their rows lie in memory
 * top first as Metal's do and every coordinate from psp_ge.c means what it
 * means there. Its own declarations of OpenGL keep it free of system headers.
 */
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psp_metal.h"

#define LOGICAL 512                  /* video memory's width, the unit vertices arrive in */
#define VERTS (1 << 21)

#if defined(_WIN32) && !defined(_WIN64)
#define GLAPI __stdcall
#else
#define GLAPI
#endif
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;

enum {
    GL_TRIANGLES = 0x0004, GL_NEVER = 0x0200, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_ONE = 1, GL_CONSTANT_COLOR = 0x8001,
    GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
    GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44, GL_DEPTH_CLAMP = 0x864F,
    GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05, GL_TEXTURE_2D = 0x0DE1,
    GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_TEXTURE0 = 0x84C0, GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0, GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_DEPTH_COMPONENT32F = 0x8CAC, GL_MAP_WRITE_BIT = 0x0002, GL_MAP_INVALIDATE_RANGE_BIT = 0x0004, GL_MAP_UNSYNCHRONIZED_BIT = 0x0020, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_MAX_SAMPLES = 0x8D57, GL_RENDERER = 0x1F01,
};

#define GL_CALLS(X) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, ClearDepth, (GLdouble)) X(void, Clear, (GLbitfield)) X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, DepthFunc, (GLenum)) X(void, DepthMask, (GLboolean)) X(void, BlendFunc, (GLenum, GLenum)) X(void, BlendEquation, (GLenum)) \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, PixelStorei, (GLenum, GLint)) X(void, GetIntegerv, (GLenum, GLint *)) \
    X(const unsigned char *, GetString, (GLenum)) X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, BindTexture, (GLenum, GLuint)) X(void, ActiveTexture, (GLenum)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) X(void, GenRenderbuffers, (GLsizei, GLuint *)) X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) X(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) X(void, BindBuffer, (GLenum, GLuint)) X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) X(GLboolean, UnmapBuffer, (GLenum)) X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, BindVertexArray, (GLuint)) X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, UseProgram, (GLuint)) X(GLint, GetUniformLocation, (GLuint, const GLchar *)) X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) X(void, SamplerParameteri, (GLuint, GLenum, GLint)) X(void, BindSampler, (GLuint, GLuint))
#define DECLARE(ret, name, args) static ret (GLAPI *gl##name) args;
GL_CALLS(DECLARE)

static const char *vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec4 pos; layout(location = 1) in vec2 uv_in; layout(location = 2) in vec4 col_in;\n"
    "out vec2 uv; out vec4 col;\n"
    /* upside down, and depth from 0..1 as Metal has it */
    "void main() { gl_Position = vec4(pos.x, -pos.y, 2.0 * pos.z - pos.w, pos.w); uv = uv_in; col = col_in; }\n";
static const char *frag_src =
    "#version 330 core\n"
    "in vec2 uv; in vec4 col; out vec4 outc;\n"
    "uniform sampler2D t; uniform vec4 uf[3]; uniform ivec4 ui[3];\n"     /* uv scale, env, fix; tex func use_a dbl, atest afunc aref amask, fix_src */
    "bool compare(int f, int a, int b) {\n"
    "    if (f == 0) return false; if (f == 1) return true; if (f == 2) return a == b; if (f == 3) return a != b;\n"
    "    if (f == 4) return a < b; if (f == 5) return a <= b; if (f == 6) return a > b; return a >= b; }\n"
    "void main() {\n"
    "    vec4 c = col; int func = ui[0].y; bool use_a = ui[0].z != 0;\n"
    "    if (ui[0].x != 0) {\n"
    "        vec4 x = texture(t, uv * uf[0].xy);\n"
    "        if (func == 0) { c.rgb *= x.rgb; if (use_a) c.a *= x.a; }\n"
    "        else if (func == 1) c.rgb = use_a ? mix(c.rgb, x.rgb, x.a) : x.rgb;\n"
    "        else if (func == 2) { c.rgb = mix(c.rgb, uf[1].rgb, x.rgb); if (use_a) c.a *= x.a; }\n"
    "        else if (func == 3) { c.rgb = x.rgb; if (use_a) c.a = x.a; }\n"
    "        else { c.rgb += x.rgb; if (use_a) c.a *= x.a; }\n"
    "        if (ui[0].w != 0) c.rgb *= 2.0;\n"
    "        c.rgb = min(c.rgb, vec3(1.0));\n"
    "    }\n"
    "    if (ui[1].x != 0 && !compare(ui[1].y, int(c.a * 255.0 + 0.5) & ui[1].w, ui[1].z & ui[1].w)) discard;\n"
    "    if (ui[2].x != 0) c.rgb *= uf[2].rgb;\n"
    "    outc = c;\n"
    "}\n";
static const char *full_src =
    "#version 330 core\n"
    "uniform vec2 edge; out vec2 uv;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); uv = vec2(p.x, 1.0 - p.y) * edge; }\n";
static const char *show_src =
    "#version 330 core\n"
    "in vec2 uv; out vec4 outc; uniform sampler2D t;\n"
    "void main() { outc = vec4(texture(t, uv).rgb, 1.0); }\n";

/* color is the texture a target is read from; with anti-aliasing, drawing goes to
 * the multisampled buffers and is resolved into it before anyone reads it */
typedef struct { uint32_t addr; int drawn, dirty; GLuint color, fbo, ms_fbo, ms_color, depth; } Target;

static void *window;
static GLuint prog, show_prog, vbo, vao, empty_vao, samplers[8], blank, small;
static GLint u_f, u_i, u_edge;
static Target targets[32];
static Target *current;
static int ntargets, used, scale_by, samples = 1;

static Target *find(uint32_t addr) {
    for (int i = 0; i < ntargets; i++) if (targets[i].addr == addr) return &targets[i];
    return NULL;
}
int mtl_is_target(uint32_t addr) { return find(addr) != NULL; }
int mtl_drawn(uint32_t addr) { const Target *t = find(addr); return t && t->drawn; }

static void resolve(Target *t) {
    if (!t->dirty) return;
    t->dirty = 0;
    if (samples == 1) return;
    const int w = 480 * scale_by, h = 272 * scale_by;     /* the part of a target anything reads */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    current = NULL;
}

static GLuint new_texture(int w, int h, const void *px) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static Target *target_for(uint32_t addr) {
    Target *t = find(addr);
    if (t || ntargets == 32) return t ? t : &targets[0];
    t = &targets[ntargets++];
    t->addr = addr;
    const int size = LOGICAL * scale_by;
    t->color = new_texture(size, size, NULL);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    if (samples > 1) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT32F, size, size);
        glGenRenderbuffers(1, &t->ms_color);
        glBindRenderbuffer(GL_RENDERBUFFER, t->ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, size, size);
        glGenFramebuffers(1, &t->ms_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->ms_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->ms_color);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, size, size);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "gl: render target %06X is incomplete\n", addr);
    glDisable(GL_SCISSOR_TEST);                    /* starts black */
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClearColor(0, 0, 0, 1);
    glClearDepth(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (samples > 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    current = NULL;
    return t;
}

static void begin_on(Target *t) {
    if (current == t) return;
    glBindFramebuffer(GL_FRAMEBUFFER, samples > 1 ? t->ms_fbo : t->fbo);
    glViewport(0, 0, LOGICAL * scale_by, LOGICAL * scale_by);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    current = t;
}

static GLenum factor(int f, int src) {
    switch (f) {
    case 0: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default: return src ? GL_ONE : GL_CONSTANT_COLOR;    /* the shader applies a fixed source */
    }
}

/* a new buffer for the vertices: the GPU keeps the old one until it has drawn from it */
static void fresh_vertices(void) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VERTS * sizeof(MtlVert), NULL, GL_STREAM_DRAW);
    used = 0;
}

void mtl_texture(void **slot, int w, int h, int pitch, const uint32_t *rgba) {
    GLuint t = (GLuint)(uintptr_t)*slot;
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);     /* new storage: earlier draws keep the old */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    *slot = (void *)(uintptr_t)t;
}

void mtl_draw(uint32_t addr, const MtlState *s, const MtlVert *v, int count) {
    if (count <= 0 || count > VERTS) return;
    const int x1 = s->sx1 * scale_by, y1 = s->sy1 * scale_by, x2 = (s->sx2 + 1) * scale_by, y2 = (s->sy2 + 1) * scale_by;
    const int top = LOGICAL * scale_by;
    if (x2 <= x1 || y2 <= y1) return;
    Target *t = target_for(addr);
    GLuint tex = blank;
    if (s->tex_target) { Target *r = find(s->tex_target); if (r && r != t) { resolve(r); tex = r->color; } }
    else if (s->tex) tex = (GLuint)(uintptr_t)s->tex;
    begin_on(t);
    t->drawn = t->dirty = 1;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (used + count > VERTS) fresh_vertices();
    /* this frame's part of the buffer that nothing has drawn from yet, so the GPU needn't be waited for */
    void *to = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)used * sizeof *v, (GLsizeiptr)count * sizeof *v,
                                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (!to) return;
    memcpy(to, v, (size_t)count * sizeof *v);
    glUnmapBuffer(GL_ARRAY_BUFFER);

    const GLfloat f[12] = { s->uv_scale[0], s->uv_scale[1], 0, 0, s->env[0], s->env[1], s->env[2], 1, s->fix_a[0], s->fix_a[1], s->fix_a[2], 1 };
    const GLint i[12] = { tex != blank, s->tfunc, s->use_alpha, s->doubled, s->alpha_test, s->alpha_func, s->alpha_ref, s->alpha_mask,
                          s->blend && s->src == 10, 0, 0, 0 };
    glUniform4fv(u_f, 3, f);
    glUniform4iv(u_i, 3, i);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, samplers[s->clamp_u | s->clamp_v << 1 | s->linear << 2]);

    const int mask = s->write_mask;
    glColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
    if (s->blend) {
        static const GLenum ops[8] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_ADD };
        glEnable(GL_BLEND);
        glBlendFunc(factor(s->src & 15, 1), factor(s->dst & 15, 0));
        glBlendEquation(ops[s->eq & 7]);
        glBlendColor(s->fix_b[0], s->fix_b[1], s->fix_b[2], 1);
    } else {
        glDisable(GL_BLEND);
    }
    static const GLenum fn[8] = { GL_NEVER, GL_NEVER + 7, GL_NEVER + 2, GL_NEVER + 5, GL_NEVER + 1, GL_NEVER + 3, GL_NEVER + 4, GL_NEVER + 6 };
    glDepthFunc(fn[s->depth_test ? s->depth_func & 7 : 1]);     /* never always equal notequal less lequal greater gequal */
    glDepthMask(s->depth_write != 0);
    glScissor(x1, y1, (x2 > top ? top : x2) - x1, (y2 > top ? top : y2) - y1);
    glDrawArrays(GL_TRIANGLES, used, count);
    used += count;
}

void mtl_present(uint32_t addr, const uint32_t *cpu_rgba) {
    Target *t = target_for(addr);
    if (cpu_rgba) {                        /* drawn by the game's own code: a picture in memory */
        if (!small) small = new_texture(480, 272, cpu_rgba);
        glBindTexture(GL_TEXTURE_2D, small);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, cpu_rgba);
    } else {
        resolve(t);
    }
    int dw, dh;                            /* the window may have changed size */
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const double k = dw / 480.0 < dh / 272.0 ? dw / 480.0 : dh / 272.0;
    glViewport((GLint)((dw - 480 * k) / 2), (GLint)((dh - 272 * k) / 2), (GLsizei)(480 * k), (GLsizei)(272 * k));
    glUseProgram(show_prog);
    glUniform2f(u_edge, cpu_rgba ? 1 : 480.f / LOGICAL, cpu_rgba ? 1 : 272.f / LOGICAL);
    glBindVertexArray(empty_vao);
    glBindTexture(GL_TEXTURE_2D, cpu_rgba ? small : t->color);
    glBindSampler(0, samplers[3 | 4]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    SDL_GL_SwapWindow(window);
    fresh_vertices();
    current = NULL;
    for (int i = 0; i < ntargets; i++) targets[i].drawn = 0;
}

void mtl_shot(uint32_t addr, const char *path) {
    Target *t = find(addr);
    if (!t) return;
    resolve(t);
    const int w = 480 * scale_by, h = 272 * scale_by;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);    /* row 0 is the top: targets are drawn upside down */
    current = NULL;
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) fwrite(px + i * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}

static GLuint shader(GLenum kind, const char *src) {
    const GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "gl shader: %s\n", log); exit(1); }
    return s;
}

static GLuint program(const char *v, const char *f) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, v));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, f));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "gl program: %s\n", log); exit(1); }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    return p;
}

void mtl_open(void *sdl_window, int scale, int msaa) {
    scale_by = scale;
    window = sdl_window;
    if (!SDL_GL_CreateContext(sdl_window)) { fprintf(stderr, "gl: no OpenGL 3.3 context: %s\n", SDL_GetError()); exit(1); }
    SDL_GL_SetSwapInterval(getenv("PSP_FPS") && atoi(getenv("PSP_FPS")) > 60 ? 0 : 1);     /* faster than the screen: show each frame as it comes */
#define LOAD(ret, name, args) \
    if (!(gl##name = (ret (GLAPI *) args)SDL_GL_GetProcAddress("gl" #name))) { fprintf(stderr, "gl: no gl%s\n", #name); exit(1); }
    GL_CALLS(LOAD)
    GLint most = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &most);
    samples = msaa < 1 ? 1 : msaa > most ? most : msaa;
    prog = program(vert_src, frag_src);
    u_f = glGetUniformLocation(prog, "uf");
    u_i = glGetUniformLocation(prog, "ui");
    show_prog = program(full_src, show_src);
    u_edge = glGetUniformLocation(show_prog, "edge");
    glGenVertexArrays(1, &empty_vao);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VERTS * sizeof(MtlVert), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(MtlVert), (void *)offsetof(MtlVert, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(MtlVert), (void *)offsetof(MtlVert, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(MtlVert), (void *)offsetof(MtlVert, col));
    glGenSamplers(8, samplers);
    for (int i = 0; i < 8; i++) {
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, i & 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, i & 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, i & 4 ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, i & 4 ? GL_LINEAR : GL_NEAREST);
    }
    const uint32_t zero = 0;
    blank = new_texture(1, 1, &zero);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_CLAMP);
    fprintf(stderr, "gl: %s, drawing at %dx%d, %dx anti-aliasing\n", glGetString(GL_RENDERER), 480 * scale, 272 * scale, samples);
}
