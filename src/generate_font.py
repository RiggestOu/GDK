# 本机生成位图字体头文件 + 图标（供 netinfo OPK 使用）
# 用 Pillow 渲染 ASCII 32..126 到 W x H 单元格，二值化，每行存为 uint16
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 12, 14
chars = [chr(i) for i in range(32, 127)]   # 95 个可打印 ASCII

try:
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 13)
except Exception:
    font = ImageFont.load_default()

lines = [
    "#ifndef NETINFO_FONT_H",
    "#define NETINFO_FONT_H",
    "#include <stdint.h>",
    "#define FONT_W %d" % W,
    "#define FONT_H %d" % H,
    "#define FONT_N %d" % len(chars),
    "static const uint16_t FONT_BITMAP[FONT_N * FONT_H] = {",
]
for c in chars:
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), c, fill=255, font=font)
    px = img.load()
    for y in range(H):
        b = 0
        for x in range(W):
            b = (b << 1) | (1 if px[x, y] > 128 else 0)
        lines.append("  0x%04X," % b)
lines.append("};")
lines.append("#endif")

out = "E:/WorkBuddy/GDKmini/GDK/src/netinfo_font.h"
open(out, "w").write("\n".join(lines))
print("FONT OK ->", out, "(%d chars)" % len(chars))

# 生成图标：深蓝底 + 青色边框 + "NET" 文字
opk = "E:/WorkBuddy/GDKmini/GDK/src/netinfo_opk"
os.makedirs(opk, exist_ok=True)
ic = Image.new('RGB', (32, 32), (20, 40, 90))
dc = ImageDraw.Draw(ic)
dc.rectangle([1, 1, 30, 30], outline=(120, 230, 255), width=2)
dc.text((5, 10), "NET", fill=(255, 255, 255),
        font=ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 11))
ic.save(os.path.join(opk, "icon.png"))
print("ICON OK ->", os.path.join(opk, "icon.png"))
