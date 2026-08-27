/**
 * apkenv - render-to-FBO + rotated present, GLES2 edition.
 *
 * A portrait game on this landscape-only panel renders into an offscreen
 * portrait FBO which is blitted, rotated, to the real framebuffer once per
 * frame. gles_wrappers.c has had that path since Where's My Water, but it is
 * fixed-function ES1: matrix stack, glTexEnv, client arrays. Temple Run 2 runs
 * on Unity's GLES2 device, so it needs the same idea expressed with a shader.
 *
 * Sizes work out exactly: 768x1024 rotated 90 degrees is 1024x768, so the
 * portrait image fills the panel with no letterboxing and no scaling.
 *
 * The engine is never told any of this happens - its glBindFramebuffer(0) is
 * redirected to our FBO (gles2_wrappers.c), so it believes it owns the screen.
 * Everything this file touches is saved and restored around the blit, because
 * the engine caches GL state across frames and will not re-set what it believes
 * it already set.
 *
 *   APKENV_FBO_ROT=1|3   90 (default) or 270 degrees
 */
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../apkenv.h"
#include "hooks.h"

extern struct GlobalState global;
extern struct ModuleHacks global_module_hacks;

/* Resolved from the ES2 device library directly: this file must reach the same
 * driver front-end that owns the live context, not the ES1 one. */
static struct {
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*glTexParameteri)(GLenum, GLenum, GLint);
    void (*glGenRenderbuffers)(GLsizei, GLuint *);
    void (*glBindRenderbuffer)(GLenum, GLuint);
    void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*glGenFramebuffers)(GLsizei, GLuint *);
    void (*glBindFramebuffer)(GLenum, GLuint);
    void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (*glCheckFramebufferStatus)(GLenum);
    GLuint (*glCreateShader)(GLenum);
    void (*glShaderSource)(GLuint, GLsizei, const char * const *, const GLint *);
    void (*glCompileShader)(GLuint);
    void (*glGetShaderiv)(GLuint, GLenum, GLint *);
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    GLuint (*glCreateProgram)(void);
    void (*glAttachShader)(GLuint, GLuint);
    void (*glBindAttribLocation)(GLuint, GLuint, const char *);
    void (*glLinkProgram)(GLuint);
    void (*glGetProgramiv)(GLuint, GLenum, GLint *);
    void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void (*glUseProgram)(GLuint);
    GLint (*glGetUniformLocation)(GLuint, const char *);
    void (*glUniform1i)(GLint, GLint);
    void (*glActiveTexture)(GLenum);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glDisable)(GLenum);
    void (*glEnable)(GLenum);
    GLboolean (*glIsEnabled)(GLenum);
    void (*glGetIntegerv)(GLenum, GLint *);
    void (*glEnableVertexAttribArray)(GLuint);
    void (*glDisableVertexAttribArray)(GLuint);
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*glGetVertexAttribiv)(GLuint, GLenum, GLint *);
    void (*glGetVertexAttribPointerv)(GLuint, GLenum, void **);
    void (*glDrawArrays)(GLenum, GLint, GLsizei);
    void (*glBindBuffer)(GLenum, GLuint);
    GLenum (*glGetError)(void);
} f;

static int   es2_ready;
static int   es2_failed;
static int   es2_rot = -1;
static GLuint fbo_id, fbo_tex, fbo_depth, prog;
static GLint  u_tex;

