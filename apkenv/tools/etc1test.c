/* etc1test.c - host-side unit test for compat/etc1.c.
 *
 * Build & run:  gcc -O2 -o /tmp/etc1test tools/etc1test.c compat/etc1.c -Icompat && /tmp/etc1test
 *
 * Expected (hand-computed from the OES_compressed_ETC1_RGB8_texture spec):
 *   diff pure red     px(0,0)=(255,2,2)                     base 255 + modifier +2, clamped
 *   diff pure green   px(0,0)=(2,255,2)
 *   indiv red|blue    px(0,0)=(255,2,2)  px(3,0)=(2,2,255)  flip=0 -> vertical subblock split
 *   indiv white       px(0,0)=(255,255,255)
 */
#include <stdio.h>
#include <string.h>
#include "etc1.h"
static void show(const char *name, unsigned char b[8]) {
    unsigned char *o = apkenv_etc1_decode(b, 4, 4);
    printf("%-22s px(0,0)=(%3d,%3d,%3d)  px(3,0)=(%3d,%3d,%3d)  px(0,3)=(%3d,%3d,%3d)\n",
        name, o[0],o[1],o[2], o[9],o[10],o[11], o[3*4*3+0],o[3*4*3+1],o[3*4*3+2]);
}
int main(void){
    unsigned char b[8];
    /* differential, base1 = R31 G0 B0 (pure red), delta 0, table 0, all idx 0 (+2) */
    memset(b,0,8); b[0]=31<<3; b[1]=0; b[2]=0; b[3]=0x02;
    show("diff pure red", b);
    /* differential, base1 = G31 (pure green) */
    memset(b,0,8); b[0]=0; b[1]=31<<3; b[2]=0; b[3]=0x02;
    show("diff pure green", b);
    /* individual mode: base1 = (15,0,0) -> 255,0,0 ; base2 = (0,0,15) -> 0,0,255 */
    memset(b,0,8); b[0]=0xF0; b[1]=0x00; b[2]=0x0F; b[3]=0x00; /* diffbit=0, flip=0 */
    show("indiv red|blue", b);
    /* white-ish: individual base1=(15,15,15) */
    memset(b,0,8); b[0]=0xFF; b[1]=0xFF; b[2]=0xFF; b[3]=0x00;
    show("indiv white", b);
    return 0;
}
