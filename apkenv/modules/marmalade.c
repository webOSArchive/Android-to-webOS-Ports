
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

#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <limits.h>

#include "common.h"
#include "../compat/gles_wrappers.h"
#include "../mixer/mixer.h"
#include "../accelerometer/accelerometer.h"

#define ANDROID_ORIENTATION_LANDSCAPE 2
#define ANDROID_ORIENTATION_PORTRAIT 1

#define AUDIO_RATE 22050
#define AUDIO_CHUKSIZE 1024
#define AUDIO_CHANNELS 2

// TODO: to jni/jnienv.h?
typedef struct
{
    jclass clazz;
    jfieldID field;

} dummy_jobject;

// LoaderThread
typedef void (*marmalade_initNative_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_shutdownNative_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_suspendAppThreads_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_resumeAppThreads_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_onAccelNative_t)(JNIEnv *env, jobject p0, jfloat f1, jfloat f2, jfloat f3) SOFTFP;
typedef void (*marmalade_onCompassNative_t)(JNIEnv *env, jobject p0, jint i1, jfloat f2, jfloat f3, jfloat f4) SOFTFP;
typedef void (*marmalade_onMotionEvent_t)(JNIEnv *env, jobject p0, jint i1, jint i2, jint i3, jint i4) SOFTFP;
typedef void (*marmalade_setViewNative_t)(JNIEnv *env, jobject p0, jobject loaderview_o) SOFTFP;
typedef void (*marmalade_runNative_t)(JNIEnv* env, jobject p0, jstring s1, jstring s2) SOFTFP;
typedef void (*marmalade_audioStoppedNotify_t)(JNIEnv *env, jobject p0, jint i1) SOFTFP;
typedef void (*marmalade_chargerStateChanged_t)(JNIEnv *env, jobject p0, jboolean b1) SOFTFP;
typedef void (*marmalade_networkCheckChanged_t)(JNIEnv *env, jobject p0, jboolean b1) SOFTFP;
typedef void (*marmalade_runOnOSThreadNative_t)(JNIEnv *env, jobject p0, jobject runnable_o) SOFTFP;
typedef void (*marmalade_runOnOSTickNative_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_signalSuspend_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_signalResume_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_setSuspended_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_setResumed_t)(JNIEnv *env, jobject p0) SOFTFP;

// LoaderView
typedef void (*marmalade_setPixelsNative_t)(JNIEnv *env, jobject p0, jint i1, jint i2, jarray a3 /* of jint */, jboolean b5) SOFTFP;
typedef void (*marmalade_videoStoppedNotify_t)(JNIEnv *env, jobject p0) SOFTFP;
typedef void (*marmalade_setInputText_t)(JNIEnv *env, jobject p0, jstring s1) SOFTFP;

// LoaderKeyboard
typedef jboolean (*marmalade_onKeyEventNative_t)(JNIEnv *env, jobject p0, jint keycode, jint unicode, jint is_down) SOFTFP;
typedef void (*marmalade_setCharInputEnabledNative_t)(JNIEnv *env, jobject p0, jboolean b1) SOFTFP;

// SoundPlayer
typedef void (*marmalade_generateAudio_t)(JNIEnv *env, jobject p0, jshortArray a1, jint length) SOFTFP;

// SoundRecord
typedef void (*marmalade_recordAudio_t)(JNIEnv *env, jobject p0, jarray a1 /* of jstring */, jint i3, jint i4) SOFTFP;

// LoaderSMSReceiver
typedef void (*marmalade_onReceiveCallback_t)(JNIEnv *env, jobject p0, jstring s1, jstring s2) SOFTFP;

// LoaderAPI
typedef void (*marmalade_s3eDebugTraceLine_t)(JNIEnv *env, jobject p0, jstring s1) SOFTFP;
typedef void (*marmalade_s3eDeviceYield_t)(JNIEnv *env, jobject p0, jint i1) SOFTFP;
typedef jint (*marmalade_s3eConfigGet_t)(JNIEnv *env, jobject p0, jstring s1, jint i2) SOFTFP;
typedef jint (*marmalade_s3eConfigGetInt_t)(JNIEnv *env, jobject p0, jstring s1, jstring s2, jarray a3 /* of jint*/) SOFTFP;

// LoaderLocation
typedef void (*marmalade_locationUpdate_t)(JNIEnv *env, jobject p0, jint i1, jlong j2, jdouble d3, jdouble d4, jdouble d5, jfloat f6, jfloat f7, jfloat f8) SOFTFP;
typedef void (*marmalade_locationSatellite_t)(JNIEnv *env, jobject p0, jint i1, jfloat f3, jfloat f4, jint i5, jfloat f6, jboolean b8) SOFTFP;

struct marmalade_loaderthread_t
{
    marmalade_shutdownNative_t shutdownNative;
    marmalade_initNative_t initNative;
    marmalade_suspendAppThreads_t suspendAppThreads;
    marmalade_resumeAppThreads_t resumeAppThreads;
    marmalade_onAccelNative_t onAccelNative;
    marmalade_onCompassNative_t onCompassNative;
    marmalade_onMotionEvent_t onMotionEvent;
    marmalade_setViewNative_t setViewNative;
    marmalade_runNative_t runNative;
    marmalade_audioStoppedNotify_t audioStoppedNotify;
    marmalade_chargerStateChanged_t chargerStateChanged;
    marmalade_networkCheckChanged_t networkCheckChanged;
    marmalade_runOnOSThreadNative_t runOnOSThreadNative;
    marmalade_runOnOSTickNative_t runOnOSTickNative;
    marmalade_signalSuspend_t signalSuspend;
    marmalade_signalResume_t signalResume;
    marmalade_setSuspended_t setSuspended;
    marmalade_setResumed_t setResumed;
};

struct marmalade_loaderview_t
{
    marmalade_setPixelsNative_t setPixelsNative;
    marmalade_videoStoppedNotify_t videoStoppedNotify;
    marmalade_setInputText_t setInputText;
};

struct marmalade_loaderkeyboard_t
{
    marmalade_onKeyEventNative_t onKeyEventNative;
    marmalade_setCharInputEnabledNative_t setCharInputEnabledNative;
};

struct marmalade_soundplayer_t
{
    marmalade_generateAudio_t generateAudio;
};

struct marmalade_soundrecord_t
{
    marmalade_recordAudio_t recordAudio;
};

struct marmalade_loadersmsreceiver_t
{
    marmalade_onReceiveCallback_t onReceiveCallback;
};

struct marmalade_loaderapi_t
{
    marmalade_s3eDebugTraceLine_t s3eDebugTraceLine;
    marmalade_s3eDeviceYield_t s3eDeviceYield;
    marmalade_s3eConfigGet_t s3eConfigGet;
    marmalade_s3eConfigGetInt_t s3eConfigGetInt;
};

struct marmalade_loaderlocation_t
{
    marmalade_locationUpdate_t locationUpdate;
    marmalade_locationSatellite_t locationSatellite;
};

struct SupportModulePriv {
    jni_onload_t JNI_OnLoad;

    struct marmalade_loaderthread_t loaderthread;
    struct marmalade_loaderview_t loaderview;
    struct marmalade_loaderkeyboard_t loaderkeyboard;
    struct marmalade_soundplayer_t soundplayer;
    struct marmalade_soundrecord_t soundrecord;
    struct marmalade_loadersmsreceiver_t loadersmsreceiver;
    struct marmalade_loaderapi_t loaderapi;
    struct marmalade_loaderlocation_t loaderlocation;

    struct GlobalState *global;
    struct SupportModule *module;

    char *home;

    /* LoaderThread is singleton */
    dummy_jobject *theloaderthread;

    /* LoaderView and LoaderView::m_Pixels */
    /* hopefully we only need one view so this should be ok */
    dummy_jobject *theview;
    jintArray *pixels;

    int accel_started;

    int width, height;

    int marmalade_found;
};

static struct SupportModulePriv marmalade_priv;

/* Touch is polled on the engine/render thread (SDL must be), but Android delivers
 * onMotionEvent on the UI thread. The engine's touch handling expects UI-thread
 * delivery, so we queue taps here and replay them via onMotionEvent FROM the OS/UI
 * thread. (action is already mapped to the engine's TOUCH_* code by the producer.) */
#define MARM_TOUCHQ 128
static struct { int finger, action, x, y; } marm_touchq[MARM_TOUCHQ];
static volatile int marm_touchq_head = 0, marm_touchq_tail = 0;
static pthread_mutex_t marm_touchq_lock = PTHREAD_MUTEX_INITIALIZER;

/* Frames presented (glSwapBuffers). The OS/UI thread watches this to detect when
 * the engine stalls (app threads got suspended) and resumes them at the RIGHT
 * time, instead of a racy fixed-delay resume. */
static volatile unsigned long marm_swap_count = 0;
static int marm_lb_on = 0, marm_lb_ox = 0, marm_lb_oy = 0;   /* letterbox offset */

/* --- runOnOSTickNative servicing thread ------------------------------------
 * The engine (runNative) queues OS-thread work — incl. GL ops for an async
 * level/menu load — and BLOCKS a worker on a pthread cond until that work runs
 * and signals back. We service it via runOnOSTickNative. WHERE it runs matters:
 * GL ops only succeed on the thread that owns the SDL/GL context = the MAIN
 * thread (which is where runNative + glSwapBuffers already run). Our separate
 * os_thread has NO GL context, so ticking only there leaves the queued GL work
 * unable to complete -> the worker's cond is never signalled -> the engine
 * spins on s3eDeviceYield forever (the main-menu load deadlock).
 *
 * Fix: also pump the tick from the main thread's deviceYield handler (it owns
 * the context). APKENV_MARM_TICK selects who pumps:
 *   "both" (default) — main thread (deviceYield) AND the os_thread fallback
 *   "main"           — only the main thread (deviceYield)
 *   "os"             — only the os_thread (legacy pre-fix behaviour)
 * A non-blocking flag serialises the two callers and blocks re-entrancy. */
