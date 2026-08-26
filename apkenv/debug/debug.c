/**
 * apkenv
 * Copyright (c) 2012, Thomas Perl <m@thp.io>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **/


/**
 * segfault debug module
 * see // http://stackoverflow.com/questions/77005/how-to-generate-a-stacktrace-when-my-gcc-c-app-crashes
 *
 */


#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdint.h>

#include "../apkenv.h"


/* This structure mirrors the one found in /usr/include/asm/ucontext.h */
typedef struct _sig_ucontext {
 unsigned long     uc_flags;
 struct ucontext   *uc_link;
 stack_t           uc_stack;
 struct sigcontext uc_mcontext;
 sigset_t          uc_sigmask;
} sig_ucontext_t;

void
apkenv_debug_dump_stack()
{
    intptr_t tmp = 0xcafebabe;
    void **here = (void **)&tmp;

    uint32_t stack_top = (intptr_t)here;
    uint32_t stack_bot = (intptr_t)apkenv_base_of_stack;
    int words = 0;
    while ((void *)here <= apkenv_base_of_stack && words++ < 2048) {
        Dl_info out = {0};
        dladdr(*here, &out);
        uint32_t value = (uint32_t)(intptr_t)*here;
        fprintf(stderr, "on stack 0x%08x: %08x", (uint32_t)(intptr_t)here, value);
        if (value <= stack_bot && value >= stack_top) {
            fprintf(stderr, " -> pointer to stack (offset = %d)", value - (uint32_t)(intptr_t)here);
        }
        if (out.dli_fname != NULL) {
            fprintf(stderr, " (fname=%s +%x)", out.dli_fname, *here - out.dli_fbase);
        }

        if (out.dli_sname != NULL) {
            fprintf(stderr, " (sname=%s +%x)", out.dli_sname, *here - out.dli_saddr);
        }
        if (out.dli_fname == NULL) {   /* apkenv-linker (anonymous) libs */
            Dl_info ao = {0};
            if (apkenv_android_dladdr(*here, &ao) && ao.dli_fname)
                fprintf(stderr, " (lib=%s +%x%s%s)", ao.dli_fname, (unsigned)((char*)*here - (char*)ao.dli_fbase),
                        ao.dli_sname ? " " : "", ao.dli_sname ? ao.dli_sname : "");
        }

        fprintf(stderr, "\n");
        ++here;
    }
}


