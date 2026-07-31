#!/usr/bin/env python3
# 模拟圆3 快捷键说明浮层布局（320x240，2x 放大）。
# 镜像 src/main.c 的 ui_keymap_overlay_draw：字号=正文(ui->font_size)，行距=字号，
# 子项缩进=左边距6+2中文字，全文字号随正文放大；内容超高时可用方向键平移(oy/ox)。
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
SC = 2
FONT = "C:/Windows/Fonts/msyh.ttc"  # 微软雅黑，设备用思源黑体度量接近

def font(sz):
    return ImageFont.truetype(FONT, sz, index=0)

def render(fs, oy, ox, out1x, out2x):
    img = Image.new("RGB", (W, H), (18, 20, 28))
    d = ImageDraw.Draw(img, "RGBA")
    d.rectangle([0, 0, W, H], fill=(8, 10, 16, 170))  # 半透明遮罩
    lh = fs
    cw = fs
    subx = 6 + 2 * cw
    f = font(fs)
    y = 4 + oy
    d.text((6 + ox, y), "快捷键说明 (L1+Y/圆3 关闭 · 方向键平移)", font=f, fill=(255, 220, 80, 255))
    y += lh + 4
    rows = [
        ("下一页", "L1 / R1"), ("上一页", "L2 / R2"), ("打开/确认", "A"),
        ("返回/退出", "B"), ("菜单", "Y"), ("书签", "X"), ("目录", "→"),
        ("息屏", "START"), ("图片缩放", "L1+L2 / R1+R2"), ("退出App", "L1+START / R1+START"),
    ]
    for name, val in rows:
        d.text((6 + ox, y), f"{name}: {val}", font=f, fill=(222, 228, 238, 255)); y += lh
    d.text((6 + ox, y), "亮度: L1+音量 ±50%  L2+音量 ±20%  音量 ±5%", font=f, fill=(222, 228, 238, 255)); y += lh
    d.text((6 + ox, y), "进入缩放界面后：", font=f, fill=(200, 215, 235, 255)); y += lh
    d.text((subx + ox, y), "按住肩键(L1/R1)再按方向键: 放大/缩小", font=f, fill=(190, 205, 225, 255)); y += lh
    d.text((subx + ox, y), "→/A 放大10%   ←/Y 缩小10%", font=f, fill=(190, 205, 225, 255)); y += lh
    d.text((subx + ox, y), "↑/X 放大1%   ↓/B 缩小1%", font=f, fill=(190, 205, 225, 255)); y += lh
    d.text((subx + ox, y), "不按肩键: 方向键/ABXY 平移查看大图", font=f, fill=(190, 205, 225, 255)); y += lh
    d.text((6 + ox, y), "SELECT: 阅读页呼出书签光标/其它界面返回", font=f, fill=(200, 215, 235, 255)); y += lh
    d.text((6 + ox, y), "L1+START / L2+START: 强制退出(自动保存书签)", font=f, fill=(200, 215, 235, 255)); y += lh
    content_h = y - oy
    img.save(out1x)
    big = img.resize((W * SC, H * SC), Image.NEAREST)
    big.save(out2x)
    print(f"fs={fs} oy={oy} -> content_h={content_h} end_y={y} fits(oy=0)={content_h<H}")
    return content_h

# 默认字号（看全）
render(12, 0, 0, "preview_overlay_1x.png", "preview_overlay_2x.png")
# 放大字号 + 下滚120px（模拟看后半部分；content_h 远超240，方向键可平移）
render(18, -120, 0, "preview_overlay_big_1x.png", "preview_overlay_big_2x.png")
