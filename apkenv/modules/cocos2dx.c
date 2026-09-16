/**
 * apkenv — **Cocos2d-x 2.0.x** engine support module.
 *
 * First target: Star Wars: Tiny Death Star 1.4.1 (LucasArts/NimbleBit,
 * com.lucasarts.tinydeathstar_goo, cocos2d-2.0-x-2.0.4 in libgame.so).
 * The Java host is the stock org.cocos2dx.lib package (Cocos2dxActivity,
 * Cocos2dxRenderer, Cocos2dxHelper, Cocos2dxBitmap) plus the game's own
 * activity class; the contract below was read off that smali before any
 * device run — see plan/TINY-DEATH-STAR.md for the table.
 *
 * What the host does, in the order Android does it:
 *
 *  1. System.loadLibrary(fmodex, fmodevent, MSDKCore, DMOIAPManager, game):
 *     JNI_OnLoad of every lib that has one. libgame's is where cocos2d-x's
 *     JniHelper stores the JavaVM — without it every engine→Java call fails.
 *  2. Cocos2dxActivity.onCreate → Cocos2dxHelper.init:
 *     nativeSetApkPath(sourceDir), nativeSetExternalAssetPath(
 *     <sdcard>/Android/data/<pkg>/files/assets/). The engine reads its assets
 *     straight out of the apk (its own minizip), so the apk path IS the asset
 *     manager.
 *  3. GLSurfaceView (ES2): onSizeChanged(w,h) → onSurfaceCreated →
 *     nativeInit(w, h) — here genuinely the view size in pixels (read
 *     Cocos2dxRenderer.setScreenWidthAndHeight's caller, not the name).
 *  4. onDrawFrame → nativeRender() every frame.
 *
 * Touch: Cocos2dxGLSurfaceView forwards MotionEvent.getX/getY(i) UNSCALED —
 * view pixels, not normalized — as nativeTouchesBegin/End(id, x, y) and
 * nativeTouchesMove/Cancel(int[] ids, float[] xs, float[] ys).
 *
 * Text: cocos2d-x has no text rasterizer on Android. CCLabelTTF asks Java's
 * Cocos2dxBitmap.createTextBitmap to draw with android.graphics.Canvas and
 * hand the pixels back through nativeInitBitmapDC(w, h, byte[]). That is the
 * platform's job, so it is ours: FreeType (in the webOS PDK) rasterizes the
 * apk's own TTFs, reproducing Cocos2dxBitmap's layout arithmetic line for
 * line (FontMetricsInt top/bottom from the font bbox, measureText wrapping,
 * alignment nibbles).
 *
 * Portrait: the game is portrait (manifest screenOrientation=1). It renders
 * into a 768x1024 offscreen FBO that compat/fbo_es2.c rotates onto the
 * 1024x768 panel; touches travel the same rotation.
 *
 * Audio: FMOD Ex with an OpenSL-ES-only output (no org.fmod Java class in
 * this apk) — served by compat/opensles.c, which this module opts into.
 */

#include "common.h"

#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <sys/time.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int  apkenv_fbo_es2_rotation(void);   /* compat/fbo_es2.c */
GLuint apkenv_fbo_es2_ensure(void);
void apkenv_opensles_enable(void);      /* compat/opensles.c */
void apkenv_bionic_stdio_enable(void);  /* compat/libc_wrappers.c */

/* ── engine entry points (readelf --dyn-syms libgame.so | grep Java_) ─────── */
typedef void     (*cc_void_t)(JNIEnv *, jclass) SOFTFP;
typedef void     (*cc_str_t)(JNIEnv *, jclass, jstring) SOFTFP;
typedef void     (*cc_init_t)(JNIEnv *, jclass, jint, jint) SOFTFP;
typedef void     (*cc_touch_t)(JNIEnv *, jclass, jint, jfloat, jfloat) SOFTFP;
typedef void     (*cc_touches_t)(JNIEnv *, jclass, jintArray, jfloatArray, jfloatArray) SOFTFP;
typedef jboolean (*cc_keydown_t)(JNIEnv *, jclass, jint) SOFTFP;
typedef void     (*cc_bitmapdc_t)(JNIEnv *, jclass, jint, jint, jbyteArray) SOFTFP;
typedef void     (*cc_sensor_t)(JNIEnv *, jclass, jfloat, jfloat, jfloat, jlong) SOFTFP;

struct SupportModulePriv {
    jni_onload_t  JNI_OnLoad_game;
    jni_onload_t  JNI_OnLoad_iap;
    cc_str_t      nativeSetApkPath;
    cc_str_t      nativeSetExternalAssetPath;
    cc_init_t     nativeInit;
    cc_void_t     nativeRender;
    cc_void_t     nativeOnPause;
    cc_void_t     nativeOnResume;
    cc_touch_t    nativeTouchesBegin;
    cc_touch_t    nativeTouchesEnd;
    cc_touches_t  nativeTouchesMove;
    cc_touches_t  nativeTouchesCancel;
    cc_keydown_t  nativeKeyDown;
    cc_bitmapdc_t nativeInitBitmapDC;
    cc_void_t     soundBoardBackground;
    cc_void_t     soundBoardForeground;

    char home[PATH_MAX];
    int  view_w, view_h;        /* what the engine believes the surface is */
    int  portrait;
    int  want_exit;
};
static struct SupportModulePriv cocos2dx_priv;
static struct GlobalState *global;

#define method_is(m) (0 == strcmp(method->name, #m))

static struct dummy_jclass cocos_renderer_class = { (char *)"org/cocos2dx/lib/Cocos2dxRenderer" };
static struct dummy_jclass cocos_bitmap_class   = { (char *)"org/cocos2dx/lib/Cocos2dxBitmap" };
static struct dummy_jclass cocos_helper_class   = { (char *)"org/cocos2dx/lib/Cocos2dxHelper" };
static struct dummy_jclass tds_class            = { (char *)"com/lucasarts/tinydeathstar/tds" };
static struct dummy_jclass inert_object         = { (char *)"java/lang/Object" };

static unsigned long cc_frames = 0;
/* CCDirector::setAnimationInterval → Cocos2dxRenderer.sAnimationInterval.
 * The Java renderer sleeps out the rest of each interval in onDrawFrame;
 * the game asks for 1/30 s. */
static double cc_anim_interval = 1.0 / 60.0;

