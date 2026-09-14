/* compat/gl_uploadcheck.h - see gl_uploadcheck.c.
 *
 * Plain C types on purpose: the GLES1 wrappers include <GL/gl.h> and the GLES2
 * wrappers <GLES2/gl2.h>, so this header must not pull in either. GLenum is an
 * unsigned int and GLint/GLsizei are ints in both. */
#ifndef APKENV_GL_UPLOADCHECK_H
#define APKENV_GL_UPLOADCHECK_H

typedef unsigned int (*apkenv_geterr_t)(void);

int          apkenv_gl_uploadcheck_on(void);
unsigned int apkenv_gl_pending_error(void);
void         apkenv_gl_upload_pre(apkenv_geterr_t geterr);
void         apkenv_gl_upload_post(apkenv_geterr_t geterr, const char *what,
                                   unsigned int target, int level,
                                   unsigned int ifmt, unsigned int type,
                                   int w, int h, long bytes);

#endif
