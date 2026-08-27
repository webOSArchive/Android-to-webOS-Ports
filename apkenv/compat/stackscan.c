/**
 * apkenv - a poor man's backtrace for bionic-loaded engine code.
 *
 * glibc's unwinder cannot walk the apk's libraries (no exidx registered) and
 * -fomit-frame-pointer defeats __builtin_return_address(n>0). So: scan the
 * stack for words that point into an engine library AND are preceded by a
 * BL/BLX, which is what a return address looks like. Same technique as the
 * crash handler. Prints "lib+offset" lines - feed the offsets to objdump.
 */
#include <stdio.h>
#include <string.h>
#include "../apkenv.h"

void
apkenv_stackscan(const char *what)
{
    struct { const char *dli_fname; void *dli_fbase;
             const char *dli_sname; void *dli_saddr; } info;
    unsigned long *sp = (unsigned long *)&sp;
    int i, printed = 0;

    fprintf(stderr, "[STACK] %s\n", what);
    for (i = 0; i < 2048 && printed < 20; i++) {
        unsigned long v = sp[i];
        unsigned long prev4; unsigned short prevh;
        if (v < 0x10000 || (v & 1) == 0 ? 0 : 0) { }
        memset(&info, 0, sizeof(info));
        if (!apkenv_android_dladdr((void *)v, &info) || info.dli_fbase == NULL ||
            info.dli_fname == NULL)
            continue;
        if (strstr(info.dli_fname, "libunity") == NULL &&
            strstr(info.dli_fname, "libmono") == NULL)
            continue;
        prev4 = *(unsigned long *)((v & ~3UL) - 4);
        prevh = *(unsigned short *)(v - 4 + 2);
        {
            int arm_bl   = ((prev4 >> 24) & 0x0F) == 0x0B;
            int arm_blx  = (prev4 >> 25) == 0x7D;
            int thumb_bl = (prevh & 0xD000) == 0xD000 || (prevh & 0xD000) == 0xC000;
            if (!arm_bl && !arm_blx && !thumb_bl) continue;
        }
        fprintf(stderr, "[STACK]   sp[%4d] = %s+0x%lx\n", i,
                strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                v - (unsigned long)info.dli_fbase);
        printed++;
    }
}