#define GET(name) do {                                                        \
        *(void **)&f.name = dlsym(h, #name);                                  \
        if (f.name == NULL) { missing = #name; }                              \
    } while (0)

static int
es2_resolve(void)
{
    const char *missing = NULL;
    void *h = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
    if (h == NULL) h = dlopen("libGLESv2.so", RTLD_LAZY);
    if (h == NULL) {
        fprintf(stderr, "[FBO2] libGLESv2.so unavailable: %s\n", dlerror());
        return 0;
    }
    GET(glGenTextures); GET(glBindTexture); GET(glTexImage2D); GET(glTexParameteri);
    GET(glGenRenderbuffers); GET(glBindRenderbuffer); GET(glRenderbufferStorage);
    GET(glGenFramebuffers); GET(glBindFramebuffer); GET(glFramebufferTexture2D);
    GET(glFramebufferRenderbuffer); GET(glCheckFramebufferStatus);
    GET(glCreateShader); GET(glShaderSource); GET(glCompileShader);
    GET(glGetShaderiv); GET(glGetShaderInfoLog);
    GET(glCreateProgram); GET(glAttachShader); GET(glBindAttribLocation);
    GET(glLinkProgram); GET(glGetProgramiv); GET(glGetProgramInfoLog);
    GET(glUseProgram); GET(glGetUniformLocation); GET(glUniform1i);
    GET(glActiveTexture); GET(glViewport); GET(glDisable); GET(glEnable);
    GET(glIsEnabled); GET(glGetIntegerv);
    GET(glEnableVertexAttribArray); GET(glDisableVertexAttribArray);
    GET(glVertexAttribPointer); GET(glGetVertexAttribiv); GET(glGetVertexAttribPointerv);
    GET(glDrawArrays); GET(glBindBuffer); GET(glGetError);
    if (missing != NULL) {
        fprintf(stderr, "[FBO2] libGLESv2.so is missing %s - no portrait present\n", missing);
        return 0;
    }
    return 1;
}

static GLuint
es2_shader(GLenum type, const char *src)
{
    GLuint s = f.glCreateShader(type);
    GLint ok = 0;
    f.glShaderSource(s, 1, &src, NULL);
    f.glCompileShader(s);
    f.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0;
        log[0] = 0;
        f.glGetShaderInfoLog(s, sizeof(log) - 1, &n, log);
        log[(n > 0 && n < (GLsizei)sizeof(log)) ? n : 0] = 0;
        fprintf(stderr, "[FBO2] blit %s shader failed: %s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        return 0;
    }
    return s;
}

/* Create the offscreen portrait target and the blit program. Returns the FBO
 * name, or 0 if unavailable (in which case the caller must not redirect). */
GLuint
apkenv_fbo_es2_ensure(void)
{
    static const char *vs_src =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";
    GLuint vs, fs;
    GLint ok = 0;
    GLenum status;
    int w, h;

    if (es2_ready) return fbo_id;
    if (es2_failed || !global_module_hacks.render_to_fbo) return 0;

    w = global_module_hacks.fbo_w;
    h = global_module_hacks.fbo_h;
    if (w <= 0 || h <= 0) return 0;

    if (!es2_resolve()) { es2_failed = 1; return 0; }

    if (es2_rot < 0) {
        const char *r = getenv("APKENV_FBO_ROT");
        es2_rot = r ? (atoi(r) & 3) : 1;      /* 90 degrees by default */
    }

    f.glGenTextures(1, &fbo_tex);
    f.glBindTexture(GL_TEXTURE_2D, fbo_tex);
    f.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    f.glGenRenderbuffers(1, &fbo_depth);
    f.glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
    f.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    f.glGenFramebuffers(1, &fbo_id);
    f.glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
    f.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    f.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);

    status = f.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[FBO2] framebuffer incomplete: 0x%x - no portrait present\n", status);
        es2_failed = 1;
        f.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    vs = es2_shader(GL_VERTEX_SHADER, vs_src);
    fs = es2_shader(GL_FRAGMENT_SHADER, fs_src);
    if (vs == 0 || fs == 0) { es2_failed = 1; return 0; }
    prog = f.glCreateProgram();
    f.glAttachShader(prog, vs);
    f.glAttachShader(prog, fs);
    f.glBindAttribLocation(prog, 0, "aPos");
    f.glBindAttribLocation(prog, 1, "aUV");
    f.glLinkProgram(prog);
    f.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; GLsizei n = 0;
        log[0] = 0;
        f.glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[(n > 0 && n < (GLsizei)sizeof(log)) ? n : 0] = 0;
        fprintf(stderr, "[FBO2] blit program link failed: %s\n", log);
        es2_failed = 1;
        return 0;
    }
    u_tex = f.glGetUniformLocation(prog, "uTex");

    es2_ready = 1;
    fprintf(stderr, "[FBO2] portrait FBO ready %dx%d id=%u tex=%u rot=%d\n",
            w, h, fbo_id, fbo_tex, es2_rot);
    /* leave it bound: the engine's fb-0 binds land here */
    return fbo_id;
}

GLuint
apkenv_fbo_es2_id(void)
{
    return es2_ready ? fbo_id : 0;
}

int
apkenv_fbo_es2_rotation(void)
{
    return es2_rot < 0 ? 1 : es2_rot;
}

/* Blit the portrait FBO to the real framebuffer, rotated. Called once per
 * frame from the platform's present, before SDL_GL_SwapBuffers. */
