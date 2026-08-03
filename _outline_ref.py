#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
_outline_ref.py —— 用"已删除 EBLC/EBDT 的字体副本"渲染轮廓, 得到与内嵌位图可比的参考图。
因为 FreeType 只要字体里有嵌入位图就会优先用它, 必须用无位图副本才能拿到真正的轮廓形状。
"""
import os
from PIL import ImageFont

NOBM = "E:/WorkBuddy/GDKmini/GDK/src/opk_build/system.ttf"   # 无位图版


def ensure_nobitmap(src, dst=NOBM):
    if os.path.exists(dst) and os.path.getsize(dst) > 100000:
        return dst
    from fontTools.ttLib import TTFont
    f = TTFont(src)
    for t in ('EBLC', 'EBDT', 'EBSC', 'CBLC', 'CBDT'):
        if t in f:
            del f[t]
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    f.save(dst)
    return dst


class OutlineRef:
    def __init__(self, path=NOBM):
        self.path = path
        self._cache = {}

    def font(self, ppem):
        f = self._cache.get(ppem)
        if f is None:
            f = ImageFont.truetype(self.path, ppem)
            self._cache[ppem] = f
        return f

    def render(self, ch, ppem, thresh=110):
        """返回 (pixels:set, w, h, bx, by) —— bx/by 与 EBDT 度量同义(by 从基线向上)"""
        f = self.font(ppem)
        try:
            m = f.getmask(ch, mode='L')
        except Exception:
            return None
        w, h = m.size
        if w == 0 or h == 0:
            return (set(), 0, 0, 0, 0)
        data = bytes(m)
        pix = set()
        for y in range(h):
            ro = y * w
            for x in range(w):
                if data[ro + x] >= thresh:
                    pix.add((x, y))
        try:
            bbox = f.getbbox(ch)
            ascent, _ = f.getmetrics()
            bx = bbox[0]
            by = ascent - bbox[1]
        except Exception:
            bx, by = 0, 0
        return (pix, w, h, bx, by)

    def render_into(self, ch, ppem, tw, th, tbx, tby, thresh=110, dx=0, dy=0):
        """把轮廓渲染结果按基线对齐, 装进 tw x th 的目标位图框"""
        r = self.render(ch, ppem, thresh)
        if r is None:
            return set()
        pix, w, h, bx, by = r
        ox = bx - tbx + dx        # 目标(X) -> 源(u) 的位移: u = X + ox
        oy = by - tby + dy        # v = Y + oy
        out = set()
        for X in range(tw):
            for Y in range(th):
                if (X + ox, Y + oy) in pix:
                    out.add((X, Y))
        return out


def iou(a, b):
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    return len(a & b) / len(a | b)


def best_align_iou(ref_pix, rw, rh, rbx, rby, tgt_pix, tw, th, tbx, tby, slack=2):
    """在 ±slack 像素范围内搜索最佳对齐, 返回最大 IoU"""
    best = 0.0
    base_ox = rbx - tbx
    base_oy = rby - tby
    for dx in range(-slack, slack + 1):
        for dy in range(-slack, slack + 1):
            ox = base_ox + dx
            oy = base_oy + dy
            shifted = set()
            for (u, v) in ref_pix:
                X = u - ox
                Y = v - oy
                if 0 <= X < tw and 0 <= Y < th:
                    shifted.add((X, Y))
            s = iou(shifted, tgt_pix)
            if s > best:
                best = s
    return best
