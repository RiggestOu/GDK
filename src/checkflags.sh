#!/bin/bash
CC="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-gcc"
echo "== 与 MXU/XBurst/lx 相关的目标选项 =="
"$CC" --target-help 2>&1 | grep -iE "mxu|xburst|lx|ingenic" | head -20
echo "== 默认 march 配置 =="
"$CC" -v 2>&1 | grep -oE "\-\-with-arch[^ ]*|--with-tune[^ ]*"
echo "== 试编译对照 =="
cat > /tmp/t.c <<'EOF'
int font_sizes[4] = {12,14,18,22};
int pick(int i){ return font_sizes[i]; }
EOF
for FLAGS in "-O2 -march=mips32r2 -mfp32" "-O2 -march=mips32r2 -mfp32 -mno-mxu" "-O2 -march=mips32" "-O2 -march=mips32r2 -mfp32 -mno-xburst"; do
  if "$CC" $FLAGS -c /tmp/t.c -o /tmp/t.o 2>/tmp/t.err; then
    N=$("$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump" -d /tmp/t.o | grep -cE "\blxw?\b|\bs32|\blxh|\blxb")
    echo "[$FLAGS] 编译OK, lx/MXU指令数=$N"
  else
    echo "[$FLAGS] 编译失败: $(head -1 /tmp/t.err)"
  fi
done