void
apkenv_fbo_es2_present(void)
{
    /* NDC triangle strip: BL, BR, TL, TR */
    static const GLfloat verts[8] = { -1,-1,  1,-1,  -1,1,  1,1 };
    /* texture origin is bottom-left; 90/270 map portrait onto landscape */
    static const GLfloat texc[4][8] = {
        { 0,0,  1,0,  0,1,  1,1 },   /* 0   */
        { 0,1,  0,0,  1,1,  1,0 },   /* 90  */
        { 1,1,  0,1,  1,0,  0,0 },   /* 180 */
        { 1,0,  1,1,  0,0,  0,1 },   /* 270 */
    };
    GLint s_prog = 0, s_atex = GL_TEXTURE0, s_tex = 0, s_vp[4];
    GLint s_arraybuf = 0, s_elembuf = 0;
    GLboolean e_depth, e_cull, e_blend, e_scissor, e_stencil, e_dither;
    struct { GLint enabled, size, type, norm, stride, buf; void *ptr; } attr[2];
    int sw = 0, sh = 0, i;

    if (!es2_ready || !global_module_hacks.render_to_fbo)
        return;

    global.platform->get_size(&sw, &sh);

    {   /* who owns the framebuffer when the frame ends? if it is 0, something
         * bound the window directly, bypassing the redirect */
        static int n; GLint cur = -1;
        extern unsigned long apkenv_gl_draws;
        static unsigned long last_draws;
        n++;
        if (n <= 12 || (n % 100) == 0) {
            f.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
            fprintf(stderr, "[FBO2] present #%d: draws since last present=%lu, FRAMEBUFFER_BINDING=%d\n",
                    n, apkenv_gl_draws - last_draws, cur);
        }
        last_draws = apkenv_gl_draws;
    }

    /* ---- save everything we touch ---------------------------------------
     * The engine sets GL state once and assumes it persists; a present that
     * leaves the program, texture or attrib arrays changed corrupts the next
     * frame in ways that look like a game bug. */
    f.glGetIntegerv(GL_CURRENT_PROGRAM, &s_prog);
    f.glGetIntegerv(GL_ACTIVE_TEXTURE, &s_atex);
    f.glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex);
    f.glGetIntegerv(GL_VIEWPORT, s_vp);
    f.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_arraybuf);
    f.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s_elembuf);
    e_depth   = f.glIsEnabled(GL_DEPTH_TEST);
    e_cull    = f.glIsEnabled(GL_CULL_FACE);
    e_blend   = f.glIsEnabled(GL_BLEND);
    e_scissor = f.glIsEnabled(GL_SCISSOR_TEST);
    e_stencil = f.glIsEnabled(GL_STENCIL_TEST);
    e_dither  = f.glIsEnabled(GL_DITHER);
    for (i = 0; i < 2; i++) {
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr[i].enabled);
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr[i].size);
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr[i].type);
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attr[i].norm);
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr[i].stride);
        f.glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attr[i].buf);
        f.glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attr[i].ptr);
    }

    /* ---- draw the rotated quad ------------------------------------------ */
    f.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    f.glViewport(0, 0, sw, sh);
    f.glDisable(GL_DEPTH_TEST);
    f.glDisable(GL_CULL_FACE);
    f.glDisable(GL_BLEND);
    f.glDisable(GL_SCISSOR_TEST);
    f.glDisable(GL_STENCIL_TEST);
    f.glDisable(GL_DITHER);
    f.glBindBuffer(GL_ARRAY_BUFFER, 0);          /* our pointers are client-side */
    f.glUseProgram(prog);
    f.glActiveTexture(GL_TEXTURE0);
    f.glBindTexture(GL_TEXTURE_2D, fbo_tex);
    f.glUniform1i(u_tex, 0);
    f.glEnableVertexAttribArray(0);
    f.glEnableVertexAttribArray(1);
    f.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    f.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texc[es2_rot & 3]);
    f.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* ---- restore --------------------------------------------------------- */
    for (i = 0; i < 2; i++) {
        f.glBindBuffer(GL_ARRAY_BUFFER, (GLuint)attr[i].buf);
        f.glVertexAttribPointer((GLuint)i, attr[i].size, (GLenum)attr[i].type,
                                (GLboolean)attr[i].norm, attr[i].stride, attr[i].ptr);
        if (attr[i].enabled) f.glEnableVertexAttribArray(i);
        else                 f.glDisableVertexAttribArray(i);
    }
    f.glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s_arraybuf);
    f.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s_elembuf);
    f.glBindTexture(GL_TEXTURE_2D, (GLuint)s_tex);
    f.glActiveTexture((GLenum)s_atex);
    f.glUseProgram((GLuint)s_prog);
    f.glViewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    if (e_depth)   f.glEnable(GL_DEPTH_TEST);
    if (e_cull)    f.glEnable(GL_CULL_FACE);
    if (e_blend)   f.glEnable(GL_BLEND);
    if (e_scissor) f.glEnable(GL_SCISSOR_TEST);
    if (e_stencil) f.glEnable(GL_STENCIL_TEST);
    if (e_dither)  f.glEnable(GL_DITHER);

    /* back to our offscreen target for the next frame */
    f.glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);

    {
        static int n;
        if ((n++ % 300) == 0) {
            GLenum e = f.glGetError();
            if (e != GL_NO_ERROR)
                fprintf(stderr, "[FBO2] GL error 0x%x after present\n", e);
        }
    }
}

/* Bind the real window framebuffer / go back to the offscreen one. Used by the
 * platform's frame grab: after present we leave the offscreen FBO bound for the
 * next frame, so a glReadPixels there would capture the un-rotated portrait
 * target at the wrong width - which looks like a corrupted, tiled screen and
 * sends you hunting a rendering bug that is not there. */
void
apkenv_fbo_es2_bind_screen(void)
{
    if (es2_ready) f.glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void
apkenv_fbo_es2_bind_offscreen(void)
{
    if (es2_ready) f.glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
}

