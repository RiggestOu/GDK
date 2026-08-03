#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成最终的人类可读错字清单 位图错字清单.txt, 并预览"用轮廓自动重绘"的效果。"""
import sys, io, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from _ebdt_raw import EBDTFont, render
from _outline_ref import OutlineRef, NOBM

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/位图错字清单.txt"

gb1, gb2 = set(), set()
allgb = []
for q in range(16, 88):
    for w in range(1, 95):
        try:
            ch = bytes([0xA0 + q, 0xA0 + w]).decode('gb2312')
        except Exception:
            continue
        if len(ch) == 1 and '\u4e00' <= ch <= '\u9fff':
            allgb.append(ch)
gb1 = set(allgb[:3755]); gb2 = set(allgb[3755:])

def level(ch):
    if ch in gb1: return "★★常用(GB2312一级)"
    if ch in gb2: return "★次常用(GB2312二级)"
    return "生僻/异体"

ef = EBDTFont(SRC)
ref = OutlineRef(NOBM)
tasks = json.load(open("E:/WorkBuddy/GDKmini/GDK/_fix_list.json", encoding='utf-8'))

def rank(t):
    ch = t['ch']
    return (0 if ch in gb1 else 1 if ch in gb2 else 2, t['ppem'])
tasks.sort(key=rank)

lines = []
lines.append("SourceHanSans-Regular-04.ttf  内嵌点阵位图 错字清单")
lines.append("=" * 74)
lines.append("")
lines.append("检出方法(三种交叉验证, 最终以第①种为准):")
lines.append("  ① 同一字号下, 两个不同的字共享完全相同的位图字节  -> 可靠, 命中 34 处")
lines.append("  ② 位图 与 该字轮廓 的相似度异常低                 -> 噪声大, 无新增")
lines.append("  ③ 同一字在各字号之间形状不合群                     -> 噪声大, 无新增")
lines.append("")
lines.append("说明: 字体制作时把某个字的点阵图直接复制给了形近的另一个字,")
lines.append("      于是屏幕上该字号会显示成另一个字。只影响列出的那一个字号,")
lines.append("      其它字号是正常的(例如 阱 只有 12px 错, 10/11/13/16/24px 都对)。")
lines.append("")
lines.append("=" * 74)
n1 = sum(1 for t in tasks if t['ch'] in gb1)
n2 = sum(1 for t in tasks if t['ch'] in gb2)
lines.append("共 %d 处。其中常用字 %d 处、次常用 %d 处、生僻/异体 %d 处。"
             % (len(tasks), n1, n2, len(tasks) - n1 - n2))
lines.append("=" * 74)
lines.append("")

for i, t in enumerate(tasks, 1):
    ch, ppem, si, gi = t['ch'], t['ppem'], t['si'], t['gi']
    lines.append("[%02d] 「%s」U+%04X   字号 %dpx   %s" % (i, ch, t['cp'], ppem, level(ch)))
    lines.append("     屏幕上会显示成 →「%s」U+%04X" % (t['shown_as'], t['shown_cp']))
    lines.append("     (相似度: 本字 %.2f  vs  冒名字 %.2f ; gi=%d, strike%d)"
                 % (t['iou'], t['shown_iou'], gi, si))
    r = ef.read_bitmap(si, gi)
    if r:
        m = r['metrics']
        cur = render(r['pixels'], r['w'], r['h']).split('\n')
        try:
            fix = ref.render_into(ch, ppem, r['w'], r['h'], m['bx'], m['by'], thresh=110)
        except Exception:
            fix = set()
        fixl = render(fix, r['w'], r['h']).split('\n')
        wl = max(len(x) for x in cur)
        lines.append("     现在(错)%s   |   轮廓自动重绘(供参考)" % (' ' * max(0, wl - 8)))
        for a in range(max(len(cur), len(fixl))):
            x1 = cur[a] if a < len(cur) else ''
            x2 = fixl[a] if a < len(fixl) else ''
            lines.append("     %-*s   |   %s" % (wl, x1, x2))
    lines.append("")

open(OUT, 'w', encoding='utf-8').write('\n'.join(lines))
print('\n'.join(lines[:12]))
print("...")
print("已写", OUT, "行数", len(lines))
for t in tasks[:12]:
    print("  「%s」%dpx -> 显示成「%s」  %s" % (t['ch'], t['ppem'], t['shown_as'], level(t['ch'])))