/* ── engine->host call-out tracer (PORTING-PLAYBOOK.md §3) ───────────────── */
#define CC_TRACE_MAX 128
static struct { char *name; unsigned long n; } cc_trace[CC_TRACE_MAX];
static int cc_trace_n = 0;

static void
cc_trace_unhandled(const char *kind, jmethodID method)
{
    int i;
    for (i = 0; i < cc_trace_n; i++)
        if (strcmp(cc_trace[i].name, method->name) == 0) {
            unsigned long n = ++cc_trace[i].n;
            if (n == 100 || n == 10000 || n == 1000000)
                fprintf(stderr, "[COCOS-JNI] %s %s called %lu times (frame=%lu)\n",
                        kind, method->name, n, cc_frames);
            return;
        }
    fprintf(stderr, "[COCOS-JNI] UNHANDLED %s %s%s (frame=%lu)\n",
            kind, method->name, method->sig ? method->sig : "", cc_frames);
    if (cc_trace_n < CC_TRACE_MAX) {
        cc_trace[cc_trace_n].name = strdup(method->name);
        cc_trace[cc_trace_n].n = 1;
        cc_trace_n++;
    }
}

/* One line per implemented contract point, bounded. */
static void
cc_log_once(jmethodID method, const char *fmt, ...)
{
    static struct { const char *name; int n; } seen[64];
    static int nseen = 0;
    int i;
    va_list ap;
    for (i = 0; i < nseen; i++)
        if (strcmp(seen[i].name, method->name) == 0) {
            if (++seen[i].n > 3) return;
            break;
        }
    if (i == nseen && nseen < 64) { seen[nseen].name = strdup(method->name); seen[nseen].n = 1; nseen++; }
    fprintf(stderr, "[COCOS-HOST] %s: ", method->name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ══ text: Cocos2dxBitmap.createTextBitmap, re-implemented on FreeType ═════ */

static FT_Library ft_lib;
static int ft_ready = 0;

struct cc_font {
    char *name;
    FT_Face face;
    char *data;          /* FT_New_Memory_Face does not copy */
    struct cc_font *next;
};
static struct cc_font *font_cache = NULL;

static FT_Face
cc_font_load_named(const char *name)
{
    struct cc_font *f;
    char *buf = NULL;
    size_t size = 0;
    char path[PATH_MAX];
    FT_Face face;

    for (f = font_cache; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f->face;

    if (!ft_ready) {
        if (FT_Init_FreeType(&ft_lib) != 0) {
            fprintf(stderr, "[COCOS-TEXT] FT_Init_FreeType failed — no text\n");
            return NULL;
        }
        ft_ready = 1;
    }

    /* Cocos2dxTypefaces.get → Typeface.createFromAsset(assets, name). The
     * engine already stripped a leading "assets/". An absolute path (a font
     * file on disk) is also legal. */
    if (name[0] == '/') {
        FILE *fp = fopen(name, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); size = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
            buf = malloc(size);
            if (buf && fread(buf, 1, size, fp) != size) { free(buf); buf = NULL; }
            fclose(fp);
        }
    } else {
        snprintf(path, sizeof(path), "assets/%s", name);
        if (!global->read_file(path, &buf, &size)) buf = NULL;
    }
    if (buf == NULL) {
        f = calloc(1, sizeof(*f));          /* remember the miss */
        f->name = strdup(name);
        f->next = font_cache;
        font_cache = f;
        return NULL;
    }

    if (FT_New_Memory_Face(ft_lib, (const FT_Byte *)buf, (FT_Long)size, 0, &face) != 0) {
        fprintf(stderr, "[COCOS-TEXT] FreeType cannot open font '%s'\n", name);
        free(buf);
        return NULL;
    }
    f = calloc(1, sizeof(*f));
    f->name = strdup(name);
    f->face = face;
    f->data = buf;
    f->next = font_cache;
    font_cache = f;
    fprintf(stderr, "[COCOS-TEXT] loaded font '%s' (%s, %ld glyphs, upem %d)\n",
            name, face->family_name ? face->family_name : "?",
            face->num_glyphs, face->units_per_EM);
    return face;
}

/* Typeface.create(name) for a non-file name falls back to the system sans
 * (Droid Sans on Android). The TouchPad's system sans is Prelude, 56 KB; the
 * apk's ArialUnicode.ttf is the second choice only, because reading it means
 * holding 23 MB in RAM — for the engine's stats labels, which this game never
 * shows. */
static FT_Face
cc_font_fallback(void)
{
    const char *env = getenv("APKENV_COCOS_DEFAULT_FONT");
    FT_Face face = NULL;
    if (env) face = cc_font_load_named(env);
    if (!face) face = cc_font_load_named("/usr/share/fonts/Prelude-Medium.ttf");
    if (!face) face = cc_font_load_named("ArialUnicode.ttf");
    return face;
}

static FT_Face
cc_font_for(const char *font_name)
{
    FT_Face face = NULL;
    size_t n = strlen(font_name);
    if (n > 4 && strcasecmp(font_name + n - 4, ".ttf") == 0)
        face = cc_font_load_named(font_name);
    if (face == NULL) {
        static char last[128];
        if (strncmp(last, font_name, sizeof(last) - 1) != 0) {
            fprintf(stderr, "[COCOS-TEXT] font '%s' -> system sans fallback\n", font_name);
            snprintf(last, sizeof(last), "%s", font_name);
        }
        face = cc_font_fallback();
    }
    return face;
}

/* UTF-8 → code points (Java strings are UTF-16; everything this game draws
 * is in the BMP, where the two index the same). */
static int
cc_utf8_decode(const char *s, unsigned **out)
{
    size_t len = strlen(s);
    unsigned *cp = malloc((len + 1) * sizeof(unsigned));
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0xc0 && c < 0xe0 && (p[0] & 0xc0) == 0x80) {
            c = ((c & 0x1f) << 6) | (p[0] & 0x3f); p += 1;
        } else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
            c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2;
        } else if (c >= 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            c = ((c & 0x07) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
        }
        cp[n++] = c;
    }
    *out = cp;
    return n;
}

struct cc_paint {
    FT_Face face;
    FT_Face fallback;
    int size;
    int top, bottom;     /* Paint.FontMetricsInt */
    int halign;          /* 1 left, 2 right, 3 center */
};