static int marm_tick_mode = -1;        /* 0=os, 1=main, 2=both */
static volatile int marm_ticking = 0;
static int
marm_tick_get_mode(void)
{
    if(marm_tick_mode < 0) {
        const char *e = getenv("APKENV_MARM_TICK");
        if(e && !strcmp(e, "os"))        marm_tick_mode = 0;
        else if(e && !strcmp(e, "main")) marm_tick_mode = 1;
        else                             marm_tick_mode = 2; /* both = default */
    }
    return marm_tick_mode;
}
static void
marm_pump_ostick(void)
{
    if(!marmalade_priv.loaderthread.runOnOSTickNative) return;
    /* skip if another caller is already inside a tick (cross-thread + re-entrant) */
    if(__sync_lock_test_and_set(&marm_ticking, 1)) return;
    marmalade_priv.loaderthread.runOnOSTickNative(ENV(marmalade_priv.global),
                                                  marmalade_priv.theloaderthread);
    __sync_lock_release(&marm_ticking);
}

/* --- deviceYield spin-loop locator (APKENV_MARM_STACKSCAN=1) ----------------
 * The engine wedges by spin-calling s3eDeviceYield on the MAIN thread. To find
 * WHAT it's polling, scan THIS (engine) thread's stack for return addresses in
 * libpvz's code range — that's the live call chain (spin loop + its callers),
 * logged as libpvz file-offsets to disassemble. Fires periodically from the
 * deviceYield handler; the entries with swap FROZEN are the wedge spin. */
static int marm_ss_on = -1;
static void
marm_dump_callchain(void)
{
    if(marm_ss_on < 0) {
        const char *e = getenv("APKENV_MARM_STACKSCAN");
        marm_ss_on = (e && e[0] == '1') ? 1 : 0;
    }
    if(!marm_ss_on || !marmalade_priv.loaderthread.runNative) return;
    uintptr_t base = ((uintptr_t)marmalade_priv.loaderthread.runNative & ~(uintptr_t)1) - 0x23a04;
    uintptr_t lo = base, hi = base + 0x68000;     /* libpvz r-xp span */
    volatile int marker;
    uintptr_t sp = (uintptr_t)&marker;
    /* SAFETY: the engine runs on an s3e FIBRE with a small stack — scanning far
     * past sp overruns it (SIGSEGV). Bound the scan to the END of the current
     * 4KB page (always mapped) so the read can never fault. */
    uintptr_t pageend = (sp | 0xFFF) - 3;
    char buf[512]; int n = 0;
    n += snprintf(buf+n, sizeof(buf)-n, "[STACKSCAN] swap=%lu chain(off):", marm_swap_count);
    uintptr_t a;
    for(a = (sp & ~(uintptr_t)3); a <= pageend && n < (int)sizeof(buf)-12; a += 4) {
        uintptr_t v = *(volatile uintptr_t *)a;
        uintptr_t t = v & ~(uintptr_t)1;          /* strip Thumb bit */
        if((v & 1) && t >= lo && t < hi)          /* Thumb return address into libpvz */
            n += snprintf(buf+n, sizeof(buf)-n, " %lx", (unsigned long)(t - base));
    }
    fprintf(stderr, "%s\n", buf);
}

/* --- LOADER-FIBRE stack inspector (APKENV_MARM_FIBREDUMP=1) -----------------
 * The wedge is a cooperative s3e-fibre stall: the engine endlessly pumps a
 * loader fibre that never completes. The loader fibre runs on its OWN stack;
 * s3eFibreSwitch_asm saves each fibre's SP at fibre_struct[0] (state at +8).
 * The fibre table lives at libpvz+0x74b74: [+0]=current-fibre ptr, fibre array
 * at +12..+84. Here we read the table, find each NON-current fibre's saved SP,
 * and scan ITS stack for libpvz return-addresses → the loader's REAL call chain
 * (its wait point). /proc/self/maps gives readable bounds so a bad ptr can't
 * fault. */
static int marm_fd_on = -1;
#define MARM_MAXRANGE 256
static struct { uintptr_t s, e; } marm_ranges[MARM_MAXRANGE];
static int marm_nranges = 0;
static void
marm_load_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[300];
    marm_nranges = 0;
    if(!f) return;
    while(marm_nranges < MARM_MAXRANGE && fgets(line, sizeof(line), f)) {
        unsigned long s, e; char perms[8];
        if(sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && perms[0] == 'r') {
            marm_ranges[marm_nranges].s = s; marm_ranges[marm_nranges].e = e; marm_nranges++;
        }
    }
    fclose(f);
}
static int
marm_readable(uintptr_t a, uintptr_t len)
{
    int i;
    for(i = 0; i < marm_nranges; i++)
        if(a >= marm_ranges[i].s && a + len <= marm_ranges[i].e) return 1;
    return 0;
}
static void
marm_scan_fibre_stack(uintptr_t sp, uintptr_t lo, uintptr_t hi, const char *tag)
{
    char buf[600]; int n = 0, hits = 0; uintptr_t a;
    n += snprintf(buf+n, sizeof(buf)-n, "[FIBRE] %s sp=%lx off:", tag, (unsigned long)sp);
    for(a = sp; a < sp + 0x3000 && n < (int)sizeof(buf)-12; a += 4) {
        if(!marm_readable(a, 4)) break;
        uintptr_t v = *(volatile uintptr_t *)a, t = v & ~(uintptr_t)1;
        if((v & 1) && t >= lo && t < hi) {
            n += snprintf(buf+n, sizeof(buf)-n, " %lx", (unsigned long)(t - lo));
            hits++;
        }
    }
    if(hits >= 3) fprintf(stderr, "%s\n", buf);   /* only log coherent chains */
}
static void
marm_dump_fibres(void)
{
    if(marm_fd_on < 0) {
        const char *e = getenv("APKENV_MARM_FIBREDUMP");
        marm_fd_on = (e && e[0] == '1') ? 1 : 0;
        if(marm_fd_on) marm_load_maps();
    }
    if(!marm_fd_on || !marmalade_priv.loaderthread.runNative) return;
    marm_load_maps();   /* refresh (stacks may have been mapped late) */
    uintptr_t base = ((uintptr_t)marmalade_priv.loaderthread.runNative & ~(uintptr_t)1) - 0x23a04;
    uintptr_t lo = base, hi = base + 0x68000;
    uintptr_t fbase = base + 0x74b74;
    if(!marm_readable(fbase, 96)) { fprintf(stderr, "[FIBRE] fbase %lx unmapped\n", (unsigned long)fbase); return; }
    uintptr_t cur = *(volatile uintptr_t *)fbase;
    fprintf(stderr, "[FIBRE] swap=%lu fbase=%lx current=%lx\n",
            marm_swap_count, (unsigned long)fbase, (unsigned long)cur);
    { char b[420]; int n = 0, i;
      n += snprintf(b+n, sizeof(b)-n, "[FIBRE] words:");
      for(i = 0; i < 24; i++) n += snprintf(b+n, sizeof(b)-n, " %lx", (unsigned long)*(volatile uintptr_t *)(fbase + i*4));
      fprintf(stderr, "%s\n", b); }
    /* probe each word in the fibre array region as a candidate saved-SP */
    uintptr_t p;
    for(p = fbase + 12; p <= fbase + 84; p += 4) {
        uintptr_t sp = *(volatile uintptr_t *)p;
        if(sp && (sp & 3) == 0 && !(sp >= lo && sp < hi) && marm_readable(sp, 0x40))
            marm_scan_fibre_stack(sp, lo, hi, "cand");
    }
}

/* copy a printable C string at vaddr p into out[] (bounds-checked); empties out
 * if p isn't readable or isn't clean printable ASCII (used to find filenames). */
static void
marm_str_at(uintptr_t p, char *out, int outlen)
{
    int i = 0;
    out[0] = 0;
    if(p < 0x1000 || !marm_readable(p, 1)) return;
    for(; i < outlen - 1; i++) {
        if(!marm_readable(p + i, 1)) { out[0] = 0; return; }
        char c = *(volatile char *)(p + i);
        if(c == 0) break;
        if(c < 32 || c > 126) { out[0] = 0; return; }
        out[i] = c;
    }
    if(i < 4) { out[0] = 0; return; }   /* ignore trivially-short noise */
    out[i] = 0;
}

/* Dump the loader-manager task queue: pending count + current task + any
 * embedded/pointed filename strings — names the task that won't drain. */
static void
marm_dump_tasks(void)
{
    if(!marm_fd_on || !marmalade_priv.loaderthread.runNative) return;
    uintptr_t base = ((uintptr_t)marmalade_priv.loaderthread.runNative & ~(uintptr_t)1) - 0x23a04;
    uintptr_t mgr = base + 0x717c0;     /* loader_mgr (literal @0x1fefc = 0x717c0) */
    if(!marm_readable(mgr, 0x140)) { fprintf(stderr, "[TASK] mgr %lx unmapped\n", (unsigned long)mgr); return; }
    int count = *(volatile int *)(mgr + 0x20);
    fprintf(stderr, "[TASK] swap=%lu mgr=%lx count(+20)=%d\n",
            marm_swap_count, (unsigned long)mgr, count);
    { char b[700]; int n = 0, i;
      n += snprintf(b+n, sizeof(b)-n, "[TASK] words+20..+120:");
      for(i = 0x20; i <= 0x120 && n < (int)sizeof(b)-16; i += 4)
          n += snprintf(b+n, sizeof(b)-n, " %lx", (unsigned long)*(volatile uintptr_t *)(mgr + i));
      fprintf(stderr, "%s\n", b); }
    /* hunt for filename strings: inline in the task slot + via any pointer word */
    { char s[160]; int i;
      marm_str_at(mgr + 0x24, s, sizeof(s));
      if(s[0]) fprintf(stderr, "[TASK] inline@+24 \"%s\"\n", s);
      for(i = 0x20; i <= 0x120; i += 4) {
          uintptr_t v = *(volatile uintptr_t *)(mgr + i);
          marm_str_at(v, s, sizeof(s));
          if(s[0]) fprintf(stderr, "[TASK] +%x -> \"%s\"\n", i, s);
      } }
}

