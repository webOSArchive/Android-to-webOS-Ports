#!/usr/bin/env python3
"""
Bake the patched libwmw.so into a Where's My Water? 1.0.2 apk - reproducibly,
from the pristine donor apk.

Pipeline:
  android-candidates/wheresmywater_1.0.2.apk  (pristine; its libwmw.so md5 f9fb998d...)
    lib/armeabi-v7a/libwmw.so + PATCHES (length-preserving ARM words, each
    verified against the original word before writing)
    -> libwmw.patched.so   (the shipped library: md5 cdf477f1..., checked)
    -> wheresmywater_patched.apk (pristine apk with that one entry replaced)
  Copy the result to apkenv/packaging/wheresmywater.apk to package it.

The shipped port needs exactly ONE patch: the std::string(NULL) boot-crash fix
(android-port-shim.md section 4). The touch-fix candidates in CANDIDATES.md were
never needed - touch was solved host-side (PDL_Init order + 0..1 coordinates).

Usage:
  python3 bake.py            # write libwmw.patched.so + wheresmywater_patched.apk
  python3 bake.py --verify   # dry-run: check offsets/original words only
"""
import struct, sys, zipfile, shutil, hashlib, os

HERE = os.path.dirname(os.path.abspath(__file__))
PATCHED_SO= os.path.join(HERE, "libwmw.patched.so")
PRISTINE  = os.path.join(HERE, "..", "..", "android-candidates", "wheresmywater_1.0.2.apk")
OUT_APK   = os.path.join(HERE, "wheresmywater_patched.apk")
LIB_ENTRY = "lib/armeabi-v7a/libwmw.so"

PRISTINE_LIB_MD5 = "f9fb998d198fc8073d8f410dc4514f3d"
SHIPPED_LIB_MD5  = "cdf477f1f396a08c5878739e3515998b"

# --- PATCHES: (file_offset, expected_original_le_word, new_le_word) ---
# .text file offset == vaddr for this .so. Length-preserving 4-byte ARM words.
PATCHES = [
    # std::string(NULL) boot crash: the game's static libstdc++ _S_construct
    # throws logic_error on a NULL char* (assets the Lite build doesn't ship).
    # Retarget the conditional branch so NULL yields an empty string.
    (0x3f891c, 0x0affffe9, 0x0affffe5),
    # --- Candidate A1 (READY; uncomment to try) -----------------------------
    # WidgetManager::touchMoved: make a moved finger UNCONDITIONALLY mark its
    # FingerInfo state active (=1), in case update()'s enter-detection skips
    # state==0 fingers. Sequence becomes: ldr r1,[ip]; mov r1,#1; (movne r1,#1);
    # str r1,[ip]  -> always stores 1. Low risk, reversible.
    # (0x1eaa0c, 0xe3510000, 0xe3a01001),   # cmp r1,#0   -> mov r1,#1
    # (0x1eaa14, 0x158c1000, 0xe58c1000),   # strne r1,[ip] -> str r1,[ip]
]

def md5(b): return hashlib.md5(b).hexdigest()

def apply_patches(data, patches, verify_only=False):
    data = bytearray(data)
    for off, orig, new in patches:
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != orig:
            raise SystemExit("PATCH MISMATCH @0x%x: found 0x%08x, expected 0x%08x" % (off, cur, orig))
        if not verify_only:
            struct.pack_into("<I", data, off, new)
        print("  %s @0x%x: 0x%08x -> 0x%08x" % ("CHECK" if verify_only else "PATCH", off, orig, new))
    return bytes(data)

def main():
    verify = "--verify" in sys.argv
    base = zipfile.ZipFile(PRISTINE, "r").read(LIB_ENTRY)
    print("pristine libwmw md5 = %s (%d bytes)" % (md5(base), len(base)))
    if md5(base) != PRISTINE_LIB_MD5:
        raise SystemExit("ERROR: not the expected Where's My Water? 1.0.2 libwmw.so")
    patched = apply_patches(base, PATCHES, verify_only=verify)
    if verify:
        print("verify OK"); return
    open(PATCHED_SO, "wb").write(patched)
    print("wrote %s md5=%s%s" % (PATCHED_SO, md5(patched),
          " (= shipped build)" if md5(patched) == SHIPPED_LIB_MD5 else " (differs from the shipped build)"))

    # repack: copy pristine apk, replace the libwmw entry
    if os.path.exists(OUT_APK): os.remove(OUT_APK)
    zin = zipfile.ZipFile(PRISTINE, "r")
    zout = zipfile.ZipFile(OUT_APK, "w")
    replaced = False
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == LIB_ENTRY:
            data = patched; replaced = True
            print("  replaced %s in apk" % LIB_ENTRY)
        # preserve original compression type per-entry
        zi = zipfile.ZipInfo(item.filename, date_time=item.date_time)
        zi.compress_type = item.compress_type
        zi.external_attr = item.external_attr
        zout.writestr(zi, data)
    zin.close(); zout.close()
    if not replaced:
        raise SystemExit("ERROR: %s not found in pristine apk" % LIB_ENTRY)
    print("wrote %s" % OUT_APK)

if __name__ == "__main__":
    main()