static FT_Face
cc_paint_face_for(struct cc_paint *p, unsigned c, FT_UInt *glyph)
{
    *glyph = FT_Get_Char_Index(p->face, c);
    if (*glyph != 0 || p->fallback == NULL || p->fallback == p->face)
        return p->face;
    {
        FT_UInt g = FT_Get_Char_Index(p->fallback, c);
        if (g == 0) return p->face;
        FT_Set_Pixel_Sizes(p->fallback, 0, (FT_UInt)p->size);
        *glyph = g;
        return p->fallback;
    }
}

/* Hinted integer advance, as Skia's FreeType host reports it without
 * LINEAR_TEXT_FLAG (which Cocos2dxBitmap never sets). */
static int
cc_advance(struct cc_paint *p, unsigned c)
{
    FT_UInt g;
    FT_Face face = cc_paint_face_for(p, c, &g);
    if (FT_Load_Glyph(face, g, FT_LOAD_DEFAULT) != 0) return 0;
    return (int)((face->glyph->advance.x + 32) >> 6);
}

/* paint.measureText(s, start, end) → FloatMath.ceil → int */
static int
cc_measure(struct cc_paint *p, const unsigned *cp, int start, int end)
{
    int i, w = 0;
    for (i = start; i < end; i++) w += cc_advance(p, cp[i]);
    return w;
}

struct cc_line { int start, end; };   /* indices into a code point buffer */
struct cc_lines { struct cc_line *v; int n, cap; };

static void
cc_lines_add(struct cc_lines *l, int start, int end)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, (size_t)l->cap * sizeof(*l->v));
    }
    l->v[l->n].start = start;
    l->v[l->n].end = end;
    l->n++;
}

/* Cocos2dxBitmap.divideStringWithMaxWidth — reproduced including its quirks
 * (the `indexOf(i) == ' '` test is effectively never true, and the "==" branch
 * drops the character after the break), because the game was laid out
 * against exactly this behaviour. */
static void
cc_divide(struct cc_paint *p, const unsigned *cp, int s, int e, int max_w, struct cc_lines *out)
{
    int len = e - s;
    int start = 0, i;
    for (i = 1; i <= len; i++) {
        int w = cc_measure(p, cp, s + start, s + i);
        if (w >= max_w) {
            int last_space = -1, k;
            for (k = i - 1; k >= 0; k--)
                if (cp[s + k] == ' ') { last_space = k; break; }
            if (last_space != -1 && last_space > start) {
                cc_lines_add(out, s + start, s + last_space);
                i = last_space;
            } else if (w > max_w) {
                cc_lines_add(out, s + start, s + i - 1);
                i = i - 1;
            } else {
                cc_lines_add(out, s + start, s + i);
            }
            start = i + 1;
            i = start;
        }
    }
    if (start < len)
        cc_lines_add(out, s + start, e);
}

/* refactorString + String.split("\\n"): empty interior lines become " ",
 * a trailing empty piece is dropped, and "" becomes " ". */
static unsigned space_cp = ' ';

static void
cc_split(struct cc_paint *p, const unsigned *cp, int n, int max_w, int max_h,
         struct cc_lines *out, unsigned **spacebuf)
{
    struct cc_lines raw = { NULL, 0, 0 };
    int i, start = 0;
    int h = p->bottom - p->top;
    int max_lines = h > 0 ? max_h / h : 0;
    (void)spacebuf;

    for (i = 0; i <= n; i++) {
        if (i == n || cp[i] == '\n') {
            cc_lines_add(&raw, start, i);
            start = i + 1;
        }
    }
    /* Java's split drops trailing empty strings. */
    while (raw.n > 0 && raw.v[raw.n - 1].start == raw.v[raw.n - 1].end) raw.n--;
    if (raw.n == 0) cc_lines_add(&raw, -1, -1);          /* " " */
    for (i = 0; i < raw.n; i++)
        if (raw.v[i].start == raw.v[i].end) raw.v[i].start = raw.v[i].end = -1;  /* " " */

    if (max_w != 0) {
        for (i = 0; i < raw.n; i++) {
            struct cc_line l = raw.v[i];
            if (l.start < 0) {
                cc_lines_add(out, -1, -1);
            } else if (cc_measure(p, cp, l.start, l.end) > max_w) {
                cc_divide(p, cp, l.start, l.end, max_w, out);
            } else {
                cc_lines_add(out, l.start, l.end);
            }
            if (max_lines > 0 && out->n >= max_lines) break;
        }
        if (max_lines > 0 && out->n > max_lines) out->n = max_lines;
    } else if (max_h != 0 && raw.n > max_lines) {
        for (i = 0; i < max_lines; i++) cc_lines_add(out, raw.v[i].start, raw.v[i].end);
    } else {
        for (i = 0; i < raw.n; i++) cc_lines_add(out, raw.v[i].start, raw.v[i].end);
    }
    free(raw.v);
}

static void
cc_draw_line(struct cc_paint *p, const unsigned *cp, int start, int end,
             int x, int baseline, unsigned char *px, int bw, int bh)
{
    int pen, i;
    int width;
    if (start < 0) return;                               /* the " " line */
    width = cc_measure(p, cp, start, end);
    /* Paint.Align: drawText's x is the left edge, centre or right edge. */
    if (p->halign == 3)      pen = x - width / 2;
    else if (p->halign == 2) pen = x - width;
    else                     pen = x;

    for (i = start; i < end; i++) {
        FT_UInt g;
        FT_Face face = cc_paint_face_for(p, cp[i], &g);
        FT_GlyphSlot slot;
        int gx, gy, r, c;
        if (FT_Load_Glyph(face, g, FT_LOAD_DEFAULT) != 0) continue;
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
        slot = face->glyph;
        gx = pen + slot->bitmap_left;
        gy = baseline - slot->bitmap_top;
        for (r = 0; r < (int)slot->bitmap.rows; r++) {
            int yy = gy + r;
            const unsigned char *src;
            if (yy < 0 || yy >= bh) continue;
            src = slot->bitmap.buffer + r * (slot->bitmap.pitch < 0 ? -slot->bitmap.pitch : slot->bitmap.pitch);
            for (c = 0; c < (int)slot->bitmap.width; c++) {
                int xx = gx + c;
                unsigned char a;
                unsigned char *d;
                unsigned v;
                if (xx < 0 || xx >= bw) continue;
                a = (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                    ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0) : src[c];
                if (a == 0) continue;
                d = px + ((size_t)yy * (size_t)bw + (size_t)xx) * 4;
                /* Premultiplied white (Paint color -1): every channel is the
                 * coverage, so the byte order nativeInitBitmapDC swaps into is
                 * immaterial. Overlapping glyph edges combine with src-over. */
                v = a + d[3] * (255u - a) / 255u;
                d[0] = d[1] = d[2] = d[3] = (unsigned char)v;
            }
        }
        pen += (int)((slot->advance.x + 32) >> 6);
    }
}

