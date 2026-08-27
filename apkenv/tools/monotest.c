/*
 * monotest.c - Checkpoint C: prove the natively-built Mono runtime works on
 * webOS *without* libunity, apkenv, or any bionic code in the process.
 *
 * If this passes, JIT + Boehm GC + Unity's corlib are all good on the device,
 * and any later failure is in the bridge or in libunity's contract - not in
 * the runtime. If it fails, there is no point wiring up the bridge at all.
 *
 * The runtime is dlopen()ed rather than linked, both to avoid the SONAME
 * (libmono.so.0) and because that is exactly what compat/hostlib.c does.
 *
 * Build (host):
 *   /opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc -march=armv7-a \
 *       -mfloat-abi=softfp -mfpu=vfp -O2 -o monotest tools/monotest.c -ldl
 * Run (device):
 *   ./monotest <libmono-webos.so> <Managed-dir> [etc-dir]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void MonoDomain;
typedef void MonoAssembly;
typedef void MonoImage;
typedef void MonoClass;
typedef void MonoMethod;
typedef void MonoObject;
typedef void MonoString;

static void *H;
static int failures;

static void *
sym(const char *name)
{
    void *p;
    dlerror();
    p = dlsym(H, name);
    if (p == NULL) {
        fprintf(stderr, "  !! dlsym(%s) failed: %s\n", name, dlerror());
        failures++;
    }
    return p;
}

int
main(int argc, char **argv)
{
    const char *libpath = (argc > 1) ? argv[1] : "./libmono-webos.so";
    const char *managed = (argc > 2) ? argv[2] : "./Managed";
    const char *etcdir  = (argc > 3) ? argv[3] : "./etc";

    MonoDomain *domain;
    MonoImage *corlib;
    MonoClass *klass;
    MonoMethod *method;
    MonoObject *res, *exc;
    MonoString *str;
    char *utf8;

    void  (*p_set_dirs)(const char *, const char *);
    void  (*p_set_assemblies_path)(const char *);
    void  (*p_register_machine_config)(const char *);
    MonoDomain *(*p_jit_init_version)(const char *, const char *);
    MonoImage  *(*p_get_corlib)(void);
    MonoClass  *(*p_class_from_name)(MonoImage *, const char *, const char *);
    MonoMethod *(*p_class_get_method_from_name)(MonoClass *, const char *, int);
    MonoObject *(*p_runtime_invoke)(MonoMethod *, void *, void **, MonoObject **);
    void       *(*p_object_unbox)(MonoObject *);
    MonoString *(*p_string_new_wrapper)(const char *);
    char       *(*p_string_to_utf8)(MonoString *);
    MonoClass  *(*p_object_get_class)(MonoObject *);
    void        (*p_jit_cleanup)(MonoDomain *);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[monotest] lib=%s managed=%s etc=%s\n", libpath, managed, etcdir);

    H = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (H == NULL) {
        fprintf(stderr, "[monotest] FATAL dlopen: %s\n", dlerror());
        return 2;
    }
    printf("[monotest] dlopen OK\n");

    p_set_dirs                  = sym("mono_set_dirs");
    p_set_assemblies_path       = sym("mono_set_assemblies_path");
    p_register_machine_config   = sym("mono_register_machine_config");
    p_jit_init_version          = sym("mono_jit_init_version");
    p_get_corlib                = sym("mono_get_corlib");
    p_class_from_name           = sym("mono_class_from_name");
    p_class_get_method_from_name= sym("mono_class_get_method_from_name");
    p_runtime_invoke            = sym("mono_runtime_invoke");
    p_object_unbox              = sym("mono_object_unbox");
    p_string_new_wrapper        = sym("mono_string_new_wrapper");
    p_string_to_utf8            = sym("mono_string_to_utf8");
    p_object_get_class          = sym("mono_object_get_class");
    p_jit_cleanup               = sym("mono_jit_cleanup");
    if (failures) {
        fprintf(stderr, "[monotest] FATAL %d symbols missing\n", failures);
        return 2;
    }
    printf("[monotest] all symbols resolved\n");

    /* Minimal machine.config so the runtime does not go looking for a file we
     * do not ship. This is also what libunity does (mono_register_machine_config
     * is called before mono_jit_init_version). */
    p_register_machine_config(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><configuration>"
        "<configSections></configSections></configuration>");

    p_set_dirs(managed, etcdir);
    p_set_assemblies_path(managed);
    printf("[monotest] dirs set; calling mono_jit_init_version...\n");

    /* This is where the bionic build dies. Everything below is bonus. */
    domain = p_jit_init_version("monotest Root Domain", "v2.0.50727");
    if (domain == NULL) {
        fprintf(stderr, "[monotest] FAIL mono_jit_init_version returned NULL\n");
        return 1;
    }
    printf("[monotest] mono_jit_init_version OK (domain=%p)\n", domain);

    corlib = p_get_corlib();
    printf("[monotest] corlib=%p\n", corlib);
    if (corlib == NULL) {
        fprintf(stderr, "[monotest] FAIL no corlib\n");
        return 1;
    }

    /* GC allocation + string marshalling round-trip. */
    str = p_string_new_wrapper("round-trip");
    utf8 = str ? p_string_to_utf8(str) : NULL;
    printf("[monotest] string round-trip: %s\n", utf8 ? utf8 : "(FAILED)");
    if (utf8 == NULL || strcmp(utf8, "round-trip") != 0) {
        fprintf(stderr, "[monotest] FAIL string round-trip\n");
        return 1;
    }

    /* JIT-compile and run real managed code: System.Environment.get_TickCount. */
    klass = p_class_from_name(corlib, "System", "Environment");
    if (klass == NULL) {
        fprintf(stderr, "[monotest] FAIL no System.Environment\n");
        return 1;
    }
    method = p_class_get_method_from_name(klass, "get_TickCount", 0);
    if (method == NULL) {
        fprintf(stderr, "[monotest] FAIL no get_TickCount\n");
        return 1;
    }
    exc = NULL;
    res = p_runtime_invoke(method, NULL, NULL, &exc);
    if (exc != NULL) {
        fprintf(stderr, "[monotest] FAIL managed exception from get_TickCount\n");
        return 1;
    }
    printf("[monotest] JIT ran managed code: Environment.TickCount = %d\n",
           res ? *(int *)p_object_unbox(res) : -1);

    /* System.Version via Environment.Version, then ToString() on it - the
     * check the plan asks for. Exercises reflection + instance invoke. */
    method = p_class_get_method_from_name(klass, "get_Version", 0);
    if (method != NULL) {
        exc = NULL;
        res = p_runtime_invoke(method, NULL, NULL, &exc);
        if (res != NULL && exc == NULL) {
            MonoClass *vclass = p_object_get_class(res);
            MonoMethod *tostr = p_class_get_method_from_name(vclass, "ToString", 0);
            if (tostr != NULL) {
                exc = NULL;
                str = p_runtime_invoke(tostr, res, NULL, &exc);
                if (str != NULL && exc == NULL) {
                    utf8 = p_string_to_utf8(str);
                    printf("[monotest] System.Environment.Version = %s\n",
                           utf8 ? utf8 : "(null)");
                }
            }
        }
    }

    printf("[monotest] calling mono_jit_cleanup...\n");
    p_jit_cleanup(domain);
    printf("[monotest] PASS\n");
    return 0;
}