/* --- SkipFreeRamCheck fix (APKENV_MARM_RAMPATCH=1) --------------------------
 * ROOT CAUSE: the menu-load gate (libpvz 0x1f690) does an s3e startup free-RAM
 * check — `if (s3eDeviceGetInt(freeMem) < MemRequired) stay-not-ready` — and on
 * the TouchPad the free-mem query reports the s3e pool free (a small/conservative
 * value), so it never clears and the loader fibre spins forever. Marmalade's
 * `SkipFreeRamCheck` config bypasses it. We have no editable ICF, so apply the
 * equivalent as a 1-byte RUNTIME patch of the loaded libpvz: the gate's branch
 * `0x1f72a: bge.n 0x1f6f0` (free>=required -> proceed) becomes an UNCONDITIONAL
 * `b.n 0x1f6f0` (always proceed). Byte @+0x1f72b: 0xDA -> 0xE7. Offsets are
 * libpvz(PvZ md5 6b32d855)-specific; overridable via env for other builds. */
static void
marm_patch_ramcheck(void)
{
    const char *e  = getenv("APKENV_MARM_RAMPATCH");
    const char *eg = getenv("APKENV_MARM_GATEBYPASS");
    int do_ram  = (e  && e[0]  == '1');
    int do_gate = (eg && eg[0] == '1');
    if(!do_ram && !do_gate) return;
    if(!marmalade_priv.loaderthread.runNative) { fprintf(stderr, "[PATCH] runNative unresolved\n"); return; }
    unsigned long run_off = 0x23a04;
    const char *r = getenv("APKENV_MARM_RUN_OFF"); if(r) run_off = strtoul(r, NULL, 0);
    uintptr_t base = ((uintptr_t)marmalade_priv.loaderthread.runNative & ~(uintptr_t)1) - run_off;

    if(do_ram) {
        unsigned long patch_off = 0x1f72b, patch_val = 0xe7;
        const char *o = getenv("APKENV_MARM_RAMPATCH_OFF"); if(o) patch_off = strtoul(o, NULL, 0);
        const char *v = getenv("APKENV_MARM_RAMPATCH_VAL"); if(v) patch_val = strtoul(v, NULL, 0);
        unsigned char *p = (unsigned char *)(base + patch_off);
        void *page = (void *)((uintptr_t)p & ~(uintptr_t)0xFFF);
        fprintf(stderr, "[RAMPATCH] base=%p patch@%p before=0x%02x\n", (void *)base, (void *)p, *p);
        if(mprotect(page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *p = (unsigned char)patch_val;
            __builtin___clear_cache((char *)page, (char *)page + 0x2000);
            mprotect(page, 0x2000, PROT_READ | PROT_EXEC);
            fprintf(stderr, "[RAMPATCH] after=0x%02x (free-RAM branch forced)\n", *p);
        } else fprintf(stderr, "[RAMPATCH] mprotect FAILED: %s\n", strerror(errno));
    }

    if(do_gate) {
        /* Force the whole loader-state gate fn 0x1f690 to `movs r0,#0; bx lr`
         * (return 0 = "ready") to test whether 0x1f8e8/0x1f690 gates the freeze. */
        unsigned long goff = 0x1f690;
        const char *go = getenv("APKENV_MARM_GATE_OFF"); if(go) goff = strtoul(go, NULL, 0);
        unsigned short *gp = (unsigned short *)(base + goff);
        void *gpage = (void *)((uintptr_t)gp & ~(uintptr_t)0xFFF);
        fprintf(stderr, "[GATEBYPASS] @%p before=0x%04x 0x%04x\n", (void *)gp, gp[0], gp[1]);
        if(mprotect(gpage, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            gp[0] = 0x2000;  /* movs r0, #0 */
            gp[1] = 0x4770;  /* bx lr        */
            __builtin___clear_cache((char *)gpage, (char *)gpage + 0x2000);
            mprotect(gpage, 0x2000, PROT_READ | PROT_EXEC);
            fprintf(stderr, "[GATEBYPASS] after=0x%04x 0x%04x (gate forced -> return 0)\n", gp[0], gp[1]);
        } else fprintf(stderr, "[GATEBYPASS] mprotect FAILED: %s\n", strerror(errno));
    }
}

#define MARMALADE_LOADERTHREAD "com/ideaworks3d/marmalade/LoaderThread"
#define MARMALADE_LOADERVIEW "com/ideaworks3d/marmalade/LoaderView"
#define MARMALADE_LOADERKEYBOARD "com/ideaworks3d/marmalade/LoaderKeyboard"
#define MARMALADE_SOUNDPLAYER "com/ideaworks3d/marmalade/SoundPlayer"
#define MARMALADE_SOUNDRECORD "com/ideaworks3d/marmalade/SoundRecord"
#define MARMALADE_LOADERSMSRECEIVER "com/ideaworks3d/marmalade/LoaderSMSReceiver"
#define MARMALADE_LOADERAPI "com/ideaworks3d/marmalade/LoaderAPI"
#define MARMALADE_LOADERLOCATION "com/ideaworks3d/marmalade/LoaderLocation"

#define AIRPLAY_LOADERTHREAD "com/ideaworks3d/airplay/AirplayThread"
#define AIRPLAY_LOADERVIEW "com/ideaworks3d/airplay/AirplayView"
#define AIRPLAY_SOUNDPLAYER "com/ideaworks3d/airplay/SoundPlayer"
#define AIRPLAY_SOUNDRECORD "com/ideaworks3d/airplay/SoundRecord"
#define AIRPLAY_LOADERSMSRECEIVER "com/ideaworks3d/airplay/AirplaySMSReceiver"
#define AIRPLAY_LOADERAPI "com/ideaworks3d/airplay/AirplayAPI"

jclass marmalade_FindClass(JNIEnv* p0, const char* p1) SOFTFP;
jthrowable marmalade_ExceptionOccurred(JNIEnv* p0) SOFTFP;
jint marmalade_RegisterNatives(JNIEnv* p0, jclass p1, const JNINativeMethod* p2, jint p3) SOFTFP;
jint marmalade_CallIntMethodV(JNIEnv *env, jobject p1, jmethodID p2, va_list p3) SOFTFP;
jobject marmalade_GetObjectField(JNIEnv* p0, jobject p1, jfieldID p2) SOFTFP;
jobject marmalade_NewGlobalRef(JNIEnv* p0, jobject p1) SOFTFP;
void marmalade_CallVoidMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3) SOFTFP;
jfieldID marmalade_GetStaticFieldID(JNIEnv *p0, jclass p1, const char *p2, const char *p3) SOFTFP;
jobject marmalade_CallObjectMethodV(JNIEnv *env, jobject p1, jmethodID p2, va_list p3) SOFTFP;
jint marmalade_GetStaticIntField(JNIEnv* p0, jclass p1, jfieldID p2) SOFTFP;
jboolean marmalade_CallBooleanMethodV(JNIEnv* p0, jobject p1, jmethodID p2, va_list p3) SOFTFP;

jthrowable
marmalade_ExceptionOccurred(JNIEnv* p0)
{
    // ignore
    return NULL;
}

#define method_is(met) (0 == strcmp(method->name,#met))
#define class_is(claxx) (strcmp(clazz->name,claxx) == 0)

/* --- engine->host call-out tracer (always on, cheap) -----------------------
 * Every Java method the engine calls that this module does NOT implement is a
 * potential contract gap (the engine may block on its result — see
 * getInputString). Log each distinct unhandled name ONCE on first sight (with
 * its JNI signature, the frame count and a coarse tally afterwards) so a gap
 * shows up in the device log instead of as a silent freeze. */
#define MARM_TRACE_MAX 96
static struct { const char *name; unsigned long n; } marm_trace[MARM_TRACE_MAX];
static int marm_trace_n = 0;
static void
marm_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for(i = 0; i < marm_trace_n; i++)
        if(strcmp(marm_trace[i].name, method->name) == 0) {
            unsigned long n = ++marm_trace[i].n;
            if(n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[MARM-JNI] %s %s called %lu times (swap=%lu)\n",
                        kind, method->name, n, marm_swap_count);
            return;
        }
    fprintf(stderr, "[MARM-JNI] UNHANDLED %s %s%s (swap=%lu)\n",
            kind, method->name, method->sig ? method->sig : "", marm_swap_count);
    if(marm_trace_n < MARM_TRACE_MAX) {
        marm_trace[marm_trace_n].name = strdup(method->name);
        marm_trace[marm_trace_n].n = 1;
        marm_trace_n++;
    }
}

/* --- s3eOSReadString: AirplayView.getInputString(title, flags) --------------
 * The engine's s3eOSReadStringUTF8 (libpvz 0x25a9c) calls Java getInputString,
 * then SPINS `while (result == NULL) deviceYield(20)` (0x25b0e..0x25b2c) until
 * the UI thread's dialog answers via the native setInputText(String) (0x23a94:
 * strdup into the result slot). On Android that is an AlertDialog+EditText; on
 * webOS nothing answered it, so the engine froze on the first prompt — PvZ HD's
 * "Enter your name" on a fresh profile, right after "tap to continue".
 * The engine clears the result slot BEFORE the getInputString call and polls it
 * AFTER, so answering synchronously from inside the call is race-free.
 * Stage 1: answer with a default (APKENV_MARM_READSTRING, default "Player"); a
 * real on-screen text entry can replace this later without touching the engine
 * contract. Returning "" would make PvZ re-prompt forever, so never answer empty. */
