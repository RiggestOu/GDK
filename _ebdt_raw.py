#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
_ebdt_raw.py —— 手写解析/改写 OpenType EBLC/EBDT，绕过 fontTools 在部分 strike 解压失败的 bug。
用途: 小字号内嵌位图(被画错的字)的读取与像素级改写。

严格按 OpenType 规范实现:
  IndexSubTable1: header(8) + uint32 offsetArray[n+1]                 变尺寸
  IndexSubTable2: header(8) + uint32 imageSize + BigGlyphMetrics(8)   常量尺寸, 无 offset 数组
  IndexSubTable3: header(8) + uint16 offsetArray[n+1]                 变尺寸
  IndexSubTable4: header(8) + uint32 numGlyphs + (u16 gid,u16 off)[]  稀疏变尺寸
  IndexSubTable5: header(8) + uint32 imageSize + BigGlyphMetrics(8)
                            + uint32 numGlyphs + uint16 glyphIdArray[] 稀疏常量尺寸

  imageFormat 1: SmallGlyphMetrics(5) + byte-aligned bitmap
  imageFormat 2: SmallGlyphMetrics(5) + bit-aligned  bitmap
  imageFormat 5: bit-aligned bitmap only (度量来自 EBLC 的 bigMetrics)
  imageFormat 6: BigGlyphMetrics(8)   + byte-aligned bitmap
  imageFormat 7: BigGlyphMetrics(8)   + bit-aligned  bitmap

本字体实测: strike 0,1,5,6,7,8,9,16 = ifmt1+imgfmt7 ; strike 2,3,4,10..15 = ifmt2+imgfmt5

API:
  ef = EBDTFont(path)
  ef.ppems                       -> [ppem, ...]  17 个 strike 的字号
  ef.glyph_index(name)           -> int
  ef.read_bitmap(si, g)          -> dict{imgfmt,w,h,metrics,pixels,bitoff,aligned} 或 None
  ef.write_bitmap(si, g, pixels) -> 原地覆盖位图位, 尺寸不变
  ef.save(outpath)
