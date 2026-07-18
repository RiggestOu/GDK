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
cp icon.png pkg/ 2>/dev/null || true

# CJK 字体：用户需自行提供一个 .ttf 命名为 font.ttf 放在本目录
if [ -f font.ttf ]; then
  cp font.ttf pkg/
  echo "==> 已包含 font.ttf (CJK 字体)"
else
  echo "!! 警告: 未找到 font.ttf，阅读器需要一款 CJK TTF 才能显示中文。"
  echo "   请从网上下载一款开源中文字体(如文泉驿/Noto CJK)并重命名为 font.ttf 放到本目录，再打包。"
fi

# 自动打包二进制依赖的动态库，使 app 自包含（不依赖设备固件是否带齐 libSDL_ttf/libSDL_image/libz 等）
TOOLCHAIN=/opt/gcw0-toolchain
CROSS_BIN="$TOOLCHAIN/bin/mipsel-gcw0-linux-uclibc-"
OBJDUMP="${CROSS_BIN}objdump"
SYSROOT="$TOOLCHAIN/mipsel-gcw0-linux-uclibc/sysroot"
LIBDIR="$SYSROOT/usr/lib"
if [ -x "${CROSS_BIN}objdump" ] && [ -d "$LIBDIR" ]; then
  mkdir -p pkg/lib
  echo "==> 分析依赖并打包动态库 ..."
  for lib in $("$OBJDUMP" -p epubreader 2>/dev/null | awk '/NEEDED/ {print $2}'); do
    # 核心 libSDL 用设备自带的(af-84 能跑证明设备的 SDL 已适配 GDK mini 屏幕/按键)；
    # libpthread 在 uClibc 下并入 libc，也不打包。只打包可能缺失的 SDL_ttf/SDL_image/z 等。
    case "$lib" in
      libSDL-1.2.so*|libSDL.so*|libpthread*) echo "    (跳过 $lib: 使用设备自带基库)"; continue ;;
    esac
    found=$(find "$LIBDIR" -maxdepth 1 -name "$lib" 2>/dev/null | head -1)
    if [ -n "$found" ]; then
      cp -L "$found" "pkg/lib/$lib"
      echo "    bundled $lib"
    else
      echo "    (跳过 $lib: 可能由系统 libc/基库提供)"
    fi
  done
  echo "==> 已打包 $(ls pkg/lib 2>/dev/null | wc -l) 个依赖库到 pkg/lib"
else
  echo "!! 未找到交叉工具链，跳过动态库打包（依赖设备固件自带）"
fi

# 启动脚本：固定帧缓冲驱动（iuxui 默认 fbcon），优先加载本地 lib/ 里的依赖库，
# 并把运行日志写到 app 目录(run.log)，便于崩溃时连电脑排查(如缺某个 .so)
cat > pkg/launch.sh <<'EOF'
#!/bin/sh
DIR=$(dirname "$0")
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
LOGFILE="$DIR/run.log"
exec 1>>"$LOGFILE" 2>&1
echo "=== launch $(date) ==="
"$DIR/epubreader"
echo "=== exit code: $? ==="
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
