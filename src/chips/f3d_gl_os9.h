/* The OpenGL 1.2 that f3d.c uses, as pointers: Mac OS 9 has no import library
 * in Retro68, so f3d_gl_load() finds each one in OpenGLLibrary by name. */
#ifndef F3D_GL_OS9_H
#define F3D_GL_OS9_H
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef unsigned char GLboolean;
typedef void GLvoid;
enum {
    GL_FALSE, GL_TRUE, GL_TRIANGLES = 4, GL_SRC_ALPHA = 0x302, GL_ONE_MINUS_SRC_ALPHA, GL_FRONT = 0x404, GL_BACK, GL_FRONT_AND_BACK = 0x408,
    GL_CULL_FACE = 0xB44, GL_DEPTH_TEST = 0xB71, GL_ALPHA_TEST = 0xBC0, GL_BLEND = 0xBE2, GL_SCISSOR_TEST = 0xC11, GL_UNPACK_ALIGNMENT = 0xCF5,
    GL_TEXTURE_2D = 0xDE1, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_MODELVIEW = 0x1700, GL_PROJECTION, GL_TEXTURE = 0x1702,
    GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_PACK_ALIGNMENT = 0xD05, GL_NEAREST = 0x2600, GL_LINEAR, GL_MODULATE = 0x2100, GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR, GL_TEXTURE_ENV = 0x2300,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_REPEAT = 0x2901, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078, GL_CLAMP_TO_EDGE = 0x812F, GL_REPLACE = 0x1E01,
    GL_TEXTURE0_ARB = 0x84C0, GL_TEXTURE1_ARB, GL_LEQUAL = 0x203, GL_GREATER = 0x204,
    GL_COMBINE_EXT = 0x8570, GL_COMBINE_RGB_EXT, GL_COMBINE_ALPHA_EXT, GL_INTERPOLATE_EXT = 0x8575, GL_CONSTANT_EXT, GL_PRIMARY_COLOR_EXT, GL_PREVIOUS_EXT,
    GL_SOURCE0_RGB_EXT = 0x8580, GL_SOURCE1_RGB_EXT, GL_SOURCE2_RGB_EXT, GL_SOURCE0_ALPHA_EXT = 0x8588, GL_SOURCE1_ALPHA_EXT, GL_SOURCE2_ALPHA_EXT,
    GL_OPERAND0_RGB_EXT = 0x8590, GL_OPERAND1_RGB_EXT, GL_OPERAND2_RGB_EXT, GL_SRC_COLOR = 0x300,
    GL_DEPTH_BUFFER_BIT = 0x100, GL_COLOR_BUFFER_BIT = 0x4000
};
#define GL_FUNCTIONS \
    X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) X(void, glClear, (GLbitfield)) X(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf)) \
    X(void, glDepthMask, (GLboolean)) X(void, glDepthFunc, (GLenum)) X(void, glBlendFunc, (GLenum, GLenum)) X(void, glAlphaFunc, (GLenum, GLclampf)) \
    X(void, glCullFace, (GLenum)) X(void, glPolygonOffset, (GLfloat, GLfloat)) X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) X(void, glMatrixMode, (GLenum)) X(void, glLoadIdentity, (void)) \
    X(void, glEnableClientState, (GLenum)) X(void, glVertexPointer, (GLint, GLenum, GLsizei, const GLvoid *)) \
    X(void, glColorPointer, (GLint, GLenum, GLsizei, const GLvoid *)) X(void, glTexCoordPointer, (GLint, GLenum, GLsizei, const GLvoid *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) X(void, glGenTextures, (GLsizei, GLuint *)) X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) X(void, glTexEnvi, (GLenum, GLenum, GLint)) X(void, glTexEnvfv, (GLenum, GLenum, const GLfloat *)) \
    X(void, glPixelStorei, (GLenum, GLint)) X(void, glActiveTextureARB, (GLenum)) X(void, glFinish, (void)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *))
#define X(r, n, a) extern r (*n) a;
GL_FUNCTIONS
#undef X
#endif