static void
marm_answer_readstring(jmethodID method, va_list args)
{
    char *title = dup_jstring(marmalade_priv.global, va_arg(args, struct jstring*));
    int flags = va_arg(args, int);
    const char *answer = getenv("APKENV_MARM_READSTRING");
    if(!answer || !answer[0]) answer = "Player";
    fprintf(stderr, "[OSREADSTRING] getInputString title=\"%s\" flags=%d (swap=%lu)\n",
            title ? title : "(null)", flags, marm_swap_count);
    if(marmalade_priv.loaderview.setInputText) {
        JNIEnv *env = ENV(marmalade_priv.global);
        jstring js = (*env)->NewStringUTF(env, answer);
        marmalade_priv.loaderview.setInputText(env, marmalade_priv.theview, js);
        fprintf(stderr, "[OSREADSTRING] answered \"%s\" via setInputText\n", answer);
    } else {
        fprintf(stderr, "[OSREADSTRING] setInputText native NOT registered -> engine will spin\n");
    }
    free(title);
}

jint
marmalade_RegisterNatives(JNIEnv* p0, jclass p1, const JNINativeMethod* p2, jint p3)
{
    MODULE_DEBUG_PRINTF("marmalade_RegisterNatives()\n");

    struct dummy_jclass *clazz = (struct dummy_jclass*)p1;
    if(NULL == clazz)
    {
        MODULE_DEBUG_PRINTF("clazz == NULL\n");
        return -1;
    }
    MODULE_DEBUG_PRINTF("\n\tClass: %s\n", clazz->name);

    int is_loaderthread = class_is(MARMALADE_LOADERTHREAD) || class_is(AIRPLAY_LOADERTHREAD);
    int is_loaderview = class_is(MARMALADE_LOADERVIEW) || class_is(AIRPLAY_LOADERVIEW);
    int is_loaderkeyboard = class_is(MARMALADE_LOADERKEYBOARD); /* airplay has no loaderkeyboard */
    int is_soundplayer = class_is(MARMALADE_SOUNDPLAYER) || class_is(AIRPLAY_SOUNDPLAYER);
    int is_soundrecord = class_is(MARMALADE_SOUNDRECORD) || class_is(AIRPLAY_SOUNDRECORD);
    int is_loadersmsreceiver = class_is(MARMALADE_LOADERSMSRECEIVER) || class_is(AIRPLAY_LOADERSMSRECEIVER);
    int is_loaderapi = class_is(MARMALADE_LOADERAPI) || class_is(AIRPLAY_LOADERAPI);
    int is_loaderlocation = class_is(MARMALADE_LOADERLOCATION); /* airplay has no loaderlocation */

    int i=0;
    const JNINativeMethod *method = p2;
    while (i<p3) {
        MODULE_DEBUG_PRINTF("\tName: %-20s Sig: %-10s Addr: %x\n", method->name, method->signature, method->fnPtr);
#define REG_IF_MATCH(theclass, themethod) if(method_is(themethod)) marmalade_priv.theclass.themethod = (marmalade_ ## themethod ## _t)method->fnPtr; else
        if(is_loaderthread)
        {
            REG_IF_MATCH(loaderthread,shutdownNative)
            REG_IF_MATCH(loaderthread,initNative)
            REG_IF_MATCH(loaderthread,suspendAppThreads)
            REG_IF_MATCH(loaderthread,resumeAppThreads)
            REG_IF_MATCH(loaderthread,onAccelNative)
            REG_IF_MATCH(loaderthread,onCompassNative)
            REG_IF_MATCH(loaderthread,onMotionEvent)
            REG_IF_MATCH(loaderthread,setViewNative)
            REG_IF_MATCH(loaderthread,runNative)
            REG_IF_MATCH(loaderthread,audioStoppedNotify)
            REG_IF_MATCH(loaderthread,chargerStateChanged)
            REG_IF_MATCH(loaderthread,networkCheckChanged)
            REG_IF_MATCH(loaderthread,runOnOSThreadNative)
            REG_IF_MATCH(loaderthread,runOnOSTickNative)
            REG_IF_MATCH(loaderthread,signalSuspend)
            REG_IF_MATCH(loaderthread,signalResume)
            REG_IF_MATCH(loaderthread,setSuspended)
            REG_IF_MATCH(loaderthread,setResumed)
            ;
        }
        else if(is_loaderview)
        {
            REG_IF_MATCH(loaderview,setPixelsNative)
            REG_IF_MATCH(loaderview,videoStoppedNotify)
            REG_IF_MATCH(loaderview,setInputText)
            ;
        }
        else if(is_loaderkeyboard)
        {
            REG_IF_MATCH(loaderkeyboard,onKeyEventNative)
            REG_IF_MATCH(loaderkeyboard,setCharInputEnabledNative)
            ;
        }
        else if(is_soundplayer)
        {
            REG_IF_MATCH(soundplayer,generateAudio)
            ;
        }
        else if(is_soundrecord)
        {
            REG_IF_MATCH(soundrecord,recordAudio)
            ;
        }
        else if(is_loadersmsreceiver)
        {
            REG_IF_MATCH(loadersmsreceiver,onReceiveCallback)
            ;
        }
        else if(is_loaderapi)
        {
            REG_IF_MATCH(loaderapi,s3eDebugTraceLine)
            REG_IF_MATCH(loaderapi,s3eDeviceYield)
            REG_IF_MATCH(loaderapi,s3eConfigGet)
            REG_IF_MATCH(loaderapi,s3eConfigGetInt)
            ;
        }
        else if(is_loaderlocation)
        {
            REG_IF_MATCH(loaderlocation,locationUpdate)
            REG_IF_MATCH(loaderlocation,locationSatellite)
            ;
        }

        method++;
        i++;
    }
    MODULE_DEBUG_PRINTF("\n");

    /* DIAG: dump resolved LoaderThread native addresses so we can map the
     * suspend/resume contract in libpvz (subtract the runtime libpvz base from
     * /proc/<pid>/maps to get file offsets). Remove once the handoff is solved. */
    if(is_loaderthread) {
        fprintf(stderr, "[MARM-REG] runNative=%p onMotionEvent=%p suspendAppThreads=%p "
                "resumeAppThreads=%p runOnOSTickNative=%p runOnOSThreadNative=%p "
                "signalSuspend=%p signalResume=%p setSuspended=%p setResumed=%p "
                "initNative=%p setViewNative=%p\n",
                (void*)marmalade_priv.loaderthread.runNative,
                (void*)marmalade_priv.loaderthread.onMotionEvent,
                (void*)marmalade_priv.loaderthread.suspendAppThreads,
                (void*)marmalade_priv.loaderthread.resumeAppThreads,
                (void*)marmalade_priv.loaderthread.runOnOSTickNative,
                (void*)marmalade_priv.loaderthread.runOnOSThreadNative,
                (void*)marmalade_priv.loaderthread.signalSuspend,
                (void*)marmalade_priv.loaderthread.signalResume,
                (void*)marmalade_priv.loaderthread.setSuspended,
                (void*)marmalade_priv.loaderthread.setResumed,
                (void*)marmalade_priv.loaderthread.initNative,
                (void*)marmalade_priv.loaderthread.setViewNative);
    }

    return 0;
}

static int sound_started;
static int sound_volume = 100; // 0-100 (AudioTrack default is full volume)
/* Device mixer is always opened stereo at the "native" rate (Android's
 * getNativeOutputSampleRate = 44100; the TouchPad's SDL_mixer refuses to
 * resample music, so music files must match this). The engine may ask for
 * mono: it then generates mono frames which the pump widens to L/R. */
#define NATIVE_RATE 44100
static int sound_rate = NATIVE_RATE, sound_channels = 2;   /* mixer/device side */
static int engine_channels = 1;                            /* what generateAudio fills */
static unsigned long sound_cb_count = 0;

/* SoundPlayer pump (the Android side is an AudioTrack thread that calls
 * generateAudio(short[] buf, nFrames) every period and writes buf out). Here we
 * run as the mixer's post-mix hook on the SDL audio thread: ask the engine for
 * exactly this callback's worth of frames and ADD it to the music already in
 * `stream`. nFrames = samples per channel (Java writes nFrames*channels shorts). */
static void my_audio_mixer(void *udata, void *stream, int len)
{
    if (!sound_started || sound_volume == 0 || !marmalade_priv.soundplayer.generateAudio)
        return;
    int nshorts = len / 2;
    int frames = nshorts / sound_channels;
    int gen = frames * engine_channels;
    JNIEnv *env = ENV(marmalade_priv.global);
    jshortArray array = (*env)->NewShortArray(env, gen);
    marmalade_priv.soundplayer.generateAudio(env, marmalade_priv.theloaderthread, array, frames);
    jshort *el = (*env)->GetShortArrayElements(env, array, 0);
    short *out = (short *)stream;
    int vol = sound_volume * 128 / 100, i, peak = 0;
    for (i = 0; i < nshorts; i++) {
        /* map output sample i -> engine sample (mono: same value for L and R) */
        int src = (engine_channels == sound_channels) ? i : (i / sound_channels) * engine_channels + (i % sound_channels) % engine_channels;
        int e = el[src];
        if (e < 0) e = -e;
        if (e > peak) peak = e;
        int v = (int)out[i] + (((int)el[src] * vol) >> 7);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i] = (short)v;
    }
    (*env)->ReleaseShortArrayElements(env, array, el, 0);
    (*env)->DeleteLocalRef(env, array);
    sound_cb_count++;
    if (sound_cb_count == 1 || (sound_cb_count % 2000) == 0)
        fprintf(stderr, "[MARM-SOUND] generateAudio cb#%lu frames=%d peak=%d vol=%d\n",
                sound_cb_count, frames, peak, sound_volume);
}

