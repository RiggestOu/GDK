#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
全量审计 SourceHanSans-Regular-04.ttf 的内嵌位图错字。

判据(经 阱/阡 实证):
  同一 strike 内, 两个 **轮廓不同** 的字形若共享 **完全相同的位图字节** ,
  说明制作字体时位图被张冠李戴 -> 显示成别的字。

过滤:
  - 跳过空位图 / 墨点过少
  - 跳过轮廓完全相同的字形(异体字合法复用同一图形)
  - 跳过无 cmap 映射的字形(用户看不到)
  - ppem 太小(<=9)时 CJK 汉字碰撞属物理必然, 单独分栏列出、不算 bug
输出: _audit2_report.txt
"""
import sys, io, time
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from _ebdt_raw import EBDTFont, render

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/_audit2_report.txt"

t0 = time.time()
ef = EBDTFont(SRC)
gi2cp = ef.cmap_gi()
print("字形总数=%d  有 cmap 的字形=%d  strikes=%s" % (ef.numGlyphs if hasattr(ef, 'numGlyphs') else -1,
                                                len(gi2cp), ef.ppems), flush=True)
ef._ensure_loca()
print("numGlyphs=%d" % ef.numGlyphs, flush=True)

# 轮廓字节哈希(只算有 cmap 的字形, 省时间)
import hashlib
outl = {}
for g in gi2cp:
    ob = ef.outline_bytes(g)
    outl[g] = hashlib.md5(ob).digest() if ob else b'EMPTY'
print("轮廓哈希完成 %.1fs" % (time.time() - t0), flush=True)

CJK = lambda cp: (0x3400 <= cp <= 0x9FFF) or (0xF900 <= cp <= 0xFAFF) or (0x20000 <= cp <= 0x2FA1F)

def label(g):
    cps = gi2cp.get(g, [])
    if not cps:
        return "gi%d" % g
    s = []
    for cp in sorted(cps)[:3]:
        try:
            c = chr(cp)
        except Exception:
            c = '?'
        s.append("U+%04X %s" % (cp, c))
    return " / ".join(s)

report_bug = []      # 真错字候选
report_tiny = []     # 小字号物理碰撞

for st in ef.strikes:
    si, ppem = st['idx'], st['ppem']
    groups = {}
    n_have = 0
    for g in gi2cp:
        k = ef.bitmap_key(si, g)
        if k is None:
            continue
        n_have += 1
        w, h, data = k
        if not data or not any(data):
            continue
        ink = sum(bin(b).count('1') for b in data)
        if ink < 6:            # 太少墨点(标点/空白)不判
            continue
        groups.setdefault((w, h, data), []).append(g)
    dup = 0
    for key, gl in groups.items():
        if len(gl) < 2:
            continue
        # 轮廓相同 -> 合法复用
        shapes = set(outl.get(g, b'') for g in gl)
        if len(shapes) < 2:
            continue
        dup += 1
        w, h, data = key
        cjkn = sum(1 for g in gl if any(CJK(cp) for cp in gi2cp.get(g, [])))
        rec = (ppem, si, w, h, gl, cjkn)
        if cjkn < 2:
            continue                       # 拉丁/希腊/西里尔同形符号, 良性
        if ppem <= 9:
            report_tiny.append(rec)
        else:
            report_bug.append(rec)
    print("strike%-2d ppem=%-3d 有位图字形=%-6d 位图组=%-6d 疑似冲突组=%d  (%.1fs)"
          % (si, ppem, n_have, len(groups), dup, time.time() - t0), flush=True)

print("\n真错字候选组 %d  小字号物理碰撞组 %d" % (len(report_bug), len(report_tiny)), flush=True)

# 排序: ppem 大的更可疑(信息足够却撞), 组内字数少的更像复制错误
report_bug.sort(key=lambda r: (-r[0], len(r[4])))

with open(OUT, 'w', encoding='utf-8') as fh:
    fh.write("SourceHanSans-Regular-04.ttf 内嵌位图错字审计\n")
    fh.write("判据: 同一字号下, 轮廓不同的两个字共享完全相同的位图字节\n")
    fh.write("=" * 72 + "\n\n")
    fh.write("【A】真错字候选(ppem>=10, 或非 CJK): %d 组\n\n" % len(report_bug))
    for ppem, si, w, h, gl, cjkn in report_bug:
        fh.write("--- ppem=%d (strike%d)  %dx%d  共 %d 个字形共享同一位图 ---\n"
                 % (ppem, si, w, h, len(gl)))
        for g in gl:
            fh.write("    gi=%-6d %s\n" % (g, label(g)))
        r = ef.read_bitmap(si, gl[0])
        if r:
            fh.write(render(r['pixels'], r['w'], r['h']).replace('\n', '\n    ') + "\n")
            fh.write("    " + "\n")
        fh.write("\n")
    fh.write("\n" + "=" * 72 + "\n")
    fh.write("【B】ppem<=9 小字号物理碰撞(信息量不足, 通常不算 bug): %d 组\n\n" % len(report_tiny))
    for ppem, si, w, h, gl, cjkn in report_tiny[:200]:
        fh.write("ppem=%d %dx%d : %s\n" % (ppem, w, h,
                 " | ".join(label(g) for g in gl[:8]) + (" ..." if len(gl) > 8 else "")))

print("报告已写 %s  (%.1fs)" % (OUT, time.time() - t0))
