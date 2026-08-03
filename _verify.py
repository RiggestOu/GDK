# -*- coding: utf-8 -*-
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from _ebdt_raw import EBDTFont, render
SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
ef = EBDTFont(SRC)
for ch in ['\u9631', '\u9621', '\u9677']:   # 阱 阡 陷
    gi = ef.glyph_index('uni%04X' % ord(ch))
    print("=== %s U+%04X gi=%d ===" % (ch, ord(ch), gi))
    for si in (0, 4, 8, 16):
        r = ef.read_bitmap(si, gi)
        if not r: continue
        print("-- strike%d ppem=%d %dx%d imgfmt=%d" % (si, r['ppem'], r['w'], r['h'], r['imgfmt']))
        print(render(r['pixels'], r['w'], r['h']))