/* legacy body kept out of the build */
static void my_audio_mixer_old(void *udata, void *stream, int len)
{
    static short soundbuf[AUDIO_CHUKSIZE * AUDIO_CHANNELS];
    struct dummy_array arr;
    short *mixbuf = (short *)stream;
    int vol;
    int i;
    JNIEnv *thread_env;
    JNIEnv ref_env;

    if (!sound_started || sound_volume == 0)
        return;

    if (len > sizeof(soundbuf)) {
        fprintf(stderr, "audio setup broken\n");
        exit(1);
    }

    jarray *array;
    (*VM(marmalade_priv.global))->AttachCurrentThread(VM(marmalade_priv.global), &thread_env, NULL);

    /* here we need the original NewGlobalRef */
    if(marmalade_priv.global->use_dvm)
    {
        ref_env = &(marmalade_priv.global->dalvik_copy_env);
    }
    else ref_env = *thread_env;

    array = (*thread_env)->NewShortArray(thread_env, len / 2);

    jobject *ref = ref_env->NewGlobalRef(thread_env, array);
    marmalade_priv.soundplayer.generateAudio(ENV(marmalade_priv.global),
        VM(marmalade_priv.global), ref, len);
    ref_env->DeleteGlobalRef(thread_env, ref);

    jshort *elements = (*thread_env)->GetShortArrayElements(thread_env, array, 0);
    memcpy(stream, elements, len);
    (*thread_env)->ReleaseShortArrayElements(thread_env, array, elements, JNI_ABORT);

    (*thread_env)->DeleteLocalRef(thread_env, array);

    (*VM(marmalade_priv.global))->DetachCurrentThread(VM(marmalade_priv.global));

    // TODO: some NEON would be nice here
    vol = sound_volume * 128 / 100;
    for (i = 0; i < len; i++) {
        int v = (int)mixbuf[i] + ((int)soundbuf[i] * vol >> 7);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        mixbuf[i] = v;
    }
}

/* MediaPlayerManager */
#define CHANNELS 16

struct audio_player_state {
    struct MixerMusic *music;
    int playing;
    int really_playing; // SDL_Mixer can only play 1, marmalade can play many..
} player_state[CHANNELS];

static void audioStop(unsigned int channel)
{
    if (channel >= CHANNELS)
        return;

    if (player_state[channel].really_playing) {
        player_state[channel].really_playing = 0;
        apkenv_mixer_stop_music(player_state[channel].music);
    }
    if (player_state[channel].playing) {
        player_state[channel].playing = 0;

        MODULE_DEBUG_PRINTF("audioStoppedNotify ch%d\n", channel);
        marmalade_priv.loaderthread.audioStoppedNotify(ENV(marmalade_priv.global),
            marmalade_priv.theloaderthread, channel);
    }
    // TODO: might want to schedule another playing music here..
}

static int audioPlay(const char *filename, int repeats, long long file_offset,
    long long file_size, unsigned int channel)
{
    struct audio_player_state *player;
    const void *mem;
    const char *ext;
    int i;

    if (channel >= CHANNELS || filename == NULL)
        return -1;

    audioStop(channel);

    player = &player_state[channel];
    if (player->music != NULL) {
        apkenv_mixer_free_music(player->music);
        player->music = NULL;
    }

    ext = strrchr(filename, '.');
    if (ext != NULL && strcasecmp(ext, ".apk") == 0
        && marmalade_priv.global->apk_in_mem != NULL)
    {
        mem = (const char *)marmalade_priv.global->apk_in_mem + file_offset;
        player->music = apkenv_mixer_load_music_buffer(mem, file_size);
    }
    else {
        if (file_offset)
            fprintf(stderr, "TODO: offset %lld for %s\n", file_offset, filename);
        /* Android resolves the path with cwd "/" (FileInputStream) and falls back
         * to the apk assets; the engine hands us "media/internal/..." without the
         * leading slash. Also accept a pre-transcoded .ogg twin: the device's
         * SDL_mixer has vorbis but no MP3 decoder. */
        char cand[PATH_MAX]; int k;
        for (k = 0; k < 4 && player->music == NULL; k++) {
            const char *base = filename;
            snprintf(cand, sizeof(cand), "%s%s", (k & 1) && filename[0] != '/' ? "/" : "", base);
            if (k & 2) {
                char *dot = strrchr(cand, '.');
                if (!dot || strcasecmp(dot, ".mp3") != 0) continue;
                strcpy(dot, ".ogg");
            }
            player->music = apkenv_mixer_load_music(cand);
            if (player->music)
                fprintf(stderr, "[MARM-AUDIO] loaded '%s'\n", cand);
        }
    }
    if (player->music == NULL) {
        /* Missing/undecodable track: report failure like Android's MediaPlayer
         * (FileNotFoundException -> -1) instead of handing NULL to the mixer. */
        fprintf(stderr, "[MARM-AUDIO] load_music FAILED for '%s'\n", filename);
        return -1;
    }

    apkenv_mixer_play_music(player->music, repeats);   /* caller passes 1 = loop */
    for (i = 0; i < CHANNELS; i++) {
        player_state[i].really_playing = 0;
    }
    player_state[channel].playing = player_state[channel].really_playing = 1;
    return 0;
}

static void music_finished(void)
{
    int i;

    for (i = 0; i < CHANNELS; i++) {
        if (player_state[i].really_playing) {
            audioStop(i);
            break;
        }
    }
}

static int my_soundInit(void)
{
    MODULE_DEBUG_PRINTF("marmalade initializing audio\n");

    // marmalade gives useless values
    int audio_rate = sound_rate;
    int audio_channels = sound_channels;
    int audio_buffers = AUDIO_CHUKSIZE;

    apkenv_mixer_open(audio_rate, AudioFormat_S16SYS, audio_channels, audio_buffers);

    // TODO Mix_SetPostMix(my_audio_mixer, NULL);
    // TODO Mix_HookMusicFinished(music_finished);

    MODULE_DEBUG_PRINTF("marmalade initializing audio done.\n");
    return audio_rate;
}

jint
marmalade_CallIntMethodV(JNIEnv *env, jobject p1, jmethodID p2, va_list p3)
{
    jmethodID method = p2;

    if(NULL == p2)
    {
        return -1;
    }

    MODULE_DEBUG_PRINTF("marmalade_CallIntMethodV(%s)\n",p2->name);

    if(method_is(getOrientation))
    {
        if(marmalade_priv.global->module_hacks->current_orientation == ORIENTATION_PORTRAIT) {
            return ANDROID_ORIENTATION_PORTRAIT;
        }
        else {
            return ANDROID_ORIENTATION_LANDSCAPE;
        }
    }
    else if(method_is(getNetworkType))
    {
        return 8; // apparently HSDPA
    }
    else if(method_is(audioPlay))
    {
        /* Two loader generations share the name but not the argument list:
         *   Marmalade LoaderThread: (Ljava/lang/String;IJJI)I  file,repeats,offset,size,channel
         *   Airplay   AirplayThread: (Ljava/lang/String;I)I    file,repeats   (single MediaPlayer)
         * Reading the 5-arg form off a 2-arg va_list yields garbage offset/size/
         * channel, so dispatch on the JNI signature. */
        char *filename_data = dup_jstring(marmalade_priv.global, va_arg(p3, struct jstring*));
        int repeats = va_arg(p3, int);
        long long file_offset = 0, file_size = 0;
        int channel = 0;
        if(method->sig && strncmp(method->sig, "(Ljava/lang/String;IJJI)", 24) == 0) {
            file_offset = va_arg(p3, long long);
            file_size = va_arg(p3, long long);
            channel = va_arg(p3, int);
        }
        fprintf(stderr, "[MARM-AUDIO] audioPlay '%s' repeats=%d off=%lld size=%lld ch=%d sig=%s\n",
            filename_data, repeats, file_offset, file_size, channel, method->sig ? method->sig : "?");
        /* Airplay: repeats==0 -> MediaPlayer.setLooping(true); Marmalade: <0 = loop */
        int airplay = (method->sig && strncmp(method->sig, "(Ljava/lang/String;I)", 21) == 0);
        int loop = airplay ? (repeats == 0) : (repeats < 0);
        int rc = audioPlay(filename_data, loop, file_offset, file_size, channel);
        fprintf(stderr, "[MARM-AUDIO] audioPlay -> %d\n", rc);
        free(filename_data);
        return rc;
    }
    else if(method_is(soundInit))
    {
        /* Airplay SoundPlayer.init(boolean stereo, int rate) -> rate actually used
         * (rate 0 = "native"); Marmalade soundInit(int rate, boolean stereo, int vol). */
        int stereo = 1, rate = 0;
        if(method->sig && strncmp(method->sig, "(ZI)", 4) == 0) {
            stereo = va_arg(p3, int); rate = va_arg(p3, int);
        } else {
            rate = va_arg(p3, int); stereo = va_arg(p3, int);
        }
        sound_rate = rate > 0 ? rate : NATIVE_RATE;
        sound_channels = 2;
        engine_channels = stereo ? 2 : 1;
        fprintf(stderr, "[MARM-SOUND] soundInit stereo=%d rate=%d -> mixer %d Hz x%d, engine fills x%d (sig=%s)\n",
                stereo, rate, sound_rate, sound_channels, engine_channels, method->sig ? method->sig : "?");
        my_soundInit();
        apkenv_mixer_set_postmix(my_audio_mixer, NULL);
        return sound_rate;
    }
    else if(method_is(audioGetStatus))
    {
        /* Airplay: 1=Started 2=Paused 3=Error else 0 */
        return player_state[0].really_playing ? 1 : 0;
    }
    else if(method_is(audioGetPosition))
    {
        return 0;
    }
    else if(method_is(audioGetNumChannels))
    {
        // TODO: actually get number of channels
        return AUDIO_CHANNELS;
    }
    else if(method_is(getBatteryLevel))
    {
        return 100;
    }

    marm_trace_unhandled("int", method);
    return 0;
}

