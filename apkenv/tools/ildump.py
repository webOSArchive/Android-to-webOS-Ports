#!/usr/bin/env python3
"""Resolve .NET metadata tokens to names in a managed assembly.

Why this exists: Mono's MONO_VERBOSE_METHOD dump prints call targets as raw
metadata tokens (`call 0x0600037f`), which is exactly the information you need
when tracking down "the game never calls X" in a Unity port - but useless until
the token has a name. There is no monodis/ikdasm in this environment, so this
parses the ECMA-335 tables directly.

    tools/ildump.py <assembly.dll>                 list MethodDef tokens
    tools/ildump.py <assembly.dll> 0x0600037f ...  resolve specific tokens
"""
import struct
import sys


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


class Meta:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self._load()

    def _rva2off(self, rva):
        for off, va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + (rva - va)
        raise ValueError('rva 0x%x not in any section' % rva)

    def _load(self):
        d = self.d
        pe = u32(d, 0x3c)
        nsec = u16(d, pe + 6)
        optsize = u16(d, pe + 20)
        opt = pe + 24
        magic = u16(d, opt)
        # data directory 14 = CLI header; 96/112 bytes of standard fields before it
        dd = opt + (96 if magic == 0x10b else 112)
        cli_rva = u32(d, dd + 14 * 8)
        sect = opt + optsize
        self.sections = []
        for i in range(nsec):
            s = sect + i * 40
            self.sections.append((s, u32(d, s + 12), u32(d, s + 8), u32(d, s + 20)))
        cli = self._rva2off(cli_rva)
        md = self._rva2off(u32(d, cli + 8))

        assert d[md:md + 4] == b'BSJB', 'not a metadata root'
        vlen = u32(d, md + 12)
        p = md + 16 + vlen + 2
        nstreams = u16(d, p)
        p += 2
        self.streams = {}
        for _ in range(nstreams):
            off, size = u32(d, p), u32(d, p + 4)
            p += 8
            name = b''
            while d[p] != 0:
                name += bytes([d[p]])
                p += 1
            p += 1
            p = (p + 3) & ~3
            self.streams[name.decode()] = (md + off, size)

        self.strings = self.streams.get('#Strings')
        tbl = self.streams.get('#~') or self.streams['#-']
        t = tbl[0]
        heap = d[t + 6]
        self.s_idx = 4 if heap & 1 else 2
        self.g_idx = 4 if heap & 2 else 2
        self.b_idx = 4 if heap & 4 else 2
        valid = struct.unpack_from('<Q', d, t + 8)[0]
        p = t + 24
        self.rows = {}
        for i in range(64):
            if valid & (1 << i):
                self.rows[i] = u32(d, p)
                p += 4
        self.tables_start = p

    def sstr(self, idx):
        base = self.strings[0] + idx
        e = self.d.index(b'\0', base)
        return self.d[base:e].decode('utf-8', 'replace')

    def simple(self, table):
        return 4 if self.rows.get(table, 0) >= 65536 else 2

    def coded(self, tables, bits):
        m = max((self.rows.get(t, 0) for t in tables), default=0)
        return 4 if m >= (1 << (16 - bits)) else 2

    def row_size(self, t):
        S, B, G = self.s_idx, self.b_idx, self.g_idx
        typedeforref = self.coded([2, 1, 27], 2)
        resolutionscope = self.coded([0, 26, 35, 1], 2)
        if t == 0x00:   return 2 + S + 3 * G
        if t == 0x01:   return resolutionscope + 2 * S
        if t == 0x02:   return 4 + 2 * S + typedeforref + self.simple(4) + self.simple(6)
        if t == 0x03:   return self.simple(4)
        if t == 0x04:   return 2 + S + B
        if t == 0x05:   return self.simple(6)
        if t == 0x06:   return 4 + 2 + 2 + S + B + self.simple(8)
        raise ValueError('row size for table 0x%02x not implemented' % t)

    def table_offset(self, t):
        off = self.tables_start
        for i in sorted(self.rows):
            if i == t:
                return off
            off += self.rows[i] * self.row_size(i)
        raise ValueError('table 0x%02x absent' % t)

    def method_name(self, row):
        """row is 1-based, as in a 0x06xxxxxx token."""
        base = self.table_offset(0x06) + (row - 1) * self.row_size(0x06)
        o = base + 8                      # RVA(4) + ImplFlags(2) + Flags(2)
        idx = u32(self.d, o) if self.s_idx == 4 else u16(self.d, o)
        return self.sstr(idx)

    def owner_of_method(self, row):
        """Walk TypeDef's MethodList to find which type owns this method row."""
        td, rs = self.table_offset(0x02), self.row_size(0x02)
        n = self.rows.get(0x02, 0)
        mlist_off = rs - self.simple(6)
        name_off = 4
        best = None
        for i in range(n):
            b = td + i * rs
            first = (u32(self.d, b + mlist_off) if self.simple(6) == 4
                     else u16(self.d, b + mlist_off))
            if first <= row:
                idx = (u32(self.d, b + name_off) if self.s_idx == 4
                       else u16(self.d, b + name_off))
                best = (first, self.sstr(idx))
            else:
                break
        return best[1] if best else '?'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    m = Meta(sys.argv[1])
    if len(sys.argv) == 2:
        for r in range(1, m.rows.get(0x06, 0) + 1):
            print('0x06%06x  %s:%s' % (r, m.owner_of_method(r), m.method_name(r)))
        return 0
    for a in sys.argv[2:]:
        tok = int(a, 0)
        t, row = tok >> 24, tok & 0xFFFFFF
        if t == 0x06:
            print('%s -> %s:%s' % (a, m.owner_of_method(row), m.method_name(row)))
        else:
            print('%s -> table 0x%02x row %d (only MethodDef is resolved)' % (a, t, row))
    return 0


if __name__ == '__main__':
    sys.exit(main())
