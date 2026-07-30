#!/bin/sh
# KeyTest 启动脚本（OPK 内由 opkrun 调用）
DIR=$(dirname "$0")
cd "$DIR"
export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0
export SDL_NOMOUSE=1

LOGFILE="/media/roms/apps/KeyTest.log"
: > "$LOGFILE" 2>/dev/null && exec 1>>"$LOGFILE" 2>&1 || exec 1>>/tmp/KeyTest.log 2>&1
echo "=== KeyTest launch $(date) ==="
./keytest
echo "=== exit code: $? ==="
