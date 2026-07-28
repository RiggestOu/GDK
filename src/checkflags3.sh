#!/bin/bash
CC="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-gcc"
OD="$HOME/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump"
cat > /tmp/t.c <<'EOF'
int font_sizes[4] = {12,14,18,22};
int pick(int i){ return font_sizes[i]; }
unsigned char buf[256];
int sum(int n){ int s=0; for(int i=0;i<n;i++) s+=buf[i]*i; return s; }
EOF
for FLAGS in "-O1 -march=mips32r2 -mfp32" "-Os -march=mips32r2 -mfp32" "-O2 -march=mips32r2 -mfp32 -fno-ivopts" ; do
  "$CC" $FLAGS -c /tmp/t.c -o /tmp/t.o 2>/dev/null
  N=$("$OD" -d /tmp/t.o | grep -cE "	(lxw|lxh|lxb|lxhu|lxbu|s32[a-z0-9]*|d32|q8|q16)	")
  echo "[$FLAGS] XBurst特有指令数=$N"
done
