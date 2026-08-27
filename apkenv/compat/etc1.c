/**
 * etc1.c - software ETC1 (GL_ETC1_RGB8_OES, 0x8D64) decoder.
 *
 * WHY: Android games ship ETC1-compressed textures as a matter of course, but
 * the TouchPad's Adreno 220 does not expose GL_OES_compressed_ETC1_RGB8_texture
 * on the GLES1 context SDL gives us - glCompressedTexImage2D returns
 * GL_INVALID_ENUM (0x500) and the upload is silently dropped. The geometry then
 * draws untextured, which looks like flat-coloured silhouettes.
 * (Temple Run 2: every texture, 1024x1024 down to 64x64.)
 *
 * Decoding on the CPU and uploading RGB8 costs 4x the texture memory but is the
 * only option short of transcoding the game's assets.
 *
 * Format: 4x4 pixel blocks, 8 bytes each, big-endian. Spec: OES_compressed_ETC1_RGB8_texture.
 */

#include <stdlib.h>
#include <string.h>

#include "etc1.h"

static const int etc1_modifier[8][4] = {
    {  2,   8,   -2,   -8 },
    {  5,  17,   -5,  -17 },
    {  9,  29,   -9,  -29 },
    { 13,  42,  -13,  -42 },
    { 18,  60,  -18,  -60 },
    { 24,  80,  -24,  -80 },
    { 33, 106,  -33, -106 },
    { 47, 183,  -47, -183 },
};

static unsigned char
clamp255(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

/* 4- and 5-bit channel values are scaled to 8 bits by bit replication. */
static int ext4(int v) { return (v << 4) | v; }
static int ext5(int v) { return (v << 3) | (v >> 2); }

/* 3-bit two's-complement delta. */
static int sign3(int v) { return (v & 4) ? (v - 8) : v; }

static void
etc1_decode_block(const unsigned char *b, unsigned char *out, int out_stride)
{
    int base[2][3];
    int table[2];
    int flip, diff;
    unsigned int idx;
    int x, y;

    diff = b[3] & 2;
    flip = b[3] & 1;

    if (diff) {
        int r = b[0] >> 3, g = b[1] >> 3, bl = b[2] >> 3;
        base[0][0] = ext5(r);
        base[0][1] = ext5(g);
        base[0][2] = ext5(bl);
        /* The deltas are applied in 5-bit space, then extended. */
        base[1][0] = ext5((r + sign3(b[0] & 7)) & 31);
        base[1][1] = ext5((g + sign3(b[1] & 7)) & 31);
        base[1][2] = ext5((bl + sign3(b[2] & 7)) & 31);
    } else {
        base[0][0] = ext4(b[0] >> 4);
        base[0][1] = ext4(b[1] >> 4);
        base[0][2] = ext4(b[2] >> 4);
        base[1][0] = ext4(b[0] & 15);
        base[1][1] = ext4(b[1] & 15);
        base[1][2] = ext4(b[2] & 15);
    }

    table[0] = (b[3] >> 5) & 7;
    table[1] = (b[3] >> 2) & 7;

    idx = ((unsigned int)b[4] << 24) | ((unsigned int)b[5] << 16) |
          ((unsigned int)b[6] << 8)  |  (unsigned int)b[7];

    for (x = 0; x < 4; x++) {
        for (y = 0; y < 4; y++) {
            int i = x * 4 + y;                  /* bit order is column-major */
            int lsb = (idx >> i) & 1;
            int msb = (idx >> (i + 16)) & 1;
            int sel = (msb << 1) | lsb;
            int sub = flip ? (y >= 2) : (x >= 2);
            int m = etc1_modifier[table[sub]][sel];
            unsigned char *p = out + y * out_stride + x * 3;
            p[0] = clamp255(base[sub][0] + m);
            p[1] = clamp255(base[sub][1] + m);
            p[2] = clamp255(base[sub][2] + m);
        }
    }
}

unsigned char *
apkenv_etc1_decode(const void *data, int width, int height)
{
    const unsigned char *src = data;
    unsigned char *rgb;
    int bw, bh, bx, by;

    if (data == NULL || width <= 0 || height <= 0)
        return NULL;

    rgb = malloc((size_t)width * (size_t)height * 3);
    if (rgb == NULL)
        return NULL;

    bw = (width + 3) / 4;
    bh = (height + 3) / 4;

    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            unsigned char block[4 * 4 * 3];
            int px, py;

            etc1_decode_block(src, block, 4 * 3);
            src += 8;

            /* Copy the 4x4 block in, clipping at the edges for NPOT sizes. */
            for (py = 0; py < 4; py++) {
                int ty = by * 4 + py;
                if (ty >= height) break;
                for (px = 0; px < 4; px++) {
                    int tx = bx * 4 + px;
                    if (tx >= width) break;
                    memcpy(rgb + ((size_t)ty * width + tx) * 3,
                           block + (py * 4 + px) * 3, 3);
                }
            }
        }
    }

    return rgb;
}
