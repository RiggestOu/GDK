#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成底部状态栏布局预览：用设备等效字体度量(微软雅黑@设备字号)精确模拟
render.c 中 draw_hud / ui_draw_status 的新逻辑，验证「左侧页脚 + 右下角簇」
在 320x240 下是否重叠、是否足够。
"""
from PIL import Image, ImageDraw, ImageFont

SCREEN_W, SCREEN_H = 320, 240
TITLE_H, STATUS_H = 18, 13
MARGIN = 6
FONT_PX = 14                     # 设备 ui->font 字号
SCALE = 3                        # 预览放大倍数(便于看清)；比例与设备一致

FONT = "C:/Windows/Fonts/msyh.ttc"   # 含中文 + ★(U+2605)，度量接近思源黑体
f = ImageFont.truetype(FONT, FONT_PX)
fd = ImageDraw.Draw(Image.new("RGB", (1, 1)))

def text_w(s):
    return fd.textlength(s, font=f)

# ---- 复刻 render.c draw_hud 的右下角簇 ----
def cluster_width(bookmark_on, clock, bright, batt):
    if batt >= 0:
        part = f"{clock} 亮{bright}% {batt}%"
    else:
        part = f"{clock} 亮{bright}% ?"
    rw = text_w(part)
    sw = 0
    if bookmark_on:
        sw = text_w("★") + 4
    return sw + rw

def draw_cluster(d, x_right, y, bookmark_on, clock, bright, batt, color):
    if batt >= 0:
        part = f"{clock} 亮{bright}% {batt}%"
    else:
        part = f"{clock} 亮{bright}% ?"
    total = cluster_width(bookmark_on, clock, bright, batt)
    x = x_right - total
    if bookmark_on:
        sw = text_w("★") + 4
        d.text((x, y), "★", font=f, fill=(255, 220, 80))
        x += sw
    d.text((x, y), part, font=f, fill=color)

def make_preview(bookmark_on, fname):
    W, H = SCREEN_W * SCALE, SCREEN_H * SCALE
    img = Image.new("RGB", (W, H), (16, 18, 26))
    d = ImageDraw.Draw(img)

    # 标题栏(占顶部,模拟书名,不挡时间了)
    d.rectangle([0, 0, W, TITLE_H * SCALE], fill=(90, 160, 240))
    d.rectangle([0, TITLE_H * SCALE - 1, W, TITLE_H * SCALE], fill=(0, 0, 0))
    d.text((MARGIN * SCALE, 3 * SCALE), "三体（长篇科幻小说节选示例书名）",
           font=f, fill=(255, 255, 255))

    # 正文占位
    for i in range(8):
        d.text((MARGIN * SCALE, (TITLE_H + 6 + i * 16) * SCALE),
               "这是阅读区正文示例行，用于观察整体排版效果。",
               font=f, fill=(210, 214, 224))

    # 进度条
    py = (SCREEN_H - STATUS_H - 8 - 1) * SCALE
    d.rectangle([0, py - 1, W, py], fill=(0, 0, 0))
    bar_w = (SCREEN_W - 2 * MARGIN - 28) * SCALE
    d.rectangle([MARGIN * SCALE, py, MARGIN * SCALE + bar_w, py + 8 * SCALE], fill=(40, 42, 50))
    d.rectangle([MARGIN * SCALE, py, MARGIN * SCALE + bar_w * 0.37, py + 8 * SCALE], fill=(90, 160, 240))
    d.text(((SCREEN_W - MARGIN - 24) * SCALE, py), "37%", font=f, fill=(200, 210, 230))

    # 状态栏(底)
    y0 = (SCREEN_H - STATUS_H) * SCALE
    d.rectangle([0, y0, W, H], fill=(30, 32, 40))
    d.rectangle([0, y0, W, y0 + 1], fill=(0, 0, 0))

    # 左侧页脚(左对齐)
    left = "X 书签 Y 菜单 B 退出"
    d.text((MARGIN * SCALE, (SCREEN_H - STATUS_H + 2) * SCALE), left,
           font=f, fill=(200, 210, 230))

    # 右下角簇(右对齐): ★ 时间 亮度% 电量%
    draw_cluster(d, (SCREEN_W - MARGIN) * SCALE, (SCREEN_H - STATUS_H + 2) * SCALE,
                 bookmark_on, "12:34", 100, 87, (200, 210, 230))

    # ---- 叠加诊断：左页脚右边界 vs 簇左边界 ----
    left_w = text_w(left)
    left_end = (MARGIN + left_w)
    cluster_total = cluster_width(bookmark_on, "12:34", 100, 87)
    cluster_start = (SCREEN_W - MARGIN - cluster_total)
    gap = cluster_start - left_end
    d.text((MARGIN * SCALE, 2 * SCALE), f"左页脚右端={left_end:.0f}px  簇左端={cluster_start:.0f}px  间隙={gap:.0f}px{'  ★已显示' if bookmark_on else ''}",
           font=f, fill=(255, 255, 0))

    img.save(fname)
    return left_end, cluster_start, gap

if __name__ == "__main__":
    le, cs, gap = make_preview(True, "preview_bottom_2x.png")
    make_preview(False, "preview_bottom_nostar_2x.png")
    print(f"左页脚右端={le:.1f}px, 簇左端={cs:.1f}px, 间隙={gap:.1f}px")
    print("overlap" if gap < 0 else "OK 不重叠")