jmethodID
jnienv_make_method(jclass clazz, const char *name, const char *sig)
{
    jmethodID id = malloc(sizeof(struct _jmethodID));
    id->clazz = clazz;
    id->name = strdup(name);
    id->sig = strdup(sig);
    return id;
}

jfieldID
jnienv_make_field(jclass clazz, const char *name, const char *sig)
{
    return (jfieldID)jnienv_make_method(clazz,name,sig);
}

/* TODO: to jni/jnienv.c? */
jclass
marmalade_GetObjectClass(JNIEnv *p0, jobject p1)
{
    MODULE_DEBUG_PRINTF("marmalade_GetObjectClass(%x,%x)\n",p0,p1);
    if(NULL != p1)
    {
        dummy_jobject *obj = p1;
        return obj->clazz;
    }
    return NULL;
}

/* is this correct? */
jobject
marmalade_GetObjectField(JNIEnv* p0, jobject p1, jfieldID p2)
{
    if(NULL != p1)
    {
        //if(0 == strcmp(((jmethodID)p2)->name,"m_GL"))
        //{
        //TODO
        //}
        /*
        MODULE_DEBUG_PRINTF("marmalade_GetObjectField(%s)\n",((jmethodID)p2)->name);
        struct dummy_jclass *cls = malloc(sizeof(struct dummy_jclass));
        cls->name = ((struct dummy_jclass*)((jmethodID)p2)->clazz)->name;

        dummy_jobject* obj = malloc(sizeof(dummy_jobject));
        obj->clazz = cls;
        obj->field = p2;

        return obj;*/
    }
    else
    {
        MODULE_DEBUG_PRINTF("marmalade_GetObjectField(%s) -> NULL\n",((jmethodID)p2)->name);
    }

    return NULL;
}

void
marmalade_CallVoidMethodV(JNIEnv* env, jobject p1, jmethodID p2, va_list p3)
{
    jmethodID method = p2;

    /* DIAG: tally which engine->loader methods are called, dump every ~200k calls
     * so we can see what the engine hammers when it appears stuck. */
    {
        static unsigned long s_total=0; static unsigned long s_dy=0,s_sw=0,s_dd=0,s_gi=0,s_gr=0,s_other=0;
        s_total++;
        if(method_is(deviceYield)) s_dy++;
        else if(method_is(glSwapBuffers)) s_sw++;
        else if(method_is(doDraw)) s_dd++;
        else if(method_is(glInit)) s_gi++;
        else if(method_is(glReInit)) s_gr++;
        else s_other++;
        if((s_total % 200000)==0)
            fprintf(stderr,"[MARMTALLY] total=%lu deviceYield=%lu swap=%lu doDraw=%lu glInit=%lu glReInit=%lu other=%lu\n",
                    s_total,s_dy,s_sw,s_dd,s_gi,s_gr,s_other);
    }

    if(method_is(getInputString))
    {
        marm_answer_readstring(method, p3);
    }
    else if(method_is(glReInit))
    {
        MODULE_DEBUG_PRINTF("TODO: implement glReInit\n");
    }
    else if(method_is(glInit))
    {
        MODULE_DEBUG_PRINTF("TODO: implement glInit\n");
    }
    else if(method_is(glSwapBuffers))
    {
        marm_swap_count++;
        /* input_update returns nonzero on a webOS pause/focus-loss event. The
         * engine's runNative is a blocking loop we can't cleanly unwind
         * (shutdownNative crashes), so don't exit(1) on it — that was killing the
         * app on any card switch / screen blank mid-test. Just keep running. */
        marmalade_priv.global->platform->input_update(marmalade_priv.module);

        if(marmalade_priv.accel_started) {
            float x, y, z;
            apkenv_accelerometer_get(&x,&y,&z);
            marmalade_priv.loaderthread.onAccelNative(ENV(marmalade_priv.global),marmalade_priv.theloaderthread,x,y,z);
        }

        marmalade_priv.global->platform->update();
        /* paint the letterbox bars of the NEXT frame black (the engine only
         * touches its own viewport; unpainted rows show the compositor) */
        if(marm_lb_on) apkenv_gles_clear_screen();
    }
    else if(method_is(videoStop))
    {
        // for now
        MODULE_DEBUG_PRINTF("videoStoppedNotify.\n");
        marmalade_priv.loaderview.videoStoppedNotify(ENV(marmalade_priv.global),marmalade_priv.theloaderthread);
        MODULE_DEBUG_PRINTF("videoStoppedNotify done.\n");
    }
    else if(method_is(deviceYield))
    {
        /* The engine yields (sleeps) while idle (e.g. a static menu waiting for
         * input) WITHOUT calling glSwapBuffers — so input was only pumped on
         * render, and taps during idle were never delivered. Pump SDL here too
         * so input reaches the engine even when it isn't drawing a new frame. */
        marmalade_priv.global->platform->input_update(marmalade_priv.module);
        /* Pump the OS-thread queue HERE, on the GL-context-owning main thread,
         * so queued GL work for an async load completes and unblocks the worker
         * (the menu-load deadlock). Skipped in "os" mode. */
        if(marm_tick_get_mode() != 0)
            marm_pump_ostick();
        /* Periodically log the engine-thread call chain + the loader-fibre stacks. */
        { static unsigned long dy = 0; dy++;
          if((dy % 150000) == 0) marm_dump_callchain();
          if((dy % 300000) == 0) { marm_dump_fibres(); marm_dump_tasks(); } }
    }
    else if(method_is(deviceUnYield))
    {
    }
    else if(method_is(fixOrientation))
    {
        int orientation = va_arg(p3,int);
        if(3 == orientation || ANDROID_ORIENTATION_PORTRAIT == orientation)
        {
            marmalade_priv.global->module_hacks->current_orientation = ORIENTATION_PORTRAIT;
        }
        else
        {
            marmalade_priv.global->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
        }
        marmalade_priv.loaderview.setPixelsNative(ENV(marmalade_priv.global),marmalade_priv.theview,marmalade_priv.width,marmalade_priv.height,marmalade_priv.pixels, 0);
    }
    else if(method_is(soundStart))
    {
        fprintf(stderr, "[MARM-SOUND] soundStart (generateAudio=%p)\n",
                (void*)marmalade_priv.soundplayer.generateAudio);
        sound_started = 1;
    }
    else if(method_is(soundStop))
    {
        MODULE_DEBUG_PRINTF("soundStop\n");
        sound_started = 0;
    }
    else if(method_is(soundSetVolume))
    {
        sound_volume = va_arg(p3, int);
        MODULE_DEBUG_PRINTF("soundSetVolume(%d) obj=%p\n", sound_volume, p1);
    }
    else if(method_is(audioSetVolume))
    {
        float volume = va_arg(p3, int) / 100.f;   /* Airplay (I)V; Marmalade (II)V adds channel */
        if(volume < 0) volume = 0; if(volume > 1) volume = 1;
        fprintf(stderr, "[MARM-AUDIO] audioSetVolume %.2f\n", volume);
        apkenv_mixer_volume_music(player_state[0].music, volume);
    }
    else if(method_is(audioStop))
    {
        int channel = 0;
        if(method->sig && strncmp(method->sig, "(I)", 3) == 0) channel = va_arg(p3, int);
        fprintf(stderr, "[MARM-AUDIO] audioStop(%d)\n", channel);
        audioStop(channel);
    }
    else if(method_is(doDraw))
    {
        // TODO (draw whatever is in LoaderThread::m_Pixels)
        /*
        GLuint doDraw_tex;
        glGenTextures(1,&doDraw_tex);
        glBindTexture(GL_TEXTURE_2D, doDraw_tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, marmalade_priv.w, marmalade_priv.h, 0, GL_RGB, GL_UNSIGNED_INT, marmalade_priv.pixels);
        GLfloat vertices[] = {-1, -1, 0,
                              -1,  1, 0,
                               1,  1, 0,
                               1, -1, 0};
        GLubyte indices[] = {0,1,2,
                             0,2,3};
        glVertexPointer(3, GL_FLOAT, 0, vertices);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
        glDeleteTextures(1,&doDraw_tex);
        */
        marmalade_priv.global->platform->update();
    }
    else if(method_is(accelStart))
    {
        marmalade_priv.accel_started = 1;
        apkenv_accelerometer_init();
    }
    else
    {
        marm_trace_unhandled("void", method);
    }
}

// TODO: shouldn't that be in jni/jnienv.c ?
jfieldID
marmalade_GetStaticFieldID(JNIEnv *p0, jclass p1, const char *p2, const char *p3)
{
    struct dummy_jclass* cls = (struct dummy_jclass*)p1;

    if(!strcmp(p2, "SDK_INT") == 0)
    {
        MODULE_DEBUG_PRINTF("marmalade_GetStaticFieldID %s %s %s\n", cls->name, p2, p3);
    }

    struct _jfieldID* field = malloc(sizeof(struct _jfieldID));

    field->clazz = p1;
    field->name = strdup(p2);
    field->sig = strdup(p3);

    return (jfieldID)field;
}

jobject
marmalade_CallObjectMethodV(JNIEnv *env, jobject p1, jmethodID p2, va_list p3)
{
    jmethodID method = p2;
    MODULE_DEBUG_PRINTF("marmalade_CallObjectMethodV %x %s\n",p1,p2->name);

    if(method_is(getLocale))
    {
        // TODO: actually get the locale
        return (*env)->NewStringUTF(env, "en");
    }
    else if(method_is(getDeviceModel))
    {
        // honesty
        return (*env)->NewStringUTF(env,"Nokia N9");
    }
    else if(method_is(getCardRoot))
    {
        return (*env)->NewStringUTF(env, marmalade_priv.home); // or "/"?
    }
    else if(method_is(getDeviceNumber))
    {
        // TODO: find out what Marmalade expects from this function
        return (*env)->NewStringUTF(env, "1234");
    }
    else
    {
        marm_trace_unhandled("obj", p2);
    }
    return NULL;
}

