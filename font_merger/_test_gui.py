# -*- coding: utf-8 -*-
"""无头冒烟测试：扫描 -> 冲突窗口 get_choices -> 构建 -> 重载校验。"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtWidgets import QApplication

import font_merger_gui as gui
from font_merge_core import FontMerger, glyph_outline_to_path

ARIAL = r"C:\Windows\Fonts\arial.ttf"
ARIALBD = r"C:\Windows\Fonts\arialbd.ttf"


def test_preview():
    m = FontMerger()
    m.add_font(ARIAL)
    path, bbox = glyph_outline_to_path(m.sources[0].font, "A", 1.0)
    assert path.elementCount() > 0, "A 的轮廓不应为空"
    assert bbox[2] > bbox[0], "bbox 应有效: %r" % (bbox,)
    print("[OK] glyph_outline_to_path: path elements=%d bbox=%s"
          % (path.elementCount(), bbox))


def main():
    app = QApplication(sys.argv)

    test_preview()

    m = FontMerger()
    m.add_font(ARIAL)
    m.add_font(ARIALBD)
    conflicts, stats = m.scan()
    print("[OK] scan: total=%d conflicts=%d"
          % (stats["total_codepoints"], stats["conflict_count"]))
    assert conflicts, "arial+arialbd 应产生冲突"

    names = [s["name"] for s in stats["sources"]]
    dlg = gui.ConflictResolver(m, conflicts, names)
    choices = dlg.get_choices()
    print("[OK] ConflictResolver.get_choices: %d entries" % len(choices))
    assert len(choices) == len(conflicts)
    for c in conflicts:
        fi = choices[c.codepoint]
        cand_fis = [cd["font_index"] for cd in c.candidates]
        assert fi in cand_fis, "选择 %d 必须是候选之一 %r" % (fi, cand_fis)

    out = os.path.join(tempfile.gettempdir(), "_t_gui_merge.ttf")
    res = m.build(choices, out, "GuiTest")
    print("[OK] build:", res)

    from fontTools.ttLib import TTFont
    f = TTFont(out)
    best = f.getBestCmap()
    print("[OK] reload bestCmap len=%d" % len(best))
    # 校验一个冲突码位确实落在合并字体里
    cp = conflicts[0].codepoint
    assert cp in best, "冲突码位 U+%04X 应在 cmap 中" % cp
    # glyphOrder 含 .notdef(GID0)，故比 unique_glyphs 多 1
    assert len(f.getGlyphOrder()) == res["unique_glyphs"] + 1, (
        len(f.getGlyphOrder()), res["unique_glyphs"])
    f.close()
    os.remove(out)
    print("\n全部冒烟测试通过。")


if __name__ == "__main__":
    main()
