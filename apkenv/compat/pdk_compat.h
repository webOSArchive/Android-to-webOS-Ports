/**
 * pdk_compat.h — force-included into every TU of the webOS apkenv build.
 *
 * The two-toolchain build compiles with gcc-13 forced onto the PalmPDK
 * glibc-2.4 headers (-nostdinc). A few symbols that the bionic-shim mapping
 * tables take the address of (compat/libc_mapping.h, compat/linux_mapping.h)
 * are not prototyped by those old headers in the way gcc-13 wants. Declare
 * them here so the mapping tables compile; the PalmPDK 4.3.3 linker resolves
 * them against glibc 2.4 at link time.
 */
#ifndef APKENV_PDK_COMPAT_H
#define APKENV_PDK_COMPAT_H

#include <signal.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BSD signal() shim referenced by compat/linux_mapping.h */
#ifndef bsd_signal
__sighandler_t bsd_signal(int signum, __sighandler_t handler);
#endif

/* _FORTIFY_SOURCE helpers referenced by compat/libc_mapping.h */
int __vsprintf_chk(char *s, int flag, size_t slen, const char *format, va_list ap);
int __sprintf_chk(char *s, int flag, size_t slen, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* APKENV_PDK_COMPAT_H */