//#define ICE_CREAM_SANDWICH 14

jint
marmalade_GetStaticIntField(JNIEnv* p0, jclass p1, jfieldID p2)
{
    struct _jfieldID *fld = (struct _jfieldID*)p2;

    if(strcmp(fld->name,"SDK_INT") == 0)
    {
        MODULE_DEBUG_PRINTF("Get SDK_INT\n");
        // TODO: look further into that
        return 0x8; // froyo, return whatever, doesn't seem to make much change
    }
    else
    {
        MODULE_DEBUG_PRINTF("marmalade_GetStaticIntField(%s)\n",fld->name);
    }

    return 0;
}

jboolean
marmalade_CallBooleanMethodV(JNIEnv* p0, jobject p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("marmalade_CallBooleanMethodV: %s\n",p2->name);

    if(strcmp(p2->name,"networkCheckStop") == 0)
    {
        MODULE_DEBUG_PRINTF("marmalade_CallBooleanMethodV: networkCheckStop default 0\n");
        // TODO: actually check network
        return 0;
    }
    else if(strcmp(p2->name,"hasMultitouch") == 0)
    {
        return HAVE_MULTITOUCH;
    }
    else if(strcmp(p2->name,"chargerIsConnected") == 0)
    {
        // TODO: actually check charger
        return 1;
    }
    else if(strcmp(p2->name,"audioIsPlaying") == 0)
    {
        return player_state[0].really_playing ? 1 : 0;
    }
    else if(strcmp(p2->name,"hasKeyboard") == 0)
    {
        return 0;
    }

    marm_trace_unhandled("bool", p2);
    return 0;
}

static jobject
marmalade_CallStaticObjectMethodV(JNIEnv *env, jclass p1, jmethodID p2, va_list p3)
{
    MODULE_DEBUG_PRINTF("marmalade_CallStaticObjectMethodV %s\n", p2->name);

    if(strcmp(p2->name, "JGetSDCardAbsolutePath") == 0) {
        return (*env)->NewStringUTF(env, marmalade_priv.home);
    }

    return NULL;
}

extern void *memmem (__const void *__haystack, size_t __haystacklen,
                     __const void *__needle, size_t __needlelen);

void
check_marmalade(const char *filename, char *buffer, size_t size)
{
    char *marmalade_str = "Marmalade";
    char *airplay_str = "Airplay";
    if(0 != memmem(buffer,size,marmalade_str,strlen(marmalade_str)) || 0 != memmem(buffer,size,airplay_str,strlen(airplay_str)))
    {
        marmalade_priv.marmalade_found = 1;
    }
    else
    {
        marmalade_priv.marmalade_found = 0;
    }
}

static int
marmalade_try_init(struct SupportModule *self)
{
    self->priv->JNI_OnLoad = (jni_onload_t)LOOKUP_M("JNI_OnLoad");

    GLOBAL_M->foreach_file("classes.dex",check_marmalade);

    self->override_env.RegisterNatives = marmalade_RegisterNatives;
    self->override_env.CallObjectMethodV = marmalade_CallObjectMethodV;
    self->override_env.CallVoidMethodV = marmalade_CallVoidMethodV;
    self->override_env.CallIntMethodV = marmalade_CallIntMethodV;
    self->override_env.CallBooleanMethodV = marmalade_CallBooleanMethodV;
    self->override_env.GetStaticIntField = marmalade_GetStaticIntField;
    self->override_env.GetStaticFieldID = marmalade_GetStaticFieldID;
    self->override_env.ExceptionOccurred = marmalade_ExceptionOccurred;
//    self->override_env.NewGlobalRef = marmalade_NewGlobalRef;
    self->override_env.GetObjectField = marmalade_GetObjectField;
    self->override_env.GetObjectClass = marmalade_GetObjectClass;
    self->override_env.CallStaticObjectMethodV = marmalade_CallStaticObjectMethodV;

    return (self->priv->JNI_OnLoad != NULL) && marmalade_priv.marmalade_found;
}

/* --- cooperative-yield experiment (APKENV_MARM_COOPYIELD=1) -----------------
 * RE finding: this engine's app/loader threads are COOPERATIVELY-scheduled real
 * pthreads. They park themselves in `suspendAppThreads` (a cond_wait that loops
 * while a suspend-count != 0) and only run when the engine drives the count to 0
 * and signals — which it does ONLY inside its bracket around JNI call-outs
 * (`dec_and_signal … <call> … inc`). During an async menu/level load the engine
 * spin-waits on `deviceYield` (an UNbracketed path) for the loader thread, which
 * is parked and never gets a turn -> deadlock (the recurring multi-threaded-load
 * wall). resumeAppThreads is a near-no-op (clears a status byte; never signals),
 * so pt7's resume approach could never wake it.
 *
 * This replicates the engine's OWN yield bracket from the os_thread when the
 * render stalls: call dec_and_signal (count--, wake the parked loader), give it a
 * real window, then inc (count++, restore the cooperative invariant). It pokes
 * the engine's internal primitives, so it's gated + the offsets are overridable.
 * libpvz(PvZ md5 6b32d855) file-offsets: runNative 0x23a04, dec_and_signal
 * 0x26538, inc 0x26568. Base derived from the resolved runNative address. */
static int marm_coop_on = -1;
static void (*marm_dec_signal)(void) = NULL;
static void (*marm_inc)(void) = NULL;
static volatile int *marm_count_ptr = NULL;   /* suspend count @ loader-state +0x148 */
static volatile int *marm_flag_ptr  = NULL;   /* suspend-enabled @ +0x13c */
static volatile int *marm_stat_ptr  = NULL;   /* "am suspended"  @ +0x144 */
static int
marm_coop_enabled(void)
{
    if(marm_coop_on < 0) {
        const char *e = getenv("APKENV_MARM_COOPYIELD");
        marm_coop_on = (e && e[0] == '1') ? 1 : 0;
        if(marm_coop_on && marmalade_priv.loaderthread.runNative) {
            unsigned long run_off = 0x23a04, dec_off = 0x26538, inc_off = 0x26568;
            unsigned long state_off = 0x71b28; /* loader-state struct (PvZ libpvz) */
            const char *r = getenv("APKENV_MARM_RUN_OFF");
            const char *d = getenv("APKENV_MARM_DEC_OFF");
            const char *i = getenv("APKENV_MARM_INC_OFF");
            const char *s = getenv("APKENV_MARM_STATE_OFF");
            if(r) run_off = strtoul(r, NULL, 0);
            if(d) dec_off = strtoul(d, NULL, 0);
            if(i) inc_off = strtoul(i, NULL, 0);
            if(s) state_off = strtoul(s, NULL, 0);
            uintptr_t base = ((uintptr_t)marmalade_priv.loaderthread.runNative & ~(uintptr_t)1) - run_off;
            marm_dec_signal = (void(*)(void))((base + dec_off) | 1);
            marm_inc        = (void(*)(void))((base + inc_off) | 1);
            uintptr_t st    = base + state_off;
            marm_flag_ptr   = (volatile int*)(st + 0x13c);
            marm_stat_ptr   = (volatile int*)(st + 0x144);
            marm_count_ptr  = (volatile int*)(st + 0x148);
            fprintf(stderr, "[MARM-COOP] enabled base=%p dec=%p inc=%p state=%p count@%p\n",
                    (void*)base, (void*)marm_dec_signal, (void*)marm_inc,
                    (void*)st, (void*)marm_count_ptr);
        } else if(marm_coop_on) {
            fprintf(stderr, "[MARM-COOP] requested but runNative unresolved\n");
        }
    }
    return marm_coop_on;
}
static void
marm_coop_yield(void)
{
    static unsigned long n = 0;
    if(!marm_dec_signal || !marm_inc) return;
    /* READ-INFORMED: read the actual suspend count and drive it to EXACTLY 0 so
     * the parked loader thread's `while(count!=0) cond_wait` gate releases. (Blind
     * dec/inc couldn't hit 0 when count>1.) Log the live state so we learn it. */
    int cnt = marm_count_ptr ? *marm_count_ptr : -999;
    int flg = marm_flag_ptr  ? *marm_flag_ptr  : -999;
    int sta = marm_stat_ptr  ? *marm_stat_ptr  : -999;
    if((n++ % 200) == 0)
        fprintf(stderr, "[MARM-COOP] yield #%lu swap=%lu  count=%d flag=%d stat=%d\n",
                n, marm_swap_count, cnt, flg, sta);
    int guard = 0;
    while(marm_count_ptr && *marm_count_ptr > 0 && guard++ < 64)
        marm_dec_signal();          /* count-- + signal, repeat until exactly 0 */
    usleep(2000);                   /* window for the loader to make progress */
}

/* Marmalade's OS/UI thread. The engine runs on its own thread (runNative) and
 * hands GL/view/load work to the OS thread via an internal queue, then BLOCKS on
 * a semaphore waiting for that thread to run it and signal back (see the spin
 * disassembled at libpvz off 0x26568). With everything on one thread nothing ever
 * services the queue, so the engine spins on deviceYield forever. This dedicated
 * thread continuously drains runOnOSTickNative so those handoffs complete. */
