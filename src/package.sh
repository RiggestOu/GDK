#!/usr/bin/env bash
# ============================================================
# 把编译好的阅读器打包成 OpenDingux .opk，放到 SD 卡 APPS 目录即可。
# 用法 (在 src/ 目录):  ./package.sh
# ============================================================
set -e
cd "$(dirname "$0")"

echo "==> 编译 ..."
make -f Makefile.gcw0 clean
make -f Makefile.gcw0

rm -rf pkg && mkdir -p pkg
cp epubreader pkg/
cp EPUBReader.gcw0.desktop pkg/

# CJK 字体：用户需自行提供一个 .ttf 命名为 font.ttf 放在本目录
if [ -f font.ttf ]; then
  cp font.ttf pkg/
  echo "==> 已包含 font.ttf (CJK 字体)"
else
  echo "!! 警告: 未找到 font.ttf，阅读器需要一款 CJK TTF 才能显示中文。"
  echo "   请从网上下载一款开源中文字体(如文泉驿/Noto CJK)并重命名为 font.ttf 放到本目录，再打包。"
fi

# 启动脚本：固定帧缓冲驱动（iuxui 默认 fbcon）
cat > pkg/launch.sh <<'EOF'
#!/bin/sh
DIR=$(dirname "$0")
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
exec "$DIR/epubreader"
EOF
chmod +x pkg/launch.sh

if ! command -v mksquashfs >/dev/null 2>&1; then
  echo "!! 缺少 mksquashfs，请先:  sudo apt-get install squashfs-tools"
  exit 1
fi

rm -f EPUBReader.opk
mksquashfs pkg EPUBReader.opk -all-root -noappend -no-progress
echo
echo "==> 已生成 EPUBReader.opk"
echo "    把它复制到 SD 卡 (H: 盘) 的 APPS 目录，在 GDK mini 菜单里即可看到 EPUB Reader。"