static unsigned long text_bitmaps = 0;

static void
cc_create_text_bitmap(JNIEnv *env, const char *text, const char *font_name,
                      int font_size, int alignment, int width, int height)
{
    struct cc_paint p;
    struct cc_lines lines = { NULL, 0, 0 };
    unsigned *cp = NULL;
    int n, i, bw, bh, h_per_line, total_h, y, valign;
    unsigned char *px;
    struct dummy_array *arr;
    FT_Face fb;

    memset(&p, 0, sizeof(p));
    p.halign = alignment & 0x0f;
    valign = (alignment >> 4) & 0x0f;
    p.size = font_size > 0 ? font_size : 1;
    p.face = cc_font_for(font_name);
    fb = cc_font_fallback();
    p.fallback = fb;

    if (p.face == NULL) {
        fprintf(stderr, "[COCOS-TEXT] no font at all for '%s' — returning a blank bitmap\n", font_name);
        p.top = -p.size; p.bottom = p.size / 4;
    } else {
        FT_Set_Pixel_Sizes(p.face, 0, (FT_UInt)p.size);
        /* SkFontHost_FreeType: fTop = -bbox.yMax*scale, fBottom = -bbox.yMin*scale;
         * Paint.getFontMetricsInt floors top and ceils bottom. */
        {
            double scale = (double)p.size / (double)p.face->units_per_EM;
            p.top = (int)floor(-(double)p.face->bbox.yMax * scale);
            p.bottom = (int)ceil(-(double)p.face->bbox.yMin * scale);
        }
    }

    n = cc_utf8_decode(text, &cp);
    if (p.face)
        cc_split(&p, cp, n, width, height, &lines, NULL);
    else
        cc_lines_add(&lines, -1, -1);

    h_per_line = (int)ceil((double)(p.bottom - p.top));
    total_h = h_per_line * lines.n;

    /* computeTextProperty: explicit width wins; else the widest line. */
    bw = width;
    if (bw == 0 && p.face)
        for (i = 0; i < lines.n; i++) {
            int w = lines.v[i].start < 0 ? cc_advance(&p, ' ')
                                         : cc_measure(&p, cp, lines.v[i].start, lines.v[i].end);
            if (w > bw) bw = w;
        }
    bh = height ? height : total_h;
    if (bw <= 0) bw = 1;
    if (bh <= 0) bh = 1;

    px = calloc((size_t)bw * (size_t)bh, 4);

    /* computeY: vAlign 1 top, 2 bottom, 3 centre — only when constrained
     * taller than the text. */
    y = -p.top;
    if (height > total_h) {
        if (valign == 3)      y = -p.top + (height - total_h) / 2;
        else if (valign == 2) y = -p.top + (height - total_h);
    }
    if (p.face) {
        for (i = 0; i < lines.n; i++) {
            int x = (p.halign == 3) ? bw / 2 : (p.halign == 2) ? bw : 0;
            cc_draw_line(&p, cp, lines.v[i].start, lines.v[i].end, x, y, px, bw, bh);
            y += h_per_line;
        }
    }

    if (text_bitmaps++ < 60)
        fprintf(stderr, "[COCOS-TEXT] '%.40s' font=%s size=%d align=0x%x box=%dx%d -> %dx%d, %d line(s)\n",
                text, font_name, font_size, alignment, width, height, bw, bh, lines.n);

    arr = malloc(sizeof(*arr));
    arr->data = px;
    arr->element_size = 1;
    arr->length = (long)bw * bh * 4;
    if (cocos2dx_priv.nativeInitBitmapDC)
        cocos2dx_priv.nativeInitBitmapDC(env, (jclass)&cocos_bitmap_class, bw, bh, (jbyteArray)arr);
    else
        fprintf(stderr, "[COCOS-TEXT] nativeInitBitmapDC not found — text will be missing\n");
    /* The engine copied the bytes out with GetByteArrayRegion. */
    free(px);
    free(arr);
    free(cp);
    free(lines.v);
}

/* ══ fake-JNI overrides ════════════════════════════════════════════════════
 * cocos2d-x's JniHelper calls through jni.h's C++ inlines, which forward to
 * the ...V (va_list) forms — so those are the ones overridden, and every V
 * form the engine can reach is covered (playbook §4: a missing V override
 * returns jnienv.c's GLOBAL sentinel). */

static char *
cc_jstr(void *s)
{
    return s ? dup_jstring(global, s) : strdup("");
}

static jobject
cc_CallStaticObjectMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    /* Cocos2dxHelper */
    if (method_is(getCocos2dxPackageName)) return (*env)->NewStringUTF(env, "com.lucasarts.tinydeathstar_goo");
    if (method_is(getCocos2dxWritablePath)) {
        /* Context.getDir("data", 0) → /data/data/<pkg>/app_data */
        /* recursive_mkdir only creates up to the last '/', so make the
         * directory with a trailing slash and hand Java's form (none) back.
         * Missing, it silently broke UserDefault.xml and FMOD's bank copies. */
        char path[PATH_MAX], dir[PATH_MAX];
        snprintf(path, sizeof(path), "%sapp_data", cocos2dx_priv.home);
        snprintf(dir, sizeof(dir), "%s/", path);
        global->recursive_mkdir(dir);
        cc_log_once(method, "%s", path);
        return (*env)->NewStringUTF(env, path);
    }
    if (method_is(getCurrentLanguage) || method_is(getLocaleLanguageCode))
        return (*env)->NewStringUTF(env, "en");
    if (method_is(getLocaleCountryCode)) return (*env)->NewStringUTF(env, "US");
    if (method_is(getDeviceModel))       return (*env)->NewStringUTF(env, "TouchPad");
    if (method_is(getAssetManager))      return (jobject)&inert_object;

    /* tds (the game's activity) */
    if (method_is(getInstance) || method_is(getContext)) return (jobject)&tds_class;
    if (method_is(getVersionNumber)) return (*env)->NewStringUTF(env, "1.4.1");
    if (method_is(getProxyHostAndPort) || method_is(getClipboard) ||
        method_is(getProxySetting))                 /* msdk Util: no proxy */
        return (*env)->NewStringUTF(env, "");
    if (method_is(getUUIDv4)) {
        /* UUID.randomUUID().toString().toUpperCase() */
        static int seeded = 0;
        char u[40];
        unsigned char b[16];
        int i;
        if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
        for (i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xff);
        b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
        b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
        snprintf(u, sizeof(u), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                 b[10], b[11], b[12], b[13], b[14], b[15]);
        return (*env)->NewStringUTF(env, u);
    }
    if (method_is(formatNumber)) {
        /* NumberFormat.getIntegerInstance() in en_US: grouping with ','. */
        int v = va_arg(args, int);
        char raw[16], out[24];
        int len, o = 0, i, neg = v < 0;
        snprintf(raw, sizeof(raw), "%u", neg ? (unsigned)(-(long long)v) : (unsigned)v);
        len = (int)strlen(raw);
        if (neg) out[o++] = '-';
        for (i = 0; i < len; i++) {
            if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
            out[o++] = raw[i];
        }
        out[o] = '\0';
        return (*env)->NewStringUTF(env, out);
    }

    /* Cocos2dxBitmap.getStringWithEllipsis(String, float width, float fontSize):
     * only CCEditBox uses it; answer with the string unchanged. */
    if (method_is(getStringWithEllipsis)) {
        jstring s = va_arg(args, jstring);
        return (jobject)s;
    }

    cc_trace_unhandled("obj", method);
    return NULL;
}

