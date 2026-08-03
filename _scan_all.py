#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
第二轮全量扫描: 逐字形比较"内嵌位图"与"该字自身的轮廓形状"。
被画成别的字的位图, 其与自身轮廓的相似度(IoU)会明显低于同字号的正常水平。
可捕获"没有与任何其它字形完全重复"的错字。
输出 _scan_all_report.txt
"""
import sys, io, time
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
import numpy as np
from PIL import ImageFont
from _ebdt_raw import EBDTFont, render
from _outline_ref import ensure_nobitmap, NOBM

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/_scan_all_report.txt"
ensure_nobitmap(SRC)

t0 = time.time()
ef = EBDTFont(SRC)
gi2cp = ef.cmap_gi()
ef._ensure_loca()

CJK = lambda cp: (0x4E00 <= cp <= 0x9FFF) or (0x3400 <= cp <= 0x4DBF)
targets = []
for g, cps in gi2cp.items():
    c = [cp for cp in cps if CJK(cp)]
    if c:
        targets.append((g, min(c)))
targets.sort()
print("待扫描汉字字形 %d 个 x %d 字号" % (len(targets), len(ef.strikes)), flush=True)

PAD = 6

def to_canvas(pix, w, h, bx, by, S, ppem):
    a = np.zeros((S, S), dtype=bool)
    for (x, y) in pix:
        cx = PAD + bx + x
        cy = ppem - by + y + PAD
        if 0 <= cx < S and 0 <= cy < S:
            a[cy, cx] = True
    return a

def mask_to_pix(m, thresh=110):
    w, h = m.size
    d = np.frombuffer(bytes(m), dtype=np.uint8).reshape(h, w)
    return d >= thresh

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
            inter = np.count_nonzero(A & C)
            uni = np.count_nonzero(A | C)
            if uni:
                s = inter / uni
                if s > best:
                    best = s
    return best

results = []      # (iou, ppem, si, g, cp)
for st in ef.strikes:
    si, ppem = st['idx'], st['ppem']
    S = ppem + 2 * PAD + 4
    font = ImageFont.truetype(NOBM, ppem)
    ascent, _ = font.getmetrics()
    n = 0
    tsi = time.time()
    for g, cp in targets:
        r = ef.read_bitmap(si, g)
        if r is None or not r['pixels']:
            continue
        ch = chr(cp)
        try:
            m = font.getmask(ch, mode='L')
        except Exception:
            continue
        if m.size[0] == 0 or m.size[1] == 0:
            continue
        bbox = font.getbbox(ch)
        rbx = bbox[0]; rby = ascent - bbox[1]
        rp = mask_to_pix(m)
        rh, rw = rp.shape
        B = np.zeros((S, S), dtype=bool)
        y0 = ppem - rby + PAD; x0 = PAD + rbx
        ys, xs = np.nonzero(rp)
        ys = ys + y0; xs = xs + x0
        ok = (ys >= 0) & (ys < S) & (xs >= 0) & (xs < S)
        B[ys[ok], xs[ok]] = True
        mt = r['metrics']
        A = to_canvas(r['pixels'], r['w'], r['h'], mt['bx'], mt['by'], S, ppem)
        s = iou_shift(A, B, slack=1)
        results.append((s, ppem, si, g, cp))
        n += 1
    print("strike%-2d ppem=%-3d 扫描 %d 字  %.1fs (累计 %.1fs)"
          % (si, ppem, n, time.time() - tsi, time.time() - t0), flush=True)

print("总样本 %d" % len(results), flush=True)

# 按 ppem 统计基线, 用相对偏离找异常
import statistics
byppem = {}
for s, ppem, si, g, cp in results:
    byppem.setdefault(ppem, []).append(s)

stat = {}
for ppem, v in byppem.items():
    v2 = sorted(v)
    med = v2[len(v2) // 2]
    p05 = v2[max(0, int(len(v2) * 0.005))]
    stat[ppem] = (med, p05, len(v2))
    print("ppem=%-3d n=%-6d 中位IoU=%.3f  0.5%%分位=%.3f" % (ppem, len(v2), med, p05), flush=True)

flag = []
for s, ppem, si, g, cp in results:
    med, p05, n = stat[ppem]
    if s < p05:
        flag.append((s - med, s, ppem, si, g, cp))
flag.sort()
print("异常候选 %d" % len(flag), flush=True)

ref_font = {}
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write("第二轮扫描: 内嵌位图 与 自身轮廓 相似度异常低的字形\n")
    fh.write("(左=字体里的内嵌位图 右=该字真正的轮廓形状; 两者差别巨大 = 位图画错)\n")
    for ppem in sorted(stat):
        med, p05, n = stat[ppem]
        fh.write("  ppem=%-3d 样本%-6d 中位IoU=%.3f 判定阈值=%.3f\n" % (ppem, n, med, p05))
    fh.write("=" * 78 + "\n\n")
    for dev, s, ppem, si, g, cp in flag[:400]:
        r = ef.read_bitmap(si, g)
        ch = chr(cp)
        f = ref_font.get(ppem)
        if f is None:
            f = ImageFont.truetype(NOBM, ppem); ref_font[ppem] = f
        m = f.getmask(ch, mode='L')
        rp = mask_to_pix(m)
        rh, rw = rp.shape
        left = render(r['pixels'], r['w'], r['h']).split('\n')
        right = ['' .join('#' if rp[y, x] else '.' for x in range(rw)) for y in range(rh)]
        fh.write("U+%04X %s  ppem=%d(strike%d)  IoU=%.3f (中位%.3f)  gi=%d\n"
                 % (cp, ch, ppem, si, s, stat[ppem][0], g))
        H = max(len(left), len(right))
        wl = max((len(x) for x in left), default=0)
        for i in range(H):
            l = left[i] if i < len(left) else ''
            rr = right[i] if i < len(right) else ''
            fh.write("    %-*s   |   %s\n" % (wl, l, rr))
        fh.write("\n")

print("报告已写 %s  总耗时 %.1fs" % (OUT, time.time() - t0))
