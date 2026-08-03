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
