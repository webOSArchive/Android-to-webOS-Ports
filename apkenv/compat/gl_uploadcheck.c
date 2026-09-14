/* compat/gl_uploadcheck.c - name the GL uploads the driver rejects.
 *
 * A texture or buffer the driver refuses leaves the engine drawing with an
 * incomplete texture (black) or no geometry at all, and nothing is logged -
 * exactly how Aralon's terrain looks on the TouchPad (black ground where the
 * Mali reference draws grass, with every shader compiling fine). This checks
 * glGetError around each upload and reports the call, target, level, format,
 * type and size of any failure, plus a census of the formats the game uploads.
 *
 * Both wrapper tables call it: the upload entry points are among the ~68 names
 * GLES1 and GLES2 share, and register_hooks_nodup() keeps the GLES1 wrappers
 * for those, so checking only the GLES2 file would silently miss them.
 *
 * Faithful to the engine: an error drained here is kept and handed back on the
 * engine's next glGetError(), so its view of GL error state does not change.
 *
 * Opt-in: APKENV_GL_UPLOADCHECK=1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>

typedef GLenum (*geterr_t)(void);

static int    uc_on = -1;
static GLenum uc_pending;           /* first error drained, owed to the engine */

int
apkenv_gl_uploadcheck_on(void)
{
    if (uc_on < 0) {
        const char *e = getenv("APKENV_GL_UPLOADCHECK");
        uc_on = (e != NULL && e[0] == '1');
        if (uc_on)
            fprintf(stderr, "[GLUP] upload checking on (APKENV_GL_UPLOADCHECK=1)\n");
    }
    return uc_on;
}

/* The engine's own glGetError(): anything we drained comes back first. */
GLenum
apkenv_gl_pending_error(void)
{
    GLenum e = uc_pending;
    uc_pending = 0;
    return e;
}

/* Before an upload: move any stale error out of the way so it is not blamed
 * on this call, but keep it for the engine. */
void
apkenv_gl_upload_pre(geterr_t geterr)
{
    GLenum e;
    if (geterr == NULL)
        return;
    while ((e = geterr()) != GL_NO_ERROR)
        if (uc_pending == 0)
            uc_pending = e;
}

static const char *
fmt_name(GLenum f)
{
    switch (f) {
    case 0:      return "-";
    case 0x1906: return "ALPHA";
    case 0x1907: return "RGB";
    case 0x1908: return "RGBA";
    case 0x1909: return "LUMINANCE";
    case 0x190A: return "LUMINANCE_ALPHA";
    case 0x1401: return "UNSIGNED_BYTE";
    case 0x1403: return "UNSIGNED_SHORT";
    case 0x1406: return "FLOAT";
    case 0x8363: return "USHORT_565";
    case 0x8033: return "USHORT_4444";
    case 0x8034: return "USHORT_5551";
    case 0x8D61: return "HALF_FLOAT_OES";
    case 0x80E1: return "BGRA_EXT";
    case 0x8D64: return "ETC1";
    case 0x8C92: return "ATC_RGB";
    case 0x8C93: return "ATC_RGBA_EXPLICIT";
    case 0x87EE: return "ATC_RGBA_INTERP";
    case 0x83F0: return "DXT1_RGB";
    case 0x83F1: return "DXT1_RGBA";
    case 0x83F2: return "DXT3";
    case 0x83F3: return "DXT5";
    case 0x8C00: return "PVRTC_RGB_4";
    case 0x8C01: return "PVRTC_RGB_2";
    case 0x8C02: return "PVRTC_RGBA_4";
    case 0x8C03: return "PVRTC_RGBA_2";
    case 0x8892: return "ARRAY_BUFFER";
    case 0x8893: return "ELEMENT_ARRAY_BUFFER";
    case 0x0DE1: return "TEXTURE_2D";
    default:     break;
    }
    return NULL;
}

static const char *
err_name(GLenum e)
{
    switch (e) {
    case 0x0500: return "INVALID_ENUM";
    case 0x0501: return "INVALID_VALUE";
    case 0x0502: return "INVALID_OPERATION";
    case 0x0505: return "OUT_OF_MEMORY";
    case 0x0506: return "INVALID_FRAMEBUFFER_OPERATION";
    default:     return "?";
    }
}

#define FMT(buf, f) (fmt_name(f) ? fmt_name(f) : (snprintf(buf, sizeof(buf), "0x%x", (unsigned)(f)), buf))

/* census of what gets uploaded: (call, internalformat, type) -> count, bytes */
#define UC_CENSUS 48
static struct { const char *what; GLenum ifmt, type; unsigned long n, bytes, fails; } uc_c[UC_CENSUS];
static int uc_nc;
static unsigned long uc_uploads;

static void
census_print(void)
{
    int i;
    char a[16], b[16];
    fprintf(stderr, "[GLUP] ---- upload census after %lu uploads ----\n", uc_uploads);
    for (i = 0; i < uc_nc; i++)
        fprintf(stderr, "[GLUP]   %-22s %-18s %-14s x%-6lu %8lu KB  fails=%lu\n",
                uc_c[i].what, FMT(a, uc_c[i].ifmt), FMT(b, uc_c[i].type),
                uc_c[i].n, uc_c[i].bytes / 1024, uc_c[i].fails);
}

/* After an upload: report a failure, and count the upload. */
void
apkenv_gl_upload_post(geterr_t geterr, const char *what, GLenum target, GLint level,
                      GLenum ifmt, GLenum type, GLsizei w, GLsizei h, long bytes)
{
    static unsigned long nfail;
    GLenum e = GL_NO_ERROR, more;
    int i;
    char a[16], b[16], c[16];

    if (geterr != NULL) {
        e = geterr();
        while ((more = geterr()) != GL_NO_ERROR) { /* drain, keep the first */ }
        if (e != GL_NO_ERROR && uc_pending == 0)
            uc_pending = e;             /* the engine is still owed this error */
    }

    for (i = 0; i < uc_nc; i++)
        if (uc_c[i].what == what && uc_c[i].ifmt == ifmt && uc_c[i].type == type)
            break;
    if (i == uc_nc && uc_nc < UC_CENSUS) {
        uc_c[i].what = what; uc_c[i].ifmt = ifmt; uc_c[i].type = type;
        uc_nc++;
    }
    if (i < uc_nc) {
        uc_c[i].n++;
        if (bytes > 0) uc_c[i].bytes += (unsigned long)bytes;
        if (e != GL_NO_ERROR) uc_c[i].fails++;
    }
    uc_uploads++;

    if (e != GL_NO_ERROR) {
        nfail++;
        if (nfail <= 60 || (nfail % 100) == 0)
            fprintf(stderr, "[GLUP] FAILED #%lu %s target=%s level=%d fmt=%s type=%s %dx%d %ld bytes -> %s (0x%x)\n",
                    nfail, what, FMT(a, target), (int)level, FMT(b, ifmt), FMT(c, type),
                    (int)w, (int)h, bytes, err_name(e), (unsigned)e);
    }
    if (uc_uploads == 50 || uc_uploads == 400 || (uc_uploads % 2000) == 0)
        census_print();
}
