#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
常用字重点复查: 只看 GB2312 一级+二级汉字(6763 字), 阅读常用字号 ppem 10~18,
列出"内嵌位图 与 该字轮廓"相似度最低的若干条, 供人工过目确认有无漏网错字。
输出 _common_report.txt
"""
import sys, io, time
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
import numpy as np
from PIL import ImageFont
from _ebdt_raw import EBDTFont, render
from _outline_ref import ensure_nobitmap, NOBM

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/_common_report.txt"
ensure_nobitmap(SRC)

# GB2312 汉字集
common = []
for q in range(16, 88):
    for w in range(1, 95):
        try:
            ch = bytes([0xA0 + q, 0xA0 + w]).decode('gb2312')
        except Exception:
            continue
        if len(ch) == 1 and '\u4e00' <= ch <= '\u9fff':
            common.append(ch)
lvl1 = set(common[:3755])
print("GB2312 汉字 %d (一级 %d)" % (len(common), len(lvl1)), flush=True)

ef = EBDTFont(SRC)
gi2cp = ef.cmap_gi()
cp2gi = {}
for g, cps in gi2cp.items():
    for cp in cps:
        cp2gi.setdefault(cp, g)

PAD = 6
def mask_pix(m, th=110):
    w, h = m.size
    return np.frombuffer(bytes(m), dtype=np.uint8).reshape(h, w) >= th

def iou_shift(A, B, slack=1):
    best = 0.0
    for dy in range(-slack, slack + 1):
        Bs = np.roll(B, dy, axis=0)
        if dy > 0: Bs[:dy, :] = False
        elif dy < 0: Bs[dy:, :] = False
        for dx in range(-slack, slack + 1):
            C = np.roll(Bs, dx, axis=1)
            if dx > 0: C[:, :dx] = False
            elif dx < 0: C[:, dx:] = False
            u = np.count_nonzero(A | C)
            if u:
                s = np.count_nonzero(A & C) / u
                if s > best: best = s
    return best

res = []
t0 = time.time()
for st in ef.strikes:
    si, ppem = st['idx'], st['ppem']
    if not (10 <= ppem <= 18):
        continue
    S = ppem + 2 * PAD + 4
    font = ImageFont.truetype(NOBM, ppem)
    ascent, _ = font.getmetrics()
    for ch in common:
        g = cp2gi.get(ord(ch))
        if g is None: continue
        r = ef.read_bitmap(si, g)
        if r is None or not r['pixels']: continue
        try:
            m = font.getmask(ch, mode='L')
            bbox = font.getbbox(ch)
        except Exception:
            continue
        if m.size[0] == 0: continue
        rp = mask_pix(m)
        B = np.zeros((S, S), dtype=bool)
        ys, xs = np.nonzero(rp)
        ys = ys + (ppem - (ascent - bbox[1]) + PAD); xs = xs + (PAD + bbox[0])
        ok = (ys >= 0) & (ys < S) & (xs >= 0) & (xs < S)
        B[ys[ok], xs[ok]] = True
        A = np.zeros((S, S), dtype=bool)
        mt = r['metrics']
        for (x, y) in r['pixels']:
            cx = PAD + mt['bx'] + x; cy = ppem - mt['by'] + y + PAD
            if 0 <= cx < S and 0 <= cy < S: A[cy, cx] = True
        res.append((iou_shift(A, B), ppem, si, g, ch))
    print("ppem=%-3d done %.1fs" % (ppem, time.time() - t0), flush=True)

byp = {}
for s, ppem, si, g, ch in res:
    byp.setdefault(ppem, []).append(s)
stat = {p: (float(np.median(v)), float(np.percentile(v, 0.5))) for p, v in byp.items()}
for p in sorted(stat):
    print("ppem=%-3d n=%-5d 中位=%.3f 0.5%%分位=%.3f" % (p, len(byp[p]), *stat[p]), flush=True)

flag = [(s - stat[p][0], s, p, si, g, ch) for s, p, si, g, ch in res if s < stat[p][1]]
flag.sort()
print("常用字异常候选 %d" % len(flag), flush=True)

fonts = {}
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write("常用字(GB2312)重点复查 — 内嵌位图 vs 该字真实轮廓, 相似度最低的条目\n")
    fh.write("人工看右边轮廓和左边位图是不是同一个字即可\n")
    fh.write("=" * 78 + "\n\n")
    for dev, s, ppem, si, g, ch in flag[:120]:
        r = ef.read_bitmap(si, g)
        f = fonts.get(ppem)
        if f is None:
            f = ImageFont.truetype(NOBM, ppem); fonts[ppem] = f
        rp = mask_pix(f.getmask(ch, mode='L'))
        rh, rw = rp.shape
        left = render(r['pixels'], r['w'], r['h']).split('\n')
        right = [''.join('#' if rp[y, x] else '.' for x in range(rw)) for y in range(rh)]
        wl = max((len(x) for x in left), default=0)
        fh.write("%s U+%04X  ppem=%d  IoU=%.3f (中位%.3f)  %s\n"
                 % (ch, ord(ch), ppem, s, stat[ppem][0],
                    "【一级常用】" if ch in lvl1 else ""))
        for i in range(max(len(left), len(right))):
            l = left[i] if i < len(left) else ''
            rt = right[i] if i < len(right) else ''
            fh.write("    %-*s   |   %s\n" % (wl, l, rt))
        fh.write("\n")
print("已写", OUT)
