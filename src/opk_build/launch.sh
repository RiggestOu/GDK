#!/bin/sh
# Epub阅读器 启动脚本
# ★ 2026-07-30 图标问题最终定案（读设备 rootfs + esoteric 源码实证）：
#   本机菜单（Esoteric/350teric fork）条目 = sections 目录下的链接文件（title=/icon=/exec= 格式），
#   图标显示 = 链接文件 icon= 指向的绝对路径 PNG（与 sdl-palmsk 的
#   icon=/media/roms/apps/sdl-palmsk/icon-32.png 完全一致，已实证可显）。
#   ★★ 关键：esoteric 实际读取的 sections 目录是 $HOME/.esoteric/sections（源码 menu.cpp:56
#      home_path("sections")；frontend_start 从 /etc/passwd 取 HOME，本机=/usr/local/home）。
#      之前误写到 /usr/share/n/sections（出厂种子，运行时不读）才是图标一直不显的真因！
#   因此本脚本每次启动：扫描活目录 $HOME/.esoteric/sections 清理所有 EPUB 残留链接 →
#   写入规范链接（自愈）。必须重启设备菜单才会重读 sections。
DIR=$(dirname "$0")
cd "$DIR"
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
export SDL_NOMOUSE=1

# 日志写到 SD 卡（U 盘模式下 PC 可见，便于排查）；SD 不可写时退回 $HOME
LOGFILE="/media/roms/apps/EPUBReader/EPUBReader.log"
mkdir -p "$HOME/.epubreader" 2>/dev/null
: >> "$LOGFILE" 2>/dev/null && exec 1>>"$LOGFILE" 2>&1 || exec 1>>"$HOME/.epubreader/run.log" 2>&1
echo "=== EPUBReader launch $(date) ==="

# ===== 菜单注册自愈（真实机制：esoteric 链接文件，目录=$HOME/.esoteric/sections） =====
# $HOME 在本机 = /usr/local/home（由 frontend_start 从 /etc/passwd 取得）。
# 用 $HOME 推导活目录，失败时回退到已知绝对路径。
ESO_HOME="$HOME/.esoteric"
[ -d "$ESO_HOME" ] || ESO_HOME="/usr/local/home/.esoteric"
SEC_DIR="$ESO_HOME/sections"
LINK_FILE="$SEC_DIR/applications/epubreader"
echo "----- MENU REG -----"
echo "  [env] HOME=$HOME  SEC_DIR=$SEC_DIR"
# 1) 清理：扫描活目录所有链接，删除任何指向 EPUBReader 的残留（旧无图标条目/重复条目）
if [ -d "$SEC_DIR" ]; then
    for f in "$SEC_DIR"/*/*; do
        [ -f "$f" ] || continue
        if grep -qi 'epubreader' "$f" 2>/dev/null || grep -q 'apps/EPUBReader' "$f" 2>/dev/null; then
            echo "  [clean] 删除残留链接: $f"
            rm -f "$f"
        fi
    done
    # 2) 写入规范链接文件（与 sdl-palmsk 同格式：icon= 绝对路径 PNG）
    if [ -d "$SEC_DIR/applications" ] && \
       printf 'title=Epub阅读器\ndescription=EPUB 3.0 reader v1.2.0\nicon=/media/roms/apps/EPUBReader/epubreader_icon.png\nexec=/usr/bin/opkrun\nparams=-m default.gcw0.desktop "/media/roms/apps/EPUBReader/EPUBReader.opk"\nconsoleapp=false\nselectorbrowser=false\n' > "$LINK_FILE" 2>/dev/null; then
        echo "  [reg] 已写入 $LINK_FILE:"
        sed 's/^/    /' "$LINK_FILE"
    else
        echo "  [reg] 写入失败（rootfs 只读？）"
    fi
    sync
    echo "  [hint] 注册已写入 $LINK_FILE（活目录，esoteric 实际读取处）"
    echo "  [hint] 必须【重启设备】(断电重启或退出 Esoteric 回 shell 再启动)，菜单才会重新读取 sections 并显示图标"
    echo "  [hint] 重启后请在菜单最右侧【apps】分区找到“Epub阅读器”条目"
else
    echo "  [reg] $SEC_DIR 不存在，跳过"
fi
echo "----- MENU REG END -----"

./epubreader
echo "=== exit code: $? ==="
