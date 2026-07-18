#!/usr/bin/env bash
# ============================================================
# 编译 EPUB 阅读器并打包成 iuxui 可直接识别的 app 文件夹。
#
# iuxui (GDK mini 自带启动器) 的识别规律(已实测设备上的 app):
#   - 绝大多数是「.dge 启动器文件」: sdljy -> sdljy.dge, sdl-pal -> sdlpal.dge
#     .dge 可以是 shell 脚本(如 sdljy.dge) 也可以是 MIPS 二进制(如 sdlpal.dge)
#   - 个别 app (如 af-84) 用「与文件夹同名、无扩展名的元数据文件」
#   - .gcw0.desktop 不是 iuxui 的识别方式(标准 OpenDingux 才用)
# 因此这里统一用 .dge 启动器, 这是设备上最主流、最稳妥的方式。
#
# 输出:
#   - pkg/              解压好的 app 文件夹(iuxui 用), 含 epubreader.dge 启动器
#   - EPUBReader.opk    标准 OpenDingux 压缩包(可选, 非 iuxui 必需)
#
# 部署: 把整个 pkg/ 文件夹改名为 EPUBReader, 复制到设备内部存储 G:/apps/EPUBReader/
#       (iuxui 只扫描 G:/apps 内部存储, 不扫描 SD 卡 H:/apps)。重启即可在菜单看到。
# ============================================================
set -e
cd "$(dirname "$0")"

echo "==> 编译 ..."
make -f Makefile.gcw0 clean
make -f Makefile.gcw0

rm -rf pkg && mkdir -p pkg
# 二进制就叫 epubreader(不要改名, 否则会和下面的 .dge 启动器混淆)
cp epubreader pkg/
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
# (libSDL_ttf -> libfreetype/libpng/libiconv；libSDL_image -> libpng/libjpeg；
#  libSDL_ttf -> libts.so.0)。之前漏打这些传递依赖会导致运行期
# "can't load library 'libXXX.so'"。
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
      # libdrm/libudev 是设备系统 libSDL 的依赖, 由固件在 /lib,/usr/lib 提供, 不打包
      case "$lib" in
        libdrm.so*|libudev.so*) echo "    (跳过 $lib: 设备系统库)"; continue ;;
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

# ============================================================
# iuxui 启动器: .dge 文件(本设备主流方式, 见 sdljy/sdl-pal 等)
# 它是个 shell 脚本: 切到自身目录, 设好帧缓冲/鼠标/库路径, 再运行二进制。
# 固定用 fbcon + /dev/fb0 (设备帧缓冲); 把运行日志写到 app 目录 run.log 便于排查。
# 注意: FAT 文件系统大小写不敏感, 二进制叫 epubreader, 启动器叫 epubreader.dge,
#       两者扩展名不同不会撞名; 不要再用「无扩展名元数据文件」(会和二进制撞名被覆盖)。
# ============================================================
cat > pkg/epubreader.dge <<'EOF'
#!/bin/sh
DIR=$(dirname "$0")
cd "$DIR"
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
export SDL_NOMOUSE=1
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
LOGFILE="$DIR/run.log"
exec 1>>"$LOGFILE" 2>&1
echo "=== EPUBReader launch $(date) ==="
./epubreader
echo "=== exit code: $? ==="
EOF
chmod +x pkg/epubreader.dge
echo "==> 已生成启动器 pkg/epubreader.dge"

# iuxui 按 <dge基名>.png 找图标(如 sdljy.dge -> sdljy.png), 这里生成 epubreader.png
if [ -f icon.png ]; then
  cp icon.png pkg/epubreader.png
  echo "==> 已生成图标 pkg/epubreader.png"
fi

if ! command -v mksquashfs >/dev/null 2>&1; then
  echo "!! 缺少 mksquashfs，请先:  sudo apt-get install squashfs-tools"
  exit 1
fi

rm -f EPUBReader.opk
mksquashfs pkg EPUBReader.opk -all-root -noappend -no-progress
echo
echo "==> 已生成 EPUBReader.opk (标准 OpenDingux 用, 可选)"
echo "==> 已生成解压文件夹 pkg/ (iuxui 用)"
echo "    部署到 GDK mini：把整个 pkg/ 文件夹重命名为 EPUBReader，"
echo "    复制到设备内部存储 G:/apps/EPUBReader/ (iuxui 只扫描 G:/apps，不扫描 SD 卡 H:/apps)。"
echo "    重启后菜单里即可看到 EPUB Reader，启动器为 epubreader.dge。"
