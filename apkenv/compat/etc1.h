#ifndef COMPAT_ETC1_H
#define COMPAT_ETC1_H

/**
 * Decode an ETC1 (GL_ETC1_RGB8_OES) image to tightly-packed RGB8.
 * Returns a malloc()ed buffer of width*height*3 bytes, or NULL.
 * The caller frees it.
 */
unsigned char *apkenv_etc1_decode(const void *data, int width, int height);

#endif /* COMPAT_ETC1_H */