static void *
marmalade_os_thread(void *arg)
{
    JNIEnv *thread_env;
    (*VM(marmalade_priv.global))->AttachCurrentThread(VM(marmalade_priv.global), &thread_env, NULL);
    unsigned long ticks = 0, last_swap = 0, stall = 0, last_resume = 0;
    for(;;) {
        /* Replay queued taps via onMotionEvent on THIS (UI) thread. */
        pthread_mutex_lock(&marm_touchq_lock);
        while(marm_touchq_tail != marm_touchq_head) {
            int fg = marm_touchq[marm_touchq_tail].finger, ac = marm_touchq[marm_touchq_tail].action;
            int x = marm_touchq[marm_touchq_tail].x, y = marm_touchq[marm_touchq_tail].y;
            marm_touchq_tail = (marm_touchq_tail + 1) % MARM_TOUCHQ;
            pthread_mutex_unlock(&marm_touchq_lock);
            if(marmalade_priv.loaderthread.onMotionEvent)
                marmalade_priv.loaderthread.onMotionEvent(ENV(marmalade_priv.global),
                        marmalade_priv.theloaderthread, fg, ac, x, y);
            pthread_mutex_lock(&marm_touchq_lock);
        }
        pthread_mutex_unlock(&marm_touchq_lock);

        /* os_thread fallback tick (no GL context). Skipped in "main" mode so the
         * tick only runs on the context-owning main thread. */
        if(marm_tick_get_mode() != 1)
            marm_pump_ostick();
        /* On Android the GL-surface lifecycle (AirplayView.surfaceChanged) calls
         * resumeAppThreads once the surface is ready; without a SurfaceView the
         * engine's app threads get left SUSPENDED, so the menu freezes and taps
         * that reach onMotionEvent are never processed. A fixed-delay resume is
         * racy (may fire before the suspend). Instead, watch the frame counter:
         * when rendering STALLS (suspended) — or never starts — resume the app
         * threads, rate-limited so we don't over-resume a single stall. */
        ticks++;
        unsigned long sc = marm_swap_count;
        if(sc != last_swap) { last_swap = sc; stall = 0; }
        else                  stall++;
        int wedged = (sc > 30 && stall > 300) || (sc == 0 && ticks > 3000);
        if(marm_coop_enabled()) {
            /* Active fix: while the engine is wedged on an unbracketed yield, give
             * the parked loader thread cooperative turns until rendering resumes. */
            if(wedged) marm_coop_yield();
        } else if(marmalade_priv.loaderthread.resumeAppThreads &&
                  (ticks - last_resume) > 2500 && wedged) {
            fprintf(stderr, "[MARM-OS] stall (frames=%lu) -> resumeAppThreads\n", sc);
            marmalade_priv.loaderthread.resumeAppThreads(ENV(marmalade_priv.global),
                                                         marmalade_priv.theloaderthread);
            last_resume = ticks; stall = 0;
        }
        usleep(1000);
    }
    return NULL;
}

static void
marmalade_init(struct SupportModule *self, int width, int height, const char *home)
{
    self->priv->global = GLOBAL_M;
    self->priv->global->module_hacks->current_orientation = ORIENTATION_LANDSCAPE;
    self->priv->global->module_hacks->glOrthof_rotation_hack = 1;
    self->priv->module = self;
    self->priv->home = strdup(home);

    self->priv->accel_started = 0;

    /* Letterbox (APKENV_MARM_LOGICAL=WxH, e.g. 1280x800 = the resolution the
     * game's asset set targets): present the engine a surface with that aspect,
     * centered on the real screen, and shift its viewport/scissor + our touches. */
    marm_lb_ox = marm_lb_oy = 0; marm_lb_on = 0;
    {
        const char *l = getenv("APKENV_MARM_LOGICAL"); int lw = 0, lh = 0;
        if(l && sscanf(l, "%dx%d", &lw, &lh) == 2 && lw > 0 && lh > 0) {
            double sc = (double)width / lw;
            if((double)height / lh < sc) sc = (double)height / lh;
            int w = (int)(lw * sc + 0.5) & ~1, h = (int)(lh * sc + 0.5) & ~1;
            marm_lb_ox = (width - w) / 2; marm_lb_oy = (height - h) / 2;
            marm_lb_on = 1;
            self->priv->global->module_hacks->viewport_offset_x = marm_lb_ox;
            self->priv->global->module_hacks->viewport_offset_y = marm_lb_oy;
            fprintf(stderr, "[MARM-LB] logical %dx%d on %dx%d -> surface %dx%d at +%d,+%d\n",
                    lw, lh, width, height, w, h, marm_lb_ox, marm_lb_oy);
            width = w; height = h;
        }
    }
    self->priv->width = width;
    self->priv->height = height;

    MODULE_DEBUG_PRINTF("JNI_OnLoad\n");
    self->priv->JNI_OnLoad(VM_M, NULL);
    MODULE_DEBUG_PRINTF("JNI_OnLoad done.\n");

    /* SkipFreeRamCheck: bypass the s3e startup free-RAM gate (runNative resolved
     * by JNI_OnLoad/RegisterNatives; patch before runNative runs the gate). */
    marm_patch_ramcheck();

    self->priv->theloaderthread = malloc(sizeof(dummy_jobject));

    MODULE_DEBUG_PRINTF("initNative\n");
    self->priv->loaderthread.initNative(ENV_M,self->priv->theloaderthread);
    MODULE_DEBUG_PRINTF("initNative done.\n");

    // TODO: extract files (implement dzip algorithm)

    // initialize LoaderView and LoaderView::m_Pixels

    self->priv->theview = (dummy_jobject*)malloc(sizeof(dummy_jobject));
    self->priv->theview->clazz = malloc(sizeof(struct dummy_jclass));
    ((struct dummy_jclass*)(self->priv->theview->clazz))->name = MARMALADE_LOADERVIEW;
    self->priv->theview->field = jnienv_make_field(self->priv->theview->clazz, "theview", "L" MARMALADE_LOADERVIEW ";");

    self->priv->pixels = (*ENV_M)->NewIntArray(ENV_M, width * height);

    MODULE_DEBUG_PRINTF("setViewNative\n");
    self->priv->loaderthread.setViewNative(ENV_M,self->priv->theloaderthread,self->priv->theview);
    MODULE_DEBUG_PRINTF("setViewNative done.\n");

    MODULE_DEBUG_PRINTF("setPixelsNative\n");
    self->priv->loaderview.setPixelsNative(ENV_M,self->priv->theview,width,height,self->priv->pixels, 1 /* == newly created */ );
    MODULE_DEBUG_PRINTF("setPixelsNative done.\n");

    /* needed ?
    MODULE_DEBUG_PRINTF("resumeAppThreads\n");
    self->priv->loaderthread.resumeAppThreads(ENV_M,self->priv->theloaderthread);
    MODULE_DEBUG_PRINTF("resumeAppThreads done.\n");
    */

    /* Start the OS/UI thread BEFORE handing control to the engine's blocking
     * runNative, so the engine's first OS-thread handoff has someone to service it. */
    {
        pthread_t os_tid;
        if(pthread_create(&os_tid, NULL, marmalade_os_thread, NULL) == 0)
            fprintf(stderr, "[MARM] OS/UI thread started\n");
        else
            fprintf(stderr, "[MARM] OS/UI thread FAILED to start\n");
    }

    MODULE_DEBUG_PRINTF("runNative\n");
    self->priv->loaderthread.runNative(ENV_M,self->priv->theloaderthread,
            GLOBAL_M->env->NewStringUTF(ENV_M,self->priv->home),
            GLOBAL_M->env->NewStringUTF(ENV_M,GLOBAL_M->apk_filename));
    MODULE_DEBUG_PRINTF("runNative done.\n");
}

#define POINTER_DOWN 1
#define POINTER_MOVE 3
#define POINTER_UP 2
#define TOUCH_DOWN 4
#define TOUCH_MOVE 6
#define TOUCH_UP 5

static void
marmalade_input(struct SupportModule *self, int event, int x, int y, int finger)
{
   int action;
   if(ACTION_DOWN == event)      action = TOUCH_DOWN;
   else if(ACTION_UP == event)   action = TOUCH_UP;
   else                          action = TOUCH_MOVE;

   /* screen -> letterboxed surface coordinates (centered, so y offset is the
    * same in SDL's top-left and GL's bottom-left frames) */
   x -= marm_lb_ox; y -= marm_lb_oy;
   /* Polled on the engine/render thread; ENQUEUE so the OS/UI thread delivers it
    * via onMotionEvent (matching Android's UI-thread touch delivery). */
   pthread_mutex_lock(&marm_touchq_lock);
   int next = (marm_touchq_head + 1) % MARM_TOUCHQ;
   if(next != marm_touchq_tail) {
       marm_touchq[marm_touchq_head].finger = finger;
       marm_touchq[marm_touchq_head].action = action;
       marm_touchq[marm_touchq_head].x = x;
       marm_touchq[marm_touchq_head].y = y;
       marm_touchq_head = next;
   }
   pthread_mutex_unlock(&marm_touchq_lock);
}

static void
marmalade_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    self->priv->loaderkeyboard.onKeyEventNative(ENV_M, self->priv->theloaderthread, keycode, unicode, event == ACTION_DOWN);
}



/* this function is never called */
static void
marmalade_update(struct SupportModule *self)
{
}

static void
marmalade_deinit(struct SupportModule *self)
{
    MODULE_DEBUG_PRINTF("marmalade_deinit\n");
    MODULE_DEBUG_PRINTF("shutdownNative\n");
    marmalade_priv.loaderthread.shutdownNative(ENV(marmalade_priv.global),marmalade_priv.theloaderthread);
    MODULE_DEBUG_PRINTF("shutdownNative done.\n");
}

static void
marmalade_pause(struct SupportModule *self)
{
}

static void
marmalade_resume(struct SupportModule *self)
{
}

static int
marmalade_requests_exit(struct SupportModule *self)
{
    return 1;
}

APKENV_MODULE(marmalade, MODULE_PRIORITY_ENGINE)

