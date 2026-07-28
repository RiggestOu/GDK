#!/bin/bash
# 反汇编 epubreader 并定位关键函数
OD="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump"
BIN="/mnt/e/WorkBuddy/GDKmini/GDK/src/epubreader"
OUT="/mnt/e/WorkBuddy/GDKmini/ep.dis"
if [ ! -x "$OD" ]; then
  echo "objdump 不存在: $OD"
  ls "$HOME/gcw0-toolchain/usr/bin/" | grep -i objdump
  exit 1
fi
"$OD" -d "$BIN" > "$OUT" 2>&1
echo "RC=$? 行数=$(wc -l < "$OUT")"
grep -n "<ui_set_font_size>:\|<apply_config>:\|<install_crash_handler>:\|<crash_handler>:\|<main>:" "$OUT"