void crit_err_hdlr(int sig_num, siginfo_t * info, void * ucontext)
{
    void *             array[50];
    void *             caller_address;
    char **            messages;
    int                size, i;
    sig_ucontext_t *   uc;

    uc = (sig_ucontext_t *)ucontext;

    caller_address = (void *) uc->uc_mcontext.arm_pc;
    { extern int apkenv_dladdr_nolock; apkenv_dladdr_nolock = 1; }

    /* First: the cheap, malloc-free facts (signal, faulting pc, backtrace).
     * Only the FIRST crashing thread does the expensive stack scan; a crash
     * in a worker thread would otherwise walk to the main thread's stack base
     * through unrelated memory and never finish. */
    static volatile int in_handler = 0;
    int first = __sync_fetch_and_add(&in_handler, 1) == 0;
    {
        char b[160]; int n;
        n = snprintf(b, sizeof(b), "signal %d addr=%p pc=%p tid-first=%d\n", sig_num, info->si_addr, caller_address, first);
        write(2, b, n);
        {   struct sigcontext *mc = &uc->uc_mcontext;
            n = snprintf(b, sizeof(b), "  regs r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
                         mc->arm_r0, mc->arm_r1, mc->arm_r2, mc->arm_r3, mc->arm_r4, mc->arm_r5, mc->arm_r6, mc->arm_r7);
            write(2, b, n);
            n = snprintf(b, sizeof(b), "  regs r8=%08lx r9=%08lx sl=%08lx fp=%08lx ip=%08lx sp=%08lx lr=%08lx cpsr=%08lx\n",
                         mc->arm_r8, mc->arm_r9, mc->arm_r10, mc->arm_fp, mc->arm_ip, mc->arm_sp, mc->arm_lr, mc->arm_cpsr);
            write(2, b, n);
            Dl_info li; memset(&li, 0, sizeof(li));
            if (apkenv_android_dladdr((void*)mc->arm_lr, &li) && li.dli_fname) {
                n = snprintf(b, sizeof(b), "  lr in %s +0x%x (%s)\n", li.dli_fname, (unsigned)(mc->arm_lr - (unsigned long)li.dli_fbase), li.dli_sname ? li.dli_sname : "?");
                write(2, b, n);
            }
        }
        {   Dl_info ai = {0};
            if (apkenv_android_dladdr(caller_address, &ai) && ai.dli_fname) {
                n = snprintf(b, sizeof(b), "  pc in %s +0x%x (%s)\n", ai.dli_fname,
                             (unsigned)((char*)caller_address - (char*)ai.dli_fbase), ai.dli_sname ? ai.dli_sname : "?");
                write(2, b, n);
            }
            Dl_info hi = {0};
            if (dladdr(caller_address, &hi) && hi.dli_fname) {
                n = snprintf(b, sizeof(b), "  pc in host %s +0x%x (%s)\n", hi.dli_fname,
                             (unsigned)((char*)caller_address - (char*)hi.dli_fbase), hi.dli_sname ? hi.dli_sname : "?");
                write(2, b, n);
            }
        }
        if (first) {   /* raw maps dump (no malloc, before any unwinding) */
            int fd = open("/proc/self/maps", 0);
            if (fd >= 0) { char mb[4096]; int r; write(2, "maps:\n", 6);
                while ((r = read(fd, mb, sizeof(mb))) > 0) write(2, mb, r); close(fd); }
        }
        size = backtrace(array, 50);
        if (size > 1) { array[1] = caller_address; backtrace_symbols_fd(array + 1, size - 1, 2); }
    }
    if (!first) _exit(EXIT_FAILURE);
    apkenv_debug_dump_stack();

    Dl_info out = {0};
    dladdr(info->si_addr, &out);
    Dl_info outa = {0};
    apkenv_android_dladdr(info->si_addr, &outa);

    if (out.dli_fname == NULL) {
        out.dli_fname = outa.dli_fname;
    }
    if (out.dli_sname == NULL) {
        out.dli_sname = outa.dli_sname;
    }

    Dl_info out_caller = {0};
    dladdr(caller_address, &out_caller);
    Dl_info out_callera = {0};
    apkenv_android_dladdr(caller_address, &out_callera);

    if (out_caller.dli_fname == NULL) {
        out_caller.dli_fname = out_callera.dli_fname;
    }
    if (out_caller.dli_sname == NULL) {
        out_caller.dli_sname = out_callera.dli_sname;
    }

    fprintf(stderr, "signal %d (%s), address is %p (%s / %s) from %p (%s / %s)\n",
            sig_num, strsignal(sig_num),

            info->si_addr,
            out.dli_fname ?: "?",
            out.dli_sname ?: "?",

            (void *)caller_address,
            out_caller.dli_fname ?: "?",
            out_caller.dli_sname ?: "?");

    // Output memory map if possible
    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        while (fgets(line, 512, fp) != NULL) {
            fprintf(stderr, "maps: %s", line);
        }
        fclose(fp);
    }

    size = backtrace(array, 50);

    /* overwrite sigaction with caller's address */
    array[1] = caller_address;

    /* malloc-free: the signal may come from inside malloc (abort in
     * munmap_chunk etc.), where backtrace_symbols() would deadlock */
    backtrace_symbols_fd(array + 1, size - 1, 2);
    (void)messages;

    exit(EXIT_FAILURE);
}


void debug_init()
{
    struct sigaction sigact;

    sigact.sa_sigaction = crit_err_hdlr;
    sigact.sa_flags = SA_RESTART | SA_SIGINFO;

    int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
    unsigned i;
    for (i = 0; i < sizeof(sigs)/sizeof(sigs[0]); i++)
        if (sigaction(sigs[i], &sigact, (struct sigaction *)NULL) != 0)
            fprintf(stderr, "error setting signal handler for %d (%s)\n", sigs[i], strsignal(sigs[i]));
}



