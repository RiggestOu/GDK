#!/bin/sh
# GDK NET INFO 启动脚本（对齐 Epub阅读器：自愈注册菜单图标 + 启动二进制）
# ★ 图标机制（读设备 esoteric 源码实证）：
#   菜单条目 = $HOME/.esoteric/sections/<分区>/<条目> 链接文件（title=/icon=/exec= 格式），
#   图标显示 = 链接文件 icon= 指向的绝对路径 PNG。
#   本脚本每次启动：清理残留链接 -> 写入规范链接（自愈）。必须重启设备菜单才会重读并显示图标。
DIR=$(dirname "$0")
cd "$DIR"
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
export SDL_NOMOUSE=1

LOGFILE="/media/roms/apps/netinfo/netinfo.log"
: >> "$LOGFILE" 2>/dev/null && exec 1>>"$LOGFILE" 2>&1 || exec 1>>"/tmp/netinfo_launch.log" 2>&1
echo "=== netinfo launch $(date) ==="

# ===== 菜单注册自愈（真实机制：esoteric 链接文件，目录=$HOME/.esoteric/sections） =====
ESO_HOME="$HOME/.esoteric"
[ -d "$ESO_HOME" ] || ESO_HOME="/usr/local/home/.esoteric"
SEC_DIR="$ESO_HOME/sections"
LINK_FILE="$SEC_DIR/applications/netinfo"
echo "----- MENU REG -----"
echo "  [env] HOME=$HOME  SEC_DIR=$SEC_DIR"
if [ -d "$SEC_DIR" ]; then
    for f in "$SEC_DIR"/*/*; do
        [ -f "$f" ] || continue
        if grep -qi 'netinfo' "$f" 2>/dev/null || grep -q 'apps/netinfo' "$f" 2>/dev/null; then
            echo "  [clean] 删除残留链接: $f"
            rm -f "$f"
        fi
    done
    # ★ 图标真因修复：设备上能正常显示图标的第三方 app，icon= 都指向 esoteric 缓存目录
    #   (/usr/local/home/.esoteric/cache/images/<名>/xxx.png)，而非 SD 卡 /media/roms 绝对路径。
    #   故这里把图标复制进缓存目录，并让 icon= 指向缓存副本；复制失败才回退 SD 路径。
    ICON_SRC="/media/roms/apps/netinfo/netinfo_icon.png"
    ICON_CACHE_DIR="$ESO_HOME/cache/images/netinfo"
    ICON_DST="$ICON_CACHE_DIR/netinfo_icon.png"
    mkdir -p "$ICON_CACHE_DIR" 2>/dev/null
    if cp -f "$ICON_SRC" "$ICON_DST" 2>/dev/null; then
        ICON_USE="$ICON_DST"
        echo "  [icon] 已复制图标 -> $ICON_DST"
    else
        ICON_USE="$ICON_SRC"
        echo "  [icon] 复制到缓存失败, 回退 SD 路径: $ICON_SRC"
    fi
    if [ -d "$SEC_DIR/applications" ] && \
       printf 'title=GDK NET INFO\ndescription=WiFi info + telnet\nicon=%s\nexec=/media/roms/apps/netinfo/netinfo\nconsoleapp=false\nselectorBrowser=false\n' "$ICON_USE" > "$LINK_FILE" 2>/dev/null; then
        chmod 0666 "$LINK_FILE" 2>/dev/null
        echo "  [reg] 已写入 $LINK_FILE:"
        sed 's/^/    /' "$LINK_FILE"
    else
        echo "  [reg] 写入失败（rootfs 只读？）"
    fi
    sync
    echo "  [hint] 重启设备后菜单（apps 分区）显示 GDK NET INFO 图标"
else
    echo "  [reg] $SEC_DIR 不存在，跳过"
fi
echo "----- MENU REG END -----"

./netinfo
echo "=== exit code: $? ==="