static jobject
cc_CallObjectMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    cc_trace_unhandled("obj(inst)", method);
    return NULL;
}

static jint
cc_CallStaticIntMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    if (method_is(playEffect)) {
        /* SimpleAudioEngine through Java SoundPool. This game plays sound via
         * FMOD, so this path should stay unused; log it if it is not. */
        char *path = cc_jstr(va_arg(args, jstring));
        cc_log_once(method, "'%s' (SimpleAudioEngine path — not implemented)", path);
        free(path);
        return 0;
    }
    /* IAPBridge.GetStoreType: IAPManager.getStoreType() is neither
     * "googleplay" nor "amazon" here → StoreType.kStoreType_Unknown.ordinal()
     * = 0. Polled every frame, so answered explicitly (not via the tracer). */
    if (method_is(GetStoreType)) return 0;
    if (method_is(getFontSizeAccordingHeight)) {
        /* Cocos2dxBitmap: the text size whose bounds height fits `height`. */
        int h = va_arg(args, int);
        return h > 0 ? h * 3 / 4 : 1;
    }
    cc_trace_unhandled("int", method);
    return 0;
}

static jint
cc_CallIntMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    cc_trace_unhandled("int(inst)", method);
    return 0;
}

static jfloat
cc_CallStaticFloatMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    if (method_is(getBackgroundMusicVolume) || method_is(getEffectsVolume))
        return 1.0f;
    cc_trace_unhandled("float", method);
    return 0.0f;
}

static jboolean
cc_CallStaticBooleanMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    /* No network on this device — the state the game already handles. */
    if (method_is(isOnline)) {
        cc_log_once(method, "-> false (offline)");
        return JNI_FALSE;
    }
    /* Configuration.screenLayout SIZE_XLARGE on a 9.7" TouchPad. */
    if (method_is(isTablet)) {
        cc_log_once(method, "-> true");
        return JNI_TRUE;
    }
    if (method_is(isBackgroundMusicPlaying)) return JNI_FALSE;
    if (method_is(backgroundApp)) {
        cc_log_once(method, "moveTaskToBack — ignored");
        return JNI_FALSE;
    }
    cc_trace_unhandled("bool", method);
    return JNI_FALSE;
}

static jboolean
cc_CallBooleanMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    cc_trace_unhandled("bool(inst)", method);
    return JNI_FALSE;
}

static void
cc_CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID method, va_list args)
{
    if (method_is(createTextBitmap)) {
        char *text = cc_jstr(va_arg(args, jstring));
        char *font = cc_jstr(va_arg(args, jstring));
        int size  = va_arg(args, int);
        int align = va_arg(args, int);
        int w     = va_arg(args, int);
        int h     = va_arg(args, int);
        cc_create_text_bitmap(env, text, font, size, align, w, h);
        free(text);
        free(font);
        return;
    }
    if (method_is(setAnimationInterval)) {
        double d = va_arg(args, double);
        if (d > 0.0 && d < 1.0) cc_anim_interval = d;
        cc_log_once(method, "%.4f s (%.1f fps)", d, d > 0 ? 1.0 / d : 0.0);
        return;
    }
    if (method_is(showDialog)) {
        char *title = cc_jstr(va_arg(args, jstring));
        char *msg = cc_jstr(va_arg(args, jstring));
        fprintf(stderr, "[COCOS-DIALOG] showDialog('%s', '%s') — AlertDialog with an OK "
                "button that only dismisses; nothing to answer\n", title, msg);
        free(title); free(msg);
        return;
    }
    if (method_is(showEditTextDialog)) {
        char *title = cc_jstr(va_arg(args, jstring));
        char *msg = cc_jstr(va_arg(args, jstring));
        fprintf(stderr, "[COCOS-DIALOG] showEditTextDialog('%s', '%s') — no text entry here; "
                "answering with the current text\n", title, msg);
        {
            /* Cocos2dxEditBoxDialog returns the edited text through
             * Cocos2dxHelper.setEditTextDialogResult → nativeSetEditTextDialogResult
             * (on the GL thread). Answer unchanged so the caller is not left
             * waiting. */
            typedef void (*settext_t)(JNIEnv *, jclass, jbyteArray) SOFTFP;
            settext_t fn = (settext_t)global->lookup_lib_symbol("libgame",
                    "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");
            if (fn) {
                struct dummy_array *a = malloc(sizeof(*a));
                a->data = strdup(msg);
                a->element_size = 1;
                a->length = (long)strlen(msg);
                fn(env, (jclass)&cocos_helper_class, (jbyteArray)a);
                free(a->data);
                free(a);
            }
        }
        free(title); free(msg);
        return;
    }
    if (method_is(terminateProcess) || method_is(terminateActivity)) {
        fprintf(stderr, "[COCOS-HOST] %s — exiting\n", method->name);
        cocos2dx_priv.want_exit = 1;
        return;
    }

    /* Fire-and-forget services that do not exist here: ads, analytics,
     * notifications, Tapjoy, wake lock, accelerometer, SimpleAudioEngine. */
    if (method_is(logAnalyticsEventWithContext) || method_is(setAnalyticsTracking) ||
        method_is(enableUrbanAirshipPush) || method_is(scheduleLocalNotification) ||
        method_is(cancelNotifications) || method_is(setTapjoyUser) ||
        method_is(openTapjoyOfferwall) || method_is(setWakeLock) ||
        method_is(showLoadingScreenStatic) || method_is(hideLoadingScreenStatic) ||
        method_is(enableAccelerometer) || method_is(disableAccelerometer) ||
        method_is(setAccelerometerInterval) || method_is(openUrl) || method_is(openHtml) ||
        method_is(openMore) || method_is(saveToClipboard) ||
        method_is(preloadBackgroundMusic) || method_is(playBackgroundMusic) ||
        method_is(stopBackgroundMusic) || method_is(pauseBackgroundMusic) ||
        method_is(resumeBackgroundMusic) || method_is(rewindBackgroundMusic) ||
        method_is(setBackgroundMusicVolume) || method_is(setEffectsVolume) ||
        method_is(preloadEffect) || method_is(unloadEffect) || method_is(stopEffect) ||
        method_is(pauseEffect) || method_is(resumeEffect) || method_is(pauseAllEffects) ||
        method_is(resumeAllEffects) || method_is(stopAllEffects) ||
        method_is(HideBurstlyBannerBottomCenter) || method_is(ShowBurstlyBannerBottomCenter) ||
        method_is(InitBurstlyPostgameInterstitial) || method_is(InitBurstlyPregameInterstitial) ||
        method_is(ShowBurstlyFSGL) || method_is(ShowBurstlyPostgameInterstitial) ||
        method_is(ShowBurstlyPregameInterstitial)) {
        cc_log_once(method, "no-op");
        return;
    }

    cc_trace_unhandled("void", method);
}

