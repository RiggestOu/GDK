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

# 自动打包二进制依赖的动态库（递归），使 app 自包含。
# 关键点：不仅要打包 epubreader 直接依赖的库，还要递归打包它们的传递依赖
# （例如 libSDL_ttf 依赖 libts.so.0 —— 之前漏打 libts 导致运行期 "can't load library 'libts.so.0'"）。
TOOLCHAIN=/opt/gcw0-toolchain
CROSS_BIN="$TOOLCHAIN/bin/mipsel-gcw0-linux-uclibc-"
OBJDUMP="${CROSS_BIN}objdump"
SYSROOT="$TOOLCHAIN/mipsel-gcw0-linux-uclibc/sysroot"
LIBDIR="$SYSROOT/usr/lib"
if [ -x "${CROSS_BIN}objdump" ] && [ -d "$LIBDIR" ]; then
  mkdir -p pkg/lib
  echo "==> 递归分析依赖并打包动态库 ..."
  SEEN=$(mktemp)
  to_scan="epubreader"
  while [ -n "$to_scan" ]; do
    cur=$(echo "$to_scan" | cut -d' ' -f1)
    to_scan=$(echo "$to_scan" | cut -d' ' -f2-)
    for lib in $("$OBJDUMP" -p "$cur" 2>/dev/null | awk '/NEEDED/ {print $2}'); do
      # 核心 libSDL 与系统基库用设备自带的（覆盖会导致 ABI 不匹配崩溃）
      case "$lib" in
        libSDL-1.2.so*|libSDL.so*|libpthread*|libc.so*|libm.so*|libgcc_s.so*|ld-uClibc*)
          echo "    (跳过 $lib: 使用设备自带基库)"; continue ;;
      esac
      grep -qxF "$lib" "$SEEN" 2>/dev/null && continue
      echo "$lib" >> "$SEEN"
      found=$(find "$LIBDIR" -maxdepth 1 -name "$lib" 2>/dev/null | head -1)
      if [ -n "$found" ]; then
        cp -L "$found" "pkg/lib/$lib"
        echo "    bundled $lib"
        to_scan="$to_scan pkg/lib/$lib"
      else
        echo "    (跳过 $lib: 可能由系统 libc/基库提供)"
      fi
    done
  done
  rm -f "$SEEN"
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