"""
import struct
from bisect import bisect_right

_SMALL = 5
_BIG = 8


class EBDTFont:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as fh:
            self.raw = bytearray(fh.read())
        self._parse_sfnt()
        self._parse_eblc()
        self._gi_cache = None
        self._name_cache = None

    # ---------- 基础读取 ----------
    def _u16(self, o): return (self.raw[o] << 8) | self.raw[o + 1]
    def _u32(self, o): return struct.unpack_from('>I', self.raw, o)[0]
    def _i8(self, o): return struct.unpack_from('>b', self.raw, o)[0]
    def _u8(self, o): return self.raw[o]

    def _parse_sfnt(self):
        r = self.raw
        self.base = self._u32(12) if r[:4] == b'ttcf' else 0
        numTables = self._u16(self.base + 4)
        d = self.base + 12
        self.tables = {}
        for i in range(numTables):
            p = d + i * 16
            tag = bytes(r[p:p + 4]).decode('latin1')
            self.tables[tag] = (self._u32(p + 8), self._u32(p + 12))
        self.eblc_off = self.tables['EBLC'][0]
        self.ebdt_off = self.tables['EBDT'][0]

    def _parse_eblc(self):
        o = self.eblc_off
        self.ver = self._u32(o)
        numSizes = self._u32(o + 4)
        bsize = 48
        self.strikes = []
        for i in range(numSizes):
            bs = o + 8 + i * bsize
            idxArrOff = self._u32(bs)
            nIdx = self._u32(bs + 8)
            startG = self._u16(bs + 40)
            endG = self._u16(bs + 42)
            ppemX = self.raw[bs + 44]
            arr = o + idxArrOff
            subs = []
            for k in range(nIdx):
                a = arr + k * 8
                first = self._u16(a)
                last = self._u16(a + 2)
                addl = self._u32(a + 4)
                idxOff = arr + addl
                ifmt = self._u16(idxOff)
                imgfmt = self._u16(idxOff + 2)
                ido = self._u32(idxOff + 4)
                sub = {'first': first, 'last': last, 'idxOff': idxOff,
                       'ifmt': ifmt, 'imgfmt': imgfmt, 'ido': ido,
                       'imageSize': None, 'big': None, 'glyphIds': None}
                if ifmt in (2, 5):
                    sub['imageSize'] = self._u32(idxOff + 8)
                    sub['big'] = self._read_big(idxOff + 12)
                    if ifmt == 5:
                        n = self._u32(idxOff + 20)
                        sub['nGlyphs'] = n
                        sub['gidArrOff'] = idxOff + 24
                subs.append(sub)
            subs.sort(key=lambda s: s['first'])
            self.strikes.append({'ppem': ppemX, 'start': startG, 'end': endG,
                                 'subs': subs, 'idx': i,
                                 'firsts': [s['first'] for s in subs]})

    # ---------- glyf 轮廓原始字节(用于判断"是否本来就是同一个字形") ----------
    def _ensure_loca(self):
        if hasattr(self, '_loca'):
            return
        head = self.tables['head'][0]
        maxp = self.tables['maxp'][0]
        self.numGlyphs = self._u16(maxp + 4)
        fmt = struct.unpack_from('>h', self.raw, head + 50)[0]
        lo = self.tables['loca'][0]
        n = self.numGlyphs + 1
        if fmt == 0:
            self._loca = [self._u16(lo + i * 2) * 2 for i in range(n)]
        else:
            self._loca = [self._u32(lo + i * 4) for i in range(n)]
        self._glyf = self.tables['glyf'][0]

    def outline_bytes(self, g):
        """该字形的 glyf 原始字节 (空字形返回 b'')"""
        self._ensure_loca()
        if g + 1 >= len(self._loca):
            return b''
        a, b = self._loca[g], self._loca[g + 1]
        if b <= a:
            return b''
        return bytes(self.raw[self._glyf + a:self._glyf + b])

    def _read_big(self, o):
        return {'h': self._u8(o), 'w': self._u8(o + 1),
                'bx': self._i8(o + 2), 'by': self._i8(o + 3),
                'adv': self._u8(o + 4),
                'vbx': self._i8(o + 5), 'vby': self._i8(o + 6),
                'vadv': self._u8(o + 7)}

    # ---------- 字形名/索引 ----------
    @property
    def ppems(self):
        return [st['ppem'] for st in self.strikes]

    def _ensure_gi(self):
        if self._gi_cache is None:
            from fontTools.ttLib import TTFont
            ff = TTFont(self.path, lazy=True)
            order = ff.getGlyphOrder()
            self._gi_cache = {nm: i for i, nm in enumerate(order)}
            self._name_cache = list(order)
            try:
                self.cmap = ff.getBestCmap()
            except Exception:
                self.cmap = {}
            ff.close()

    def glyph_index(self, name):
        self._ensure_gi()
        return self._gi_cache.get(name)

    def glyph_name(self, gi):
        self._ensure_gi()
        return self._name_cache[gi] if 0 <= gi < len(self._name_cache) else None

    def cmap_gi(self):
        """返回 {glyphIndex: [codepoint,...]}"""
        self._ensure_gi()
        m = {}
        for cp, nm in self.cmap.items():
            gi = self._gi_cache.get(nm)
            if gi is not None:
                m.setdefault(gi, []).append(cp)
        return m

    # ---------- 定位 ----------
    def _locate(self, si, g):
        """返回 (sub, dataOff, imageSize|None) —— dataOff 是相对 EBDT 表首的偏移"""
        st = self.strikes[si]
        k = bisect_right(st['firsts'], g) - 1
        if k < 0:
            return None
        for sub in st['subs'][k:k + 1]:
            if not (sub['first'] <= g <= sub['last']):
                continue
            ifmt = sub['ifmt']
            if ifmt == 1:
                e = sub['idxOff'] + 8 + (g - sub['first']) * 4
                off = self._u32(e)
                nxt = self._u32(e + 4)
                if nxt == off:      # 空位图
                    return None
                return (sub, sub['ido'] + off, nxt - off)
            if ifmt == 3:
                e = sub['idxOff'] + 8 + (g - sub['first']) * 2
                off = self._u16(e)
                nxt = self._u16(e + 2)
                if nxt == off:
                    return None
                return (sub, sub['ido'] + off, nxt - off)
            if ifmt == 2:
                isz = sub['imageSize']
                return (sub, sub['ido'] + (g - sub['first']) * isz, isz)
            if ifmt == 4:
                n = self._u32(sub['idxOff'] + 8)
                p = sub['idxOff'] + 12
                for k in range(n):
                    gid = self._u16(p + k * 4)
                    if gid == g:
                        off = self._u16(p + k * 4 + 2)
                        nxt = self._u16(p + (k + 1) * 4 + 2)
                        if nxt == off:
                            return None
                        return (sub, sub['ido'] + off, nxt - off)
                return None
            if ifmt == 5:
                n = sub['nGlyphs']
                lo, hi = 0, n - 1
                base = sub['gidArrOff']
                while lo <= hi:
                    mid = (lo + hi) // 2
                    gid = self._u16(base + mid * 2)
                    if gid == g:
                        isz = sub['imageSize']
                        return (sub, sub['ido'] + mid * isz, isz)
                    if gid < g:
                        lo = mid + 1
                    else:
                        hi = mid - 1
                return None
            return None
        return None

    # ---------- 读位图 ----------
    def read_bitmap(self, si, g):
        rec = self._locate(si, g)
        if rec is None:
            return None
        sub, doff, isz = rec
        imgfmt = sub['imgfmt']
        base = self.ebdt_off + doff
        if imgfmt in (1, 2):
            h = self._u8(base); w = self._u8(base + 1)
            m = {'h': h, 'w': w, 'bx': self._i8(base + 2),
                 'by': self._i8(base + 3), 'adv': self._u8(base + 4)}
            bmoff = base + _SMALL
            aligned = (imgfmt == 1)
        elif imgfmt in (6, 7):
            m = self._read_big(base)
            h, w = m['h'], m['w']
            bmoff = base + _BIG
            aligned = (imgfmt == 6)
        elif imgfmt == 5:
            m = dict(sub['big'])
            h, w = m['h'], m['w']
            bmoff = base
            aligned = False
        else:
            return None

        pixels = set()
        if w and h:
            if aligned:
                rb = (w + 7) // 8
                for y in range(h):
                    ro = bmoff + y * rb
                    for x in range(w):
                        if (self.raw[ro + (x >> 3)] >> (7 - (x & 7))) & 1:
                            pixels.add((x, y))
            else:
                for y in range(h):
                    b0 = y * w
                    for x in range(w):
                        bi = b0 + x
                        if (self.raw[bmoff + (bi >> 3)] >> (7 - (bi & 7))) & 1:
                            pixels.add((x, y))
        return {'imgfmt': imgfmt, 'w': w, 'h': h, 'metrics': m,
                'pixels': pixels, 'bmoff': bmoff, 'aligned': aligned,
                'imageSize': isz, 'ppem': self.strikes[si]['ppem']}

    def bitmap_key(self, si, g):
        """快速返回 (w,h,bytes) 用于分组比较, 不展开像素"""
        rec = self._locate(si, g)
        if rec is None:
            return None
        sub, doff, isz = rec
        imgfmt = sub['imgfmt']
        base = self.ebdt_off + doff
        if imgfmt in (1, 2):
            h = self._u8(base); w = self._u8(base + 1)
            bmoff = base + _SMALL
            aligned = (imgfmt == 1)
        elif imgfmt in (6, 7):
            h = self._u8(base); w = self._u8(base + 1)
            bmoff = base + _BIG
            aligned = (imgfmt == 6)
        elif imgfmt == 5:
            h = sub['big']['h']; w = sub['big']['w']
            bmoff = base
            aligned = False
        else:
            return None
        if not w or not h:
            return (w, h, b'')
        nb = ((w + 7) // 8) * h if aligned else (w * h + 7) // 8
        return (w, h, bytes(self.raw[bmoff:bmoff + nb]))

    # ---------- 写位图 ----------
    def write_bitmap(self, si, g, pixels):
        r = self.read_bitmap(si, g)
        if r is None:
            raise ValueError("该 strike 无此字形位图")
        w, h, bmoff, aligned = r['w'], r['h'], r['bmoff'], r['aligned']
        if aligned:
            rb = (w + 7) // 8
            nb = rb * h
            buf = bytearray(nb)
            for (x, y) in pixels:
                if 0 <= x < w and 0 <= y < h:
                    buf[y * rb + (x >> 3)] |= (0x80 >> (x & 7))
        else:
            nb = (w * h + 7) // 8
            buf = bytearray(nb)
            for (x, y) in pixels:
                if 0 <= x < w and 0 <= y < h:
                    bi = y * w + x
                    buf[bi >> 3] |= (0x80 >> (bi & 7))
        self.raw[bmoff:bmoff + nb] = buf
        return nb

    def save(self, outpath):
        with open(outpath, 'wb') as fh:
            fh.write(self.raw)


def render(pix, w, h, on='#', off='.'):
    return '\n'.join(''.join(on if (x, y) in pix else off for x in range(w))
                     for y in range(h))


if __name__ == "__main__":
    SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
    ef = EBDTFont(SRC)
    print("ver=0x%X  strikes=%d  ppems=%s" % (ef.ver, len(ef.strikes), ef.ppems))
    for st in ef.strikes:
        s0 = st['subs'][0]
        print("  strike%-2d ppem=%-3d nsub=%-3d ifmt=%d imgfmt=%d imageSize=%s big=%s"
              % (st['idx'], st['ppem'], len(st['subs']), s0['ifmt'], s0['imgfmt'],
                 s0['imageSize'], s0['big']))
    gi = ef.glyph_index('uni9631')
    print("\n阱 gi =", gi)
    for si in range(len(ef.strikes)):
        r = ef.read_bitmap(si, gi)
        if r:
            print("  strike%-2d ppem=%-3d %dx%d ink=%d imgfmt=%d aligned=%s"
                  % (si, r['ppem'], r['w'], r['h'], len(r['pixels']),
                     r['imgfmt'], r['aligned']))
