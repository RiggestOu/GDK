#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
edit_bitmap.py —— SourceHanSans 内嵌位图(EBLC/EBDT)像素级手工调整工具

用法:
  # 1) 把某个字的全部 strike 位图导出成可编辑文本网格 + PNG 预览
  python edit_bitmap.py dump  <字符或码点>  <字体ttf>  <输出目录>

  # 2) 你手工修改 <目录>/<字>_ppem<X>.txt 里的 '#'(墨)/'.'(空) 后，写回字体
  python edit_bitmap.py restore  <字符或码点>  <源字体ttf>  <编辑目录>  <输出字体ttf>

说明:
  - 文本网格第一行是注释头(含 w/h/bearingX/bearingY/advance)，其余每行 w 个字符。
  - 只改网格里的 '#' / '.' 即可；尺寸/度量不变时最安全(直接重写 data 字节)。
  - 若你改变了字形外框(宽度/高度/偏移)，工具会同步更新 metrics，fontTools 存盘时
    会自动重算 EBDT/EBLC 偏移。
"""
import sys, os, struct
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw

def parse_char(s):
    if s.lower().startswith("u+") or s.lower().startswith("0x"):
        return int(s, 16)
    if s.isdigit():
        return int(s)
    return ord(s[0])

def emb_ink(bgd):
    m = bgd.metrics
    w, h = m.width, m.height
    rows = []
    for y in range(h):
        row = bgd.getRow(y)
        bits = ""
        for x in range(w):
            bits += "#" if ((row[x >> 3] >> (7 - (x & 7))) & 1) else "."
        rows.append(bits)
    return rows, (m.width, m.height, m.horiBearingX, m.horiBearingY, m.horiAdvance)

def pack_rows(rows, w, h):
    rowbytes = (w + 7) // 8
    data = bytearray()
    for r in rows:
        val = 0
        for ch in r:
            val = (val << 1) | (1 if ch == "#" else 0)
        # 左对齐到 rowbytes
        val <<= (rowbytes * 8 - w)
        data += val.to_bytes(rowbytes, "big")
    return bytes(data)

def dump(chcp, ttf, outdir):
    f = TTFont(ttf)
    cmap = f.getBestCmap()
    name = cmap.get(chcp)
    if name is None:
        print("该字符不在 cmap 中"); return
    sd = f['EBDT'].strikeData
    eblc = f['EBLC']
    os.makedirs(outdir, exist_ok=True)
    char = chr(chcp)
    dumped = 0
    for si, strike in enumerate(eblc.strikes):
        sdi = sd[si]
        if name not in sdi: continue
        ppem = strike.bitmapSizeTable.ppemX
        rows, (w, h, bx, by, adv) = emb_ink(sdi[name])
        base = f"{outdir}/{char}_ppem{ppem}"
        # 文本网格
        with open(base + ".txt", "w", encoding="utf-8") as fh:
            fh.write(f"# glyph={char} U+{chcp:04X} strike={si} ppem={ppem} w={w} h={h} bearingX={bx} bearingY={by} advance={adv}\n")
            fh.write("\n".join(rows) + "\n")
        # PNG 预览 (放大 24 倍)
        S = 24
        img = Image.new("L", (w*S, h*S), 255)
        d = ImageDraw.Draw(img)
        for y in range(h):
            for x in range(w):
                if rows[y][x] == "#":
                    d.rectangle([x*S, y*S, x*S+S-1, y*S+S-1], fill=0)
        img.save(base + ".png")
        dumped += 1
        print(f"  dump strike[{si}] ppem={ppem} -> {base}.txt / .png")
    print(f"共导出 {dumped} 个 strike 的位图到 {outdir}/")

def restore(chcp, ttf, editdir, outttf):
    f = TTFont(ttf)
    cmap = f.getBestCmap()
    name = cmap.get(chcp)
    if name is None:
        print("该字符不在 cmap 中"); return
    sd = f['EBDT'].strikeData
    eblc = f['EBLC']
    char = chr(chcp)
    changed = 0
    for si, strike in enumerate(eblc.strikes):
        sdi = sd[si]
        if name not in sdi: continue
        ppem = strike.bitmapSizeTable.ppemX
        base = f"{editdir}/{char}_ppem{ppem}"
        txt = base + ".txt"
        if not os.path.exists(txt): continue
        with open(txt, encoding="utf-8") as fh:
            lines = [l.rstrip("\n") for l in fh if not l.startswith("#")]
        lines = [l for l in lines if l.strip() != ""]
        w = len(lines[0]); h = len(lines)
        data = pack_rows(lines, w, h)
        bgd = sdi[name]
        bgd.data = data
        bgd.metrics.width = w
        bgd.metrics.height = h
        changed += 1
        print(f"  restore strike[{si}] ppem={ppem}: 写回 {w}x{h} 位图")
    f.save(outttf)
    print(f"已保存修正字体: {outttf} (改了 {changed} 个 strike)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "dump":
        chcp = parse_char(sys.argv[2])
        ttf = sys.argv[3]
        outdir = sys.argv[4] if len(sys.argv) > 4 else "_bmp_edit"
        dump(chcp, ttf, outdir)
    elif cmd == "restore":
        chcp = parse_char(sys.argv[2])
        ttf = sys.argv[3]
        editdir = sys.argv[4]
        outttf = sys.argv[5]
        restore(chcp, ttf, editdir, outttf)
    else:
        print(__doc__)
