# Writing an apkenv module

A module stands in for the game's Java `Activity`. It finds the engine's JNI entry points, calls them
in the order the real Java host did, and answers the Java methods the engine calls back. Read
`PORTING-PLAYBOOK.md` first. It covers deriving that contract from the game's own smali before
writing any code.

## Which module to start from

| Engine | Module | Worked example (trail) |
|---|---|---|
| Unity 3.5 / 4.0 / 4.2 + Mono | `unity.c` | `plan/TEMPLERUN2*.md`, `plan/ARALON.md`, `plan/ROBOCOP.md` |
| Cocos2d-x 2.x | `cocos2dx.c` | `plan/TINY-DEATH-STAR.md` |
| Halfbrick Mortar 1.8.x | `mortar.c` | `plan/FRUITNINJA.md` |
| EA BLAST (`com.ea.blast`) | `eablast.c` | `plan/DEAD-SPACE.md` |
| Marmalade / Airplay | `marmalade.c` | `plan/PVZ-HD-menu-freeze.md` |
| Rovio ka3d | `angrybirds.c` | `plan/AMAZING-ALEX.md` |
| Walaber (Disney) | `wheresmywater.c`, `wheresmywater2.c` | `android-port-shim.md`, `plan/STAGE-*.md` |

If the engine is already covered, extend that module and gate new behaviour on something that only
the new game has, such as a native method only its host registers. Shipped ports must keep their exact
code path (`plan/memory/dont-touch-shipped-ports.md`). The other files here (`cuttherope.c`,
`fruitninja.c` for Mortar 1.7, `worldofgoo.c`, `generic.c`, …) are unmaintained upstream thp/apkenv
modules. They are useful as reference but not built for webOS.

## Anatomy

A module is one C file with a private struct, nine callbacks and a registration macro.
`cocos2dx.c` (about 1,200 lines) is the cleanest complete example.

```c
#include "common.h"

struct SupportModulePriv {            /* your state: entry points, flags */
    jni_onload_t JNI_OnLoad_game;
    void (*nativeInit)(JNIEnv *, jobject, jint, jint);
    void (*nativeRender)(JNIEnv *, jobject);
};
static struct SupportModulePriv mygame_priv;

/* 1. Claim the apk: look up the engine's JNI symbols. Return 1 only if they are
 *    all present. Modules are tried from highest priority down, and the first
 *    one to return 1 wins. */
static int mygame_try_init(struct SupportModule *self) {
    self->priv->nativeInit = LOOKUP_LIBM("libgame", "Java_com_example_Renderer_nativeInit");
    self->priv->nativeRender = LOOKUP_LIBM("libgame", "Java_com_example_Renderer_nativeRender");
    self->priv->JNI_OnLoad_game = LOOKUP_LIBM("libgame", "JNI_OnLoad");
    /* Override fake-JNI entries the engine calls back through, e.g.
     * self->override_env.CallObjectMethodV = mygame_CallObjectMethodV; */
    return self->priv->nativeInit != NULL && self->priv->nativeRender != NULL;
}

/* 2. Boot, in the Java host's order. ALWAYS call the engine's JNI_OnLoad first:
 *    engines RegisterNatives their own callbacks there
 *    (plan/memory/call-the-engines-jni-onload.md). */
static void mygame_init(struct SupportModule *self, int width, int height, const char *home) {
    if (self->priv->JNI_OnLoad_game) self->priv->JNI_OnLoad_game(VM_M, NULL);
    self->priv->nativeInit(ENV_M, GLOBAL_M, width, height);
}

static void mygame_update(struct SupportModule *self)            /* once per frame */
{ self->priv->nativeRender(ENV_M, GLOBAL_M); }
static void mygame_input(struct SupportModule *self, int event, int x, int y, int finger)
{ /* event: ACTION_DOWN / ACTION_UP / ACTION_MOVE (apkenv.h); x,y in screen pixels */ }
static void mygame_key_input(struct SupportModule *self, int event, int keycode, int unicode) {}
static void mygame_pause(struct SupportModule *self) {}          /* card minimized */
static void mygame_resume(struct SupportModule *self) {}
static void mygame_deinit(struct SupportModule *self) {}         /* see exit-and-lifecycle memory */
static int  mygame_requests_exit(struct SupportModule *self) { return 0; }

APKENV_MODULE(mygame, MODULE_PRIORITY_ENGINE)   /* or _GAME / _GAME_VERSION (common.h) */
```

Useful macros from `common.h`:
- `GLOBAL_M`: the global state.
- `ENV_M` / `VM_M`: the fake `JNIEnv*` / `JavaVM*` to pass to natives.
- `LOOKUP_LIBM(lib, sym)`: resolve a symbol in a loaded apk library.
- `self->override_env` / `override_vm`: replace any fake-JNI function (defaults are in `jni/jnienv.c`).
- `jnienv_find_native_method(class, name)`: a native the engine registered via `RegisterNatives`.

Per-game switches belong in environment variables read by the module, and they ship in
`packaging/<game>/apkenv.env`. See `ENV-VARS.md` for the variables that already exist.

## Wiring it in

1. **Add the file to `SOURCES` in `build-webos.sh`.** It is a hand-kept list, and a module missing
   from it is silently absent: the binary logs "Not supported yet, but found JNI methods" and lists
   the natives it saw.
2. Create `packaging/<game>/appinfo.json` (unique `id`, `version`; bump it for every reinstall) and,
   if needed, `packaging/<game>/apkenv.env`.
3. Build and package:
   ```sh
   cd apkenv && ./build-webos.sh
   APPID=com.apkenv.<game> APK=packaging/<game>.apk \
     APPINFO=packaging/<game>/appinfo.json ENVFILE=packaging/<game>/apkenv.env \
     [HOSTLIBS=hostlibs/webos|hostlibs/unity42] [EXTRAS=packaging/extras/<game>] [DATA=<dir>] \
     packaging/build-ipk.sh
   ```
4. Iterate on the device:
   - `APPID=… tools/tr2-run.sh <log>`: full install, launch and log pull. The name is historical; it
     works for any port.
   - `APPID=… tools/push-run.sh <log>`: binary-only cycle.
   - `tools/grab.sh <name>`: screenshot of the GL frame.
   
   Logs land in `plan/logs/`.
