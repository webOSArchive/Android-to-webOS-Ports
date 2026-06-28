#!/usr/bin/env python3
"""
derbh_extract.py - unpack Marmalade Derbh ".dz" (DTRZ-magic) archives.

WHY: NDK games that pack resources in a Derbh archive (e.g. PvZ HD = assets/PvZ.dz)
can't reliably stream them out through the apkenv shim at runtime (see
android-port-shim.md §8). Do a ONE-TIME host-side unpack with this, push the tree
to the device, and let the engine read real files.

Format (DTRZ): 4s magic 'DTRZ' | u16 file_count | u16 version | 1 pad byte |
  file_count null-term names | null-term dir names (until a valid ref table) |
  ref table[file_count] = (u16 dir_idx, u16 name_idx, u16 parent_idx) |
  u16 marker_a, u16 marker_b(==file_count) | meta[file_count]=(u32 off,u32 usize,
  u32 usize2,u32 flags). Payload per entry is LZMA-Alone, raw zlib, or stored.
  NB: PvZ.dz's payload offsets are NOT sorted -> compressed size must be derived
  from offsets sorted independently (not index order). Credit: format mapped with
  help from github.com/codefl0w/MarmDump (LZMA-only); this adds zlib+store+unsorted.

Usage:  python3 derbh_extract.py <archive.dz> <output_dir>
"""
import struct, zlib, lzma, sys, bisect, json
from pathlib import Path

def read_cstr(d, pos):
    e = d.index(0, pos); return d[pos:e].decode('latin-1'), e+1

def parse(d):
    assert d[:4]==b'DTRZ', "bad magic"
    fc = struct.unpack_from('<H', d, 4)[0]
    ver = struct.unpack_from('<H', d, 6)[0]
    pos = 9
    names=[]
    for _ in range(fc):
        nm,pos = read_cstr(d,pos); names.append(nm)
    dirs=[]
    while True:
        dn,pos = read_cstr(d,pos); dirs.append(dn)
        rtp=pos
        if rtp+fc*6>len(d): continue
        ok=True
        for i in range(min(fc,64)):
            dr,ni,pa = struct.unpack_from('<HHH', d, rtp+i*6)
            if ni!=i or dr>len(dirs) or (pa!=0xFFFF and pa>=fc): ok=False;break
        if ok: break
    ref=pos; pos+=fc*6
    ma,mb = struct.unpack_from('<HH', d, pos); pos+=4
    assert mb==fc, f"meta count {mb}!={fc}"
    meta=[struct.unpack_from('<IIII', d, pos+i*16) for i in range(fc)]
    return fc,ver,names,dirs,ref,meta

def main(dz, outdir):
    d=open(dz,'rb').read()
    fc,ver,names,dirs,ref,meta = parse(d)
    print(f"DTRZ v{ver}: {fc} files, {len(dirs)} dirs")
    offs=sorted(set(m[0] for m in meta)|{len(d)})
    def csz(off): return offs[bisect.bisect_right(offs,off)]-off
    stats={'zlib':0,'lzma':0,'store':0,'fail':0}
    manifest=[]
    for i in range(fc):
        off,usize,usize2,flags = meta[i]
        dr,ni,pa = struct.unpack_from('<HHH', d, ref+i*6)
        directory = dirs[dr-1] if dr>0 else ''
        name = names[ni]
        rel = (directory+'\\'+name) if directory else name
        rel = rel.replace('\\','/').lstrip('/')
        blob = d[off:off+csz(off)]
        out=None; method='fail'
        if blob[:1]==b'\x78':
            try:
                o=zlib.decompressobj(); out=o.decompress(blob); out+=o.flush(); method='zlib'
            except Exception: out=None
        if out is None:
            try:
                out=lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(blob); method='lzma'
            except Exception: out=None
        if out is None:
            out = blob[:usize] if usize else blob; method='store'
        stats[method]+=1
        p=Path(outdir)/rel; p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(out)
        manifest.append({'path':rel,'method':method,'usize':usize,'got':len(out),'off':off})
    print("methods:",stats)
    # validation: check magics
    from collections import Counter
    mg=Counter()
    for m in manifest:
        ext=m['path'].rsplit('.',1)[-1].lower() if '.' in m['path'] else '(none)'
        mg[ext]+=1
    print("extensions:",dict(mg.most_common(10)))
    json.dump(manifest, open(Path(outdir)/'_manifest.json','w'))
    return manifest

if __name__=='__main__':
    main(sys.argv[1], sys.argv[2])
