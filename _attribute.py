#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
对审计出的每一组"共享同一位图"的字, 判定谁是"正主"、谁是"受害者(被显示成别的字)"。
判据: 用无位图字体渲染两字的轮廓, 与嵌入位图算最佳对齐 IoU, 高者为正主。
"""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from _ebdt_raw import EBDTFont, render
from _outline_ref import OutlineRef, best_align_iou, ensure_nobitmap

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
ensure_nobitmap(SRC)
ef = EBDTFont(SRC)
gi2cp = ef.cmap_gi()
ef._ensure_loca()
ref = OutlineRef()

import hashlib
outl = {}
for g in gi2cp:
    ob = ef.outline_bytes(g)
    outl[g] = hashlib.md5(ob).digest() if ob else b'EMPTY'

CJK = lambda cp: (0x3400 <= cp <= 0x9FFF) or (0xF900 <= cp <= 0xFAFF)

groups_all = []
for st in ef.strikes:
    si, ppem = st['idx'], st['ppem']
    if ppem <= 9:
        continue
    groups = {}
    for g in gi2cp:
        k = ef.bitmap_key(si, g)
        if k is None:
            continue
        w, h, data = k
        if not data or not any(data):
            continue
        if sum(bin(b).count('1') for b in data) < 6:
            continue
        groups.setdefault((w, h, data), []).append(g)
    for key, gl in groups.items():
        if len(gl) < 2:
            continue
        if len(set(outl.get(g, b'') for g in gl)) < 2:
            continue
        cjk = [g for g in gl if any(CJK(cp) for cp in gi2cp.get(g, []))]
        if len(cjk) < 2:
            continue
        groups_all.append((ppem, si, key[0], key[1], gl))

print("待归属组数:", len(groups_all), flush=True)

rows = []
for ppem, si, w, h, gl in groups_all:
    r0 = ef.read_bitmap(si, gl[0])
    tgt = r0['pixels']
    m = r0['metrics']
    scores = []
    for g in gl:
        cp = sorted(gi2cp[g])[0]
        ch = chr(cp)
        rr = ref.render(ch, ppem)
        if rr is None or not rr[0]:
            scores.append((0.0, g, ch, cp))
            continue
        pix, rw, rh, rbx, rby = rr
        s = best_align_iou(pix, rw, rh, rbx, rby, tgt, w, h, m['bx'], m['by'], slack=2)
        scores.append((s, g, ch, cp))
    scores.sort(reverse=True)
    rows.append((ppem, si, w, h, scores, tgt, m))

rows.sort(key=lambda r: r[0])

OUT = "E:/WorkBuddy/GDKmini/GDK/_fix_list.txt"
with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write("内嵌位图错字 —— 受害者判定\n")
    fh.write("(同一位图被两个字共用; IoU 高者=位图真身[正主], 低者=受害者, 屏幕上会显示成正主的字形)\n")
    fh.write("=" * 78 + "\n\n")
    for ppem, si, w, h, scores, tgt, m in rows:
        owner = scores[0]
        victims = scores[1:]
        fh.write("ppem=%-3d strike%-2d %dx%d\n" % (ppem, si, w, h))
        fh.write("   位图真身(正主): U+%04X %s   IoU=%.3f\n" % (owner[3], owner[2], owner[0]))
        for s, g, ch, cp in victims:
            fh.write("   ★受害者      : U+%04X %s   IoU=%.3f  gi=%d  -> 屏幕显示成「%s」\n"
                     % (cp, ch, s, g, owner[2]))
        fh.write(render(tgt, w, h).replace('\n', '\n      ') + "\n")
        fh.write("      \n\n")

import json
tasks = []
for ppem, si, w, h, scores, tgt, m in rows:
    o = scores[0]
    for s, g, ch, cp in scores[1:]:
        tasks.append({'ppem': ppem, 'si': si, 'gi': g, 'cp': cp, 'ch': ch,
                      'iou': round(s, 3), 'shown_as': o[2], 'shown_cp': o[3],
                      'shown_iou': round(o[0], 3), 'w': w, 'h': h, 'done': False})
with open("E:/WorkBuddy/GDKmini/GDK/_fix_list.json", 'w', encoding='utf-8') as fh:
    json.dump(tasks, fh, ensure_ascii=False, indent=1)
print("已写", OUT, "和 _fix_list.json (%d 项)" % len(tasks))
for ppem, si, w, h, scores, tgt, m in rows:
    o = scores[0]
    v = scores[1]
    print("ppem=%-3d 受害 U+%04X %s (IoU %.2f)  显示成  U+%04X %s (IoU %.2f)"
          % (ppem, v[3], v[2], v[0], o[3], o[2], o[0]))
