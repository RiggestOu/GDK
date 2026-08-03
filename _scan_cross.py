#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
第三轮扫描(最灵敏): 跨字号一致性。
同一个汉字在 17 个字号的内嵌位图, 缩放归一化后应当彼此相似。
若某个字号的位图明显不合群 -> 那个字号八成被画成了别的字。
(阱: ppem12 画成了阡, 与其余 16 个字号都不像 -> 必被捕获)
输出 _cross_report.txt / _cross_list.json
"""
import sys, io, time, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
import numpy as np
from _ebdt_raw import EBDTFont, render

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/_cross_report.txt"
JS = "E:/WorkBuddy/GDKmini/GDK/_cross_list.json"
N = 14           # 归一化网格

t0 = time.time()
ef = EBDTFont(SRC)
gi2cp = ef.cmap_gi()
CJK = lambda cp: (0x4E00 <= cp <= 0x9FFF) or (0x3400 <= cp <= 0x4DBF)
targets = []
for g, cps in gi2cp.items():
    c = [cp for cp in cps if CJK(cp)]
    if c:
        targets.append((g, min(c)))
targets.sort()
print("汉字字形 %d" % len(targets), flush=True)

nS = len(ef.strikes)
ppems = ef.ppems


def fingerprint(pix, w, h):
    """按 ink 包围盒归一化到 NxN 覆盖率网格"""
    if not pix:
        return None
    xs = [p[0] for p in pix]; ys = [p[1] for p in pix]
    x0, x1 = min(xs), max(xs) + 1
    y0, y1 = min(ys), max(ys) + 1
    bw, bh = x1 - x0, y1 - y0
    if bw <= 0 or bh <= 0:
        return None
    acc = np.zeros((N, N), dtype=np.float32)
    cnt = np.zeros((N, N), dtype=np.float32)
    for (x, y) in pix:
        gx = int((x - x0) * N / bw)
        gy = int((y - y0) * N / bh)
        if gx >= N: gx = N - 1
        if gy >= N: gy = N - 1
        acc[gy, gx] += 1
    # 每个归一格覆盖的原始像素数
    px_per = max(1.0, (bw * bh) / (N * N))
    return (acc / px_per) > 0.30


def jac(a, b):
    i = np.count_nonzero(a & b)
    u = np.count_nonzero(a | b)
    return i / u if u else 1.0


rows = []
done = 0
for g, cp in targets:
    fps = []
    for si in range(nS):
        r = ef.read_bitmap(si, g)
        if r is None or not r['pixels'] or r['w'] < 6:
            fps.append(None); continue
        fps.append(fingerprint(r['pixels'], r['w'], r['h']))
    idx = [i for i, f in enumerate(fps) if f is not None]
    if len(idx) < 6:
        continue
    # 每个字号与其它字号的平均相似度
    sim = np.zeros(len(idx))
    for a in range(len(idx)):
        s = 0.0
        for b in range(len(idx)):
            if a == b: continue
            s += jac(fps[idx[a]], fps[idx[b]])
        sim[a] = s / (len(idx) - 1)
    med = float(np.median(sim))
    for a in range(len(idx)):
        rows.append((sim[a], med, idx[a], g, cp))
    done += 1
    if done % 3000 == 0:
        print("  %d/%d  %.1fs" % (done, len(targets), time.time() - t0), flush=True)

print("样本 %d  %.1fs" % (len(rows), time.time() - t0), flush=True)

# 按字号归一化: 每个 strike 自己的分布
bysi = {}
for sim, med, si, g, cp in rows:
    bysi.setdefault(si, []).append(sim)
stat = {}
for si, v in bysi.items():
    v2 = np.array(v)
    stat[si] = (float(np.median(v2)), float(np.percentile(v2, 0.3)))
    print("strike%-2d ppem=%-3d 中位一致度=%.3f  0.3%%分位=%.3f"
          % (si, ppems[si], stat[si][0], stat[si][1]), flush=True)

flag = []
for sim, med, si, g, cp in rows:
    m, p = stat[si]
    # 该字号明显不合群, 且该字其它字号彼此是合群的(med 正常)
    if sim < p and med - sim > 0.10:
        flag.append((sim - med, sim, med, si, g, cp))
flag.sort()
print("离群候选 %d" % len(flag), flush=True)

seen = set()
tasks = []
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write("跨字号一致性扫描 —— 某字号的位图与该字其它字号明显不合群\n")
    fh.write("(一致度 = 该字号位图 与 该字其它字号位图 归一化后的平均 Jaccard)\n")
    fh.write("=" * 78 + "\n\n")
    for dev, sim, med, si, g, cp in flag[:300]:
        r = ef.read_bitmap(si, g)
        if r is None: continue
        ch = chr(cp)
        fh.write("U+%04X %s   ppem=%d(strike%d)   一致度=%.3f  该字平均=%.3f  落差=%.3f  gi=%d\n"
                 % (cp, ch, ppems[si], si, sim, med, -dev, g))
        # 并排: 问题字号 vs 一个正常大字号
        ref_si = None
        for cand in (16, 12, 8, 10, 14):
            if cand != si:
                rr = ef.read_bitmap(cand, g)
                if rr and rr['pixels']:
                    ref_si = cand; break
        left = render(r['pixels'], r['w'], r['h']).split('\n')
        right = []
        if ref_si is not None:
            rr = ef.read_bitmap(ref_si, g)
            right = render(rr['pixels'], rr['w'], rr['h']).split('\n')
        wl = max((len(x) for x in left), default=0)
        fh.write("    [ppem%d 有问题]%s   |   [ppem%d 参照]\n"
                 % (ppems[si], ' ' * max(0, wl - 14), ppems[ref_si] if ref_si is not None else 0))
        for i in range(max(len(left), len(right))):
            l = left[i] if i < len(left) else ''
            rt = right[i] if i < len(right) else ''
            fh.write("    %-*s   |   %s\n" % (wl, l, rt))
        fh.write("\n")
        k = (g, si)
        if k not in seen:
            seen.add(k)
            tasks.append({'ppem': ppems[si], 'si': si, 'gi': g, 'cp': cp, 'ch': ch,
                          'sim': round(float(sim), 3), 'avg': round(float(med), 3),
                          'shown_as': '?', 'done': False})
json.dump(tasks, open(JS, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print("报告 %s  清单 %s (%d)  总耗时 %.1fs" % (OUT, JS, len(tasks), time.time() - t0))
