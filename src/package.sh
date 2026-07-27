#!/usr/bin/env bash
# ============================================================
# 编译 EPUB 阅读器并打包成 iuxui 可直接识别的 app。
#
# ★ iuxui 注册机制（已读 GMenuNX 源码坐实，非猜测）★
#   iuxui 是 GMenuNX/GMenu2X 的换皮启动器。它【不扫描 app 文件夹】，
#   只读取  $HOME/.gmenunx/sections/<分类>/<link>.lnk  来列出应用。
#   - $HOME 在设备上是内部存储根（G: 盘在 U 盘模式下的根），如 /usr/local/home。
#   - 每个 sections/ 下的子目录 = 一个分类(tab)；启动器强制内置
#     applications / settings 两个分类 → 「apps」分类必定存在。
#   - 每条 .lnk 必须让 exec 指向【存在的绝对路径文件】，否则被静默丢弃。
#   - 图标会自动回退到  dir_name(exec)/<exec基名>.png，故同名 png 即可，
#     不写 icon= 也行。
#   - af-84/sdljy/OpenBOR 能显示，全因固件预注册了对应的 .lnk。
#
#   本脚本产物同时包含：
#     pkg/                      资源文件夹（部署到 $HOME/apps/epubreader/）
#       epubreader-sdl-1.2      二进制（带 -sdl-1.2 后缀，避免 FAT 撞名）
#       epubreader.dge          启动器 shell 脚本（exec 指向它）
#       epubreader.png          图标（与 .dge 同目录，自动命中）
#       font.ttf                CJK 字体
#       lib/                    递归打包的 8 个依赖库
#     pkg/sections/applications/epubreader.lnk   ← 真正的注册文件！
#                                                       （部署到 $HOME/.gmenunx/sections/applications/）
#
# 部署（GKD Mini / IUX 实测路径）：
#   设备 U 盘模式挂载点为 G:，设备上对应绝对前缀 = /media/roms。
#   1) 资源：把整个 pkg/ 放到  G:/apps/epubreader/
#      （设备内即 /media/roms/apps/epubreader/）。本文件夹含
#      epubreader.dge(启动器) + epubreader-sdl-1.2(二进制) + epubreader.png
#      + font.ttf + lib/(8个依赖库)，以及可选的 epubreader(无扩展名元数据)。
#   2) 注册：IUX 不扫 .gmenunx（U 盘上也没有），真正注册靠设备端
#      「应用程序扫描」——在主菜单选中任意一个已显示的应用，按 SELECT
#      进入编辑菜单，选「应用程序扫描」，它会扫描 G:/apps/ 下的新文件夹
#      （识别 .dge 启动器或同名元数据文件）并加入菜单。扫描/重启后，
#      EPUB Reader 即出现在「应用程序」或「独立游戏」栏目。
#   3) 备选（文件管理器合并法）：把本脚本同时产出的 pkg/sections/ 按
#      Iceway 风格合并进系统目录（需设备端文件管理器操作），可绕过扫描。
#
# 注：IUX_HOME 仅用于生成 pkg/sections/applications/epubreader.lnk（备选方案）。
#   本机前缀已实证为 /media/roms（见 af-84 元数据 exec），故默认即此值；
#   若换设备前缀不同，可用 `IUX_HOME=/其它前缀 ./package.sh` 覆盖。
# ============================================================
set -e
cd "$(dirname "$0")"

IUX_HOME="${IUX_HOME:-/media/roms}"

echo "==> 编译 ..."
make -f Makefile.gcw0 clean
make -f Makefile.gcw0

rm -rf pkg && mkdir -p pkg
# 二进制带 -sdl-1.2 后缀: 避免与「无扩展名元数据文件」在 FAT 上撞名(参考 af-84)
cp epubreader pkg/epubreader-sdl-1.2
# 图标: 必须是一张合法 PNG(损坏/过小的 png 会让启动器跳过本 app)
if [ -f icon.png ]; then
  cp icon.png pkg/epubreader.png
  echo "==> 已包含 epubreader.png (图标)"
else
  echo "!! 警告: 未找到 icon.png，启动器可能因缺图标而跳过本 app"
fi

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
      # ★ GKD mini 修复: 设备 /usr/lib 已自带全部依赖(SDL/freetype/png/jpeg/iconv/z),
      #   且都是 fp32 兼容版本。我们自带的工具链版本是 fp64, 打包进去会导致 SIGILL。
      #   因此除基库外, 下列库也一律用设备系统的, 不打包。
      case "$lib" in
        libSDL-1.2.so*|libSDL.so*|libSDL_ttf*|libSDL_image*|libts*|libpthread*|libc.so*|libm.so*|libgcc_s.so*|ld-uClibc*)
          echo "    (跳过 $lib: 使用设备自带基库)"; continue ;;
      esac
      # 下列库设备 /usr/lib 也有, 用系统的(fp32兼容), 不打包
      case "$lib" in
        libfreetype*|libpng*|libjpeg*|libiconv*|libz*|libdrm.so*|libudev.so*)
          echo "    (跳过 $lib: 设备系统库)"; continue ;;
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
# 启动器: .dge 文件（GMenuNX 的 .lnk 里 exec 指向它）
# 固定用 fbcon + /dev/fb0 (设备帧缓冲); 把运行日志写到 app 目录 run.log 便于排查。
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
./epubreader-sdl-1.2
echo "=== exit code: $? ==="
EOF
chmod +x pkg/epubreader.dge
echo "==> 已生成启动器 pkg/epubreader.dge"

# ============================================================
# ★ 关键：GMenuNX 的注册文件 .lnk ★
#   放在 sections/<分类>/ 下，启动器只读这里。applications 分类由启动器
#   强制内置（菜单显示名 "apps"），无需新建分类、无需补分类图标。
#   exec 必须是【存在的绝对路径】——指向我们的 .dge 启动器。
#   icon 省略：启动器会自动回退到 dir_name(exec)/<exec基名>.png
#              （即 epubreader.png，已与 .dge 同目录）。
# ============================================================
mkdir -p "pkg/sections/applications"
cat > "pkg/sections/applications/epubreader.lnk" <<EOF
title=电子书
description=EPUB 电子书阅读器 (SDL1.2, MIPS)
icon=$IUX_HOME/apps/epubreader/epubreader.png
exec=$IUX_HOME/apps/epubreader/epubreader.dge
selectorbrowser=false
EOF
echo "==> 已生成注册文件 pkg/sections/applications/epubreader.lnk (exec 前缀=$IUX_HOME)"

if ! command -v mksquashfs >/dev/null 2>&1; then
  echo "!! 缺少 mksquashfs，跳过 OPK（不影响 iuxui 部署）"
else
  rm -f EPUBReader.opk
  mksquashfs pkg EPUBReader.opk -all-root -noappend -no-progress
  echo "==> 已生成 EPUBReader.opk (标准 OpenDingux 用, 可选)"
fi

echo
echo "==> 产物: pkg/ (资源) + pkg/sections/applications/epubreader.lnk (注册)"
echo "    部署到 GDK mini："
echo "    1) 资源: pkg/ 改名为 epubreader → 复制到 G:/apps/epubreader/"
echo "           (即设备 \$HOME/apps/epubreader/)"
echo "    2) 注册: 把 pkg/sections/ 合并进 G:/.gmenunx/sections/"
echo "           (即设备 \$HOME/.gmenunx/sections/)，applications 下出现 epubreader.lnk"
echo "    3) 重启 → 菜单「apps」标签即见 EPUB Reader"