static void
cc_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID method, va_list args)
{
    if (method_is(setCurrentActivity)) return;     /* msdk Facebook/G+ managers */
    cc_trace_unhandled("void(inst)", method);
}

static jint
cc_RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n)
{
    struct dummy_jclass *c = clazz;
    jint i;
    for (i = 0; i < n; i++)
        fprintf(stderr, "[COCOS-REG] %s.%s%s -> %p\n", c ? c->name : "?",
                methods[i].name, methods[i].signature, methods[i].fnPtr);
    return JNIEnv_RegisterNatives(env, clazz, methods, n);
}

static jthrowable cc_ExceptionOccurred(JNIEnv *env) { return NULL; }
static void       cc_ExceptionClear(JNIEnv *env) { }
static jboolean   cc_ExceptionCheck(JNIEnv *env) { return JNI_FALSE; }

/* ══ module ════════════════════════════════════════════════════════════════ */

static int
cocos2dx_try_init(struct SupportModule *self)
{
    struct SupportModulePriv *p = self->priv;
#define GET(field, name) \
    p->field = (void *)LOOKUP_LIBM("libgame", name)

    GET(nativeInit,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    GET(nativeRender,     "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
    GET(nativeSetApkPath, "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
    if (p->nativeInit == NULL || p->nativeRender == NULL || p->nativeSetApkPath == NULL)
        return 0;

    p->JNI_OnLoad_game = (jni_onload_t)LOOKUP_LIBM("libgame", "JNI_OnLoad");
    p->JNI_OnLoad_iap  = (jni_onload_t)LOOKUP_LIBM("libDMOIAPManager", "JNI_OnLoad");
    GET(nativeSetExternalAssetPath, "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetExternalAssetPath");
    GET(nativeOnPause,        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
    GET(nativeOnResume,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
    GET(nativeTouchesBegin,   "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    GET(nativeTouchesEnd,     "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    GET(nativeTouchesMove,    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    GET(nativeTouchesCancel,  "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
    GET(nativeKeyDown,        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
    GET(nativeInitBitmapDC,   "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
    GET(soundBoardBackground, "Java_com_lucasarts_tinydeathstar_tds_soundBoardBackground");
    GET(soundBoardForeground, "Java_com_lucasarts_tinydeathstar_tds_soundBoardForeground");
#undef GET

    fprintf(stderr, "[COCOS] try_init: cocos2d-x 2.0.x host (JNI_OnLoad game=%p iap=%p, "
            "bitmapDC=%p, touches=%p/%p/%p)\n",
            (void *)p->JNI_OnLoad_game, (void *)p->JNI_OnLoad_iap,
            (void *)p->nativeInitBitmapDC, (void *)p->nativeTouchesBegin,
            (void *)p->nativeTouchesMove, (void *)p->nativeTouchesEnd);

    /* Portrait game (manifest screenOrientation=1) on a landscape panel:
     * render into a 768x1024 FBO and rotate at present. APKENV_COCOS_LANDSCAPE=1
     * turns it off for comparison. */
    {
        const char *e = getenv("APKENV_COCOS_LANDSCAPE");
        p->portrait = !(e && e[0] == '1');
    }
    GLOBAL_M->module_hacks->prefer_gles_version = 2;
    if (p->portrait) {
        GLOBAL_M->module_hacks->render_to_fbo = 1;
        GLOBAL_M->module_hacks->fbo_w = 768;
        GLOBAL_M->module_hacks->fbo_h = 1024;
    }

    /* FMOD in this apk only has an OpenSL ES output. */
    apkenv_opensles_enable();

    /* libgame's gnustl reads bionic's fp->_file directly (std::ofstream is how
     * SoundBoard copies its FMOD banks out of the apk); hand out
     * bionic-layout FILEs so that read yields the real descriptor. */
    apkenv_bionic_stdio_enable();

    self->override_env.RegisterNatives          = cc_RegisterNatives;
    self->override_env.ExceptionOccurred        = cc_ExceptionOccurred;
    self->override_env.ExceptionClear           = cc_ExceptionClear;
    self->override_env.ExceptionCheck           = cc_ExceptionCheck;
    self->override_env.CallStaticObjectMethodV  = cc_CallStaticObjectMethodV;
    self->override_env.CallObjectMethodV        = cc_CallObjectMethodV;
    self->override_env.CallStaticIntMethodV     = cc_CallStaticIntMethodV;
    self->override_env.CallIntMethodV           = cc_CallIntMethodV;
    self->override_env.CallStaticFloatMethodV   = cc_CallStaticFloatMethodV;
    self->override_env.CallStaticBooleanMethodV = cc_CallStaticBooleanMethodV;
    self->override_env.CallBooleanMethodV       = cc_CallBooleanMethodV;
    self->override_env.CallStaticVoidMethodV    = cc_CallStaticVoidMethodV;
    self->override_env.CallVoidMethodV          = cc_CallVoidMethodV;
    return 1;
}

static void
cocos2dx_init(struct SupportModule *self, int width, int height, const char *home)
{
    struct SupportModulePriv *p = self->priv;
    char ext[PATH_MAX];
    global = GLOBAL_M;
    snprintf(p->home, sizeof(p->home), "%s", home);

    /* 1. System.loadLibrary order: DMOIAPManager, then game. */
    if (p->JNI_OnLoad_iap) {
        fprintf(stderr, "[COCOS] JNI_OnLoad(libDMOIAPManager)\n");
        p->JNI_OnLoad_iap(VM_M, NULL);
    }
    if (p->JNI_OnLoad_game) {
        fprintf(stderr, "[COCOS] JNI_OnLoad(libgame)\n");
        p->JNI_OnLoad_game(VM_M, NULL);
    } else {
        fprintf(stderr, "[COCOS] WARNING: libgame has no JNI_OnLoad — JniHelper will have no JavaVM\n");
    }

    /* 2. Cocos2dxHelper.init */
    fprintf(stderr, "[COCOS] nativeSetApkPath(%s)\n", global->apk_filename);
    p->nativeSetApkPath(ENV_M, (jclass)&cocos_helper_class,
                        (*ENV_M)->NewStringUTF(ENV_M, global->apk_filename));
    snprintf(ext, sizeof(ext), "%sexternal/Android/data/com.lucasarts.tinydeathstar_goo/files/assets/", home);
    global->recursive_mkdir(ext);
    if (p->nativeSetExternalAssetPath) {
        fprintf(stderr, "[COCOS] nativeSetExternalAssetPath(%s)\n", ext);
        p->nativeSetExternalAssetPath(ENV_M, (jclass)&cocos_helper_class,
                                      (*ENV_M)->NewStringUTF(ENV_M, ext));
    }

    /* 3. GLSurfaceView: onSizeChanged(w,h) → onSurfaceCreated → nativeInit(w,h) */
    if (p->portrait) {
        GLuint fb;
        width  = GLOBAL_M->module_hacks->fbo_w;
        height = GLOBAL_M->module_hacks->fbo_h;
        fb = apkenv_fbo_es2_ensure();
        fprintf(stderr, "[COCOS] portrait: %dx%d surface via FBO %u (rot=%d)\n",
                width, height, fb, apkenv_fbo_es2_rotation());
    }
    p->view_w = width;
    p->view_h = height;

    /* The first nativeRender loads the tower for ~8 s and nothing presents
     * meanwhile. On Android main.xml's loading ImageView (res splash.png,
     * fitCenter on black) covers it; here the shim is the window, so put the
     * same art up now (APKENV_SPLASH_RGB, compat/fbo_es2.c; it retires on the
     * engine's first draw). Twice: with double buffering one present leaves
     * the other buffer black. No-ops unless the env names a splash. */
    if (global->platform != NULL && global->platform->update != NULL) {
        global->platform->update();
        global->platform->update();
    }
    fprintf(stderr, "[COCOS] nativeInit(%d, %d)\n", width, height);
    p->nativeInit(ENV_M, (jclass)&cocos_renderer_class, width, height);
    fprintf(stderr, "[COCOS] nativeInit returned\n");
}

/* ── APKENV_COCOS_AUTOTAP: synthesise input for testing without a finger ──
 * "x,y@frame" taps and "x1,y1>x2,y2@frame" drags, in PANEL pixels (what SDL
 * reports), fed through cocos2dx_input() so the rotation and the engine's
 * contract are exercised too. Diagnostic only; never in a shipped env. */
#define CC_AUTOTAP_MAX 32
#define CC_SWIPE_STEPS 8
struct cc_autotap { int x0, y0, x1, y1, swipe, stage; unsigned long frame; };
static struct cc_autotap autotap[CC_AUTOTAP_MAX];
static int autotap_n = -1;

static void cocos2dx_input(struct SupportModule *self, int event, int x, int y, int finger);

static void
cc_autotap_run(struct SupportModule *self)
{
    int i;
    if (autotap_n < 0) {
        const char *e = getenv("APKENV_COCOS_AUTOTAP");
        autotap_n = 0;
        while (e && *e && autotap_n < CC_AUTOTAP_MAX) {
            struct cc_autotap *a = &autotap[autotap_n];
            unsigned long f;
            if (sscanf(e, "%d,%d>%d,%d@%lu", &a->x0, &a->y0, &a->x1, &a->y1, &f) == 5) {
                a->swipe = 1; a->frame = f; a->stage = 0; autotap_n++;
            } else if (sscanf(e, "%d,%d@%lu", &a->x0, &a->y0, &f) == 3) {
                a->x1 = a->x0; a->y1 = a->y0; a->swipe = 0; a->frame = f; a->stage = 0; autotap_n++;
            }
            e = strchr(e, ';');
            if (e) e++;
        }
        if (autotap_n)
            fprintf(stderr, "[COCOS-AUTOTAP] %d synthetic gesture(s) queued\n", autotap_n);
    }
    for (i = 0; i < autotap_n; i++) {
        struct cc_autotap *a = &autotap[i];
        long step;
        if (a->stage < 0) continue;
        if (a->stage == 0) {
            if (cc_frames != a->frame) continue;
            fprintf(stderr, "[COCOS-AUTOTAP] down (%d,%d) frame %lu\n", a->x0, a->y0, cc_frames);
            cocos2dx_input(self, ACTION_DOWN, a->x0, a->y0, 0);
            a->stage = 1;
            continue;
        }
        step = (long)(cc_frames - a->frame);
        if (step != a->stage) continue;
        if (a->stage <= CC_SWIPE_STEPS) {
            int x = a->x0 + (a->x1 - a->x0) * a->stage / CC_SWIPE_STEPS;
            int y = a->y0 + (a->y1 - a->y0) * a->stage / CC_SWIPE_STEPS;
            if (a->swipe) cocos2dx_input(self, ACTION_MOVE, x, y, 0);
            a->stage++;
        } else {
            fprintf(stderr, "[COCOS-AUTOTAP] up (%d,%d)\n", a->x1, a->y1);
            cocos2dx_input(self, ACTION_UP, a->x1, a->y1, 0);
            a->stage = -1;
        }
    }
}

static void
cocos2dx_input(struct SupportModule *self, int event, int x, int y, int finger)
{
    struct SupportModulePriv *p = self->priv;
    float fx = (float)x, fy = (float)y;
    static unsigned long logged = 0;

    /* Panel pixels → the engine's portrait view pixels, along the same
     * rotation the present uses (derivation in modules/unity.c). */
    if (p->portrait) {
        int sw = 0, sh = 0;
        GLOBAL_M->platform->get_size(&sw, &sh);
        if (sw > 0 && sh > 0) {
            if (apkenv_fbo_es2_rotation() == 3) {
                fx = (float)p->view_w * (float)y / (float)sh;
                fy = (float)p->view_h * (1.0f - (float)x / (float)sw);
            } else {
                fx = (float)p->view_w * (1.0f - (float)y / (float)sh);
                fy = (float)p->view_h * ((float)x / (float)sw);
            }
        }
    }
    if (fx < 0) fx = 0; if (fx > p->view_w - 1) fx = (float)(p->view_w - 1);
    if (fy < 0) fy = 0; if (fy > p->view_h - 1) fy = (float)(p->view_h - 1);

    if (logged++ < 60)
        fprintf(stderr, "[COCOS-TOUCH] ev=%d id=%d panel=(%d,%d) -> view=(%.0f,%.0f)\n",
                event, finger, x, y, fx, fy);

    if (event == ACTION_DOWN) {
        if (p->nativeTouchesBegin)
            p->nativeTouchesBegin(ENV_M, (jclass)&cocos_renderer_class, finger, fx, fy);
    } else if (event == ACTION_UP) {
        if (p->nativeTouchesEnd)
            p->nativeTouchesEnd(ENV_M, (jclass)&cocos_renderer_class, finger, fx, fy);
    } else if (event == ACTION_MOVE) {
        if (p->nativeTouchesMove) {
            jint ids[1] = { finger };
            jfloat xs[1] = { fx }, ys[1] = { fy };
            struct dummy_array aid = { ids, sizeof(jint), 1 };
            struct dummy_array ax = { xs, sizeof(jfloat), 1 };
            struct dummy_array ay = { ys, sizeof(jfloat), 1 };
            p->nativeTouchesMove(ENV_M, (jclass)&cocos_renderer_class,
                                 (jintArray)&aid, (jfloatArray)&ax, (jfloatArray)&ay);
        }
    }
}

static void
cocos2dx_key_input(struct SupportModule *self, int event, int keycode, int unicode)
{
    /* Cocos2dxGLSurfaceView.onKeyDown forwards only BACK (4) and MENU (82). */
    if (event == ACTION_DOWN && (keycode == 4 || keycode == 82) && self->priv->nativeKeyDown) {
        fprintf(stderr, "[COCOS] nativeKeyDown(%d)\n", keycode);
        self->priv->nativeKeyDown(ENV_M, (jclass)&cocos_renderer_class, keycode);
    }
}

static void
cocos2dx_update(struct SupportModule *self)
{
    cc_autotap_run(self);

    cc_frames++;
    {
        static struct timeval t0;
        static unsigned long mark = 0;
        struct timeval now;
        gettimeofday(&now, NULL);
        if (mark == 0) { t0 = now; mark = cc_frames; }
        else if (cc_frames - mark >= 300) {
            double secs = (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6;
            fprintf(stderr, "[COCOS-FPS] %.1f fps over %lu frames\n",
                    (cc_frames - mark) / secs, cc_frames - mark);
            t0 = now; mark = cc_frames;
        }
    }

    {
        static struct timeval last;
        static double worst = 0.0;
        static unsigned long worst_mark = 0;
        struct timeval a, b;
        double used, busy;
        gettimeofday(&a, NULL);
        self->priv->nativeRender(ENV_M, (jclass)&cocos_renderer_class);
        gettimeofday(&b, NULL);
        busy = (b.tv_sec - a.tv_sec) + (b.tv_usec - a.tv_usec) / 1e6;
        if (busy > worst) worst = busy;
        if (cc_frames - worst_mark >= 300) {
            fprintf(stderr, "[COCOS-FPS] slowest nativeRender in the last 300 frames: %.1f ms\n",
                    worst * 1000.0);
            worst = 0.0; worst_mark = cc_frames;
        }
        if (cc_frames == 1)
            fprintf(stderr, "[COCOS] nativeRender #1 returned\n");

        /* Cocos2dxRenderer.onDrawFrame sleeps out the rest of the animation
         * interval; without it the loop renders frames the game never asked
         * for and takes CPU from the FMOD mixer. APKENV_COCOS_NO_PACING=1
         * disables it for comparison. */
        {
            static int pacing = -1;
            if (pacing < 0) {
                const char *e = getenv("APKENV_COCOS_NO_PACING");
                pacing = !(e && e[0] == '1');
            }
            if (pacing && last.tv_sec != 0) {
                used = (b.tv_sec - last.tv_sec) + (b.tv_usec - last.tv_usec) / 1e6;
                if (used < cc_anim_interval)
                    usleep((useconds_t)((cc_anim_interval - used) * 1e6));
            }
            gettimeofday(&last, NULL);
        }
    }
}

static void
cocos2dx_deinit(struct SupportModule *self)
{
    if (self->priv->nativeOnPause)
        self->priv->nativeOnPause(ENV_M, (jclass)&cocos_renderer_class);
}

static void
cocos2dx_pause(struct SupportModule *self)
{
    fprintf(stderr, "[COCOS] pause\n");
    if (self->priv->soundBoardBackground)
        self->priv->soundBoardBackground(ENV_M, (jclass)&tds_class);
    if (self->priv->nativeOnPause)
        self->priv->nativeOnPause(ENV_M, (jclass)&cocos_renderer_class);
}

static void
cocos2dx_resume(struct SupportModule *self)
{
    fprintf(stderr, "[COCOS] resume\n");
    if (self->priv->nativeOnResume)
        self->priv->nativeOnResume(ENV_M, (jclass)&cocos_renderer_class);
    if (self->priv->soundBoardForeground)
        self->priv->soundBoardForeground(ENV_M, (jclass)&tds_class);
}

static int
cocos2dx_requests_exit(struct SupportModule *self)
{
    return self->priv->want_exit;
}

APKENV_MODULE(cocos2dx, MODULE_PRIORITY_ENGINE)
