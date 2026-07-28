#!/bin/bash
CC="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-gcc"
OD="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump"
cat > /tmp/t.c <<'EOF'
int font_sizes[4] = {12,14,18,22};
int pick(int i){ return font_sizes[i]; }
EOF
for FLAGS in "-O2 -march=mips32r2 -mfp32" "-O0 -march=mips32r2 -mfp32" "-O2 -march=mips32r2 -mfp32 -fno-tree-vectorize"; do
  "$CC" $FLAGS -c /tmp/t.c -o /tmp/t.o 2>/dev/null
  echo "=== [$FLAGS] ==="
  "$OD" -d /tmp/t.o | sed -n '/<pick>:/,/^$/p'
done
echo "=== gcc 全部 -m 选项里含 lx/swap/xburst/mxu 的 ==="
"$CC" -Q --help=target 2>&1 | grep -iE "lx|mxu|xburst|swap|ingenic" | head
