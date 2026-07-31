#!/usr/bin/env python3
# 镜像新版底部布局：标题栏随字号长高；底部 = HUD 单行(小号12) + 脚注折行(正文字号)
# 验证不同正文字号下文字是否完整在屏内、HUD 与脚注是否纵向分层不重叠。
import sys
from PIL import Image, ImageDraw, ImageFont

SCREEN_W, SCREEN_H = 320, 240
MARGIN = 6
PROG_H = 8
SCALE = 3
FONTP = "C:/Windows/Fonts/msyh.ttc"  # 用雅黑近似思源黑体度量

def load(sz):
    try:
        return ImageFont.truetype(FONTP, sz)
    except Exception:
        return ImageFont.load_default()

def cjk_w(ch, fs):
    # 近似：CJK 全宽=fs，ASCII≈0.55fs
    return fs if ord(ch) >= 0x2E80 else int(fs * 0.55)

def text_w(s, font, fs):
    if not s:
        return 0
    return sum(cjk_w(c, fs) for c in s)

def wrap(s, fs, maxw):
    lines, cur = [], ""
    cw = 0
    for c in s:
        w = cjk_w(c, fs)
        if cw + w > maxw and cur:
            lines.append(cur); cur = c; cw = w
        else:
            cur += c; cw += w
    if cur:
        lines.append(cur)
    return lines or [""]

def draw_case(d, ox, oy, fs, footer, title):
    # 标题栏
    font = load(fs)
    fh = fs + 2  # 近似 FontHeight
    title_h = fh + 6
    line_h = fh
    hud_font = load(12); hud_fh = 14
    hud_lh = hud_fh + 2
    flines = wrap(footer, fs, SCREEN_W - 2 * MARGIN)
    n = max(1, len(flines))
    status_h = hud_lh + n * line_h + 4
    # 标题蓝条
    d.rectangle([ox, oy, ox + SCREEN_W * SCALE, oy + title_h * SCALE], fill=(90, 160, 240))
    d.rectangle([ox, oy + title_h * SCALE - 2, ox + SCREEN_W * SCALE, oy + title_h * SCALE], fill=(0, 0, 0))
    ty = (title_h - fh) / 2
    d.text((ox + MARGIN * SCALE, oy + ty * SCALE), title, fill=(255, 255, 255), font=load(fs))
    # 正文占位（几条灰线）
    body_top = title_h + 3
    st = SCREEN_H - status_h
    yb = body_top
    while yb < st - 4:
        d.rectangle([ox + MARGIN * SCALE, oy + yb * SCALE, ox + (SCREEN_W - MARGIN) * SCALE, oy + (yb + line_h - 4) * SCALE], fill=(60, 64, 74))
        yb += line_h
    # 进度条
    py = st - PROG_H - 1
    d.rectangle([ox, oy + py * SCALE, ox + SCREEN_W * SCALE, oy + (py + PROG_H) * SCALE], fill=(40, 42, 50))
    d.rectangle([ox + MARGIN * SCALE, oy + py * SCALE, ox + (MARGIN + (SCREEN_W - 2 * MARGIN - 28) * 0.6) * SCALE, oy + (py + PROG_H) * SCALE], fill=(90, 160, 240))
    d.text((ox + (SCREEN_W - MARGIN - 24) * SCALE, oy + py * SCALE), "60%", fill=(200, 210, 230), font=load(fs))
    # 底部状态区背景
    d.rectangle([ox, oy + st * SCALE, ox + SCREEN_W * SCALE, oy + SCREEN_H * SCALE], fill=(30, 32, 40))
    d.rectangle([ox, oy + st * SCALE, ox + SCREEN_W * SCALE, oy + st * SCALE + 2], fill=(0, 0, 0))
    # HUD 行（小号右对齐）
    part = "12:34 亮100% 87%"
    pfont = load(12)
    pw = text_w(part, pfont, 12)
    d.text((ox + (SCREEN_W - MARGIN) * SCALE - pw * SCALE, oy + (st + 1) * SCALE), part, fill=(200, 210, 230), font=pfont)
    # 脚注折行（左对齐）
    fy = st + hud_lh + 1
    for i, ln in enumerate(flines):
        d.text((ox + MARGIN * SCALE, oy + (fy + i * line_h) * SCALE), ln, fill=(200, 210, 230), font=load(fs))
    # 标注
    d.text((ox + 4 * SCALE, oy + 4 * SCALE), f"字号{fs} 标题{title_h}px 底部{status_h}px 脚注{n}行", fill=(255, 255, 0), font=load(11))
    return status_h, n

img = Image.new("RGB", (SCREEN_W * SCALE * 2 + 20, SCREEN_H * SCALE), (16, 18, 26))
d = ImageDraw.Draw(img)
draw_case(d, 0, 0, 14, "L1+Y或圆3 查看按键说明", "三体 第一部 黑暗森林")
draw_case(d, SCREEN_W * SCALE + 20, 0, 22, "L1+Y或圆3 查看按键说明", "三体 第一部 黑暗森林")
img.save("preview_layout_2x.png")
print("written preview_layout_2x.png  size", img.size)
