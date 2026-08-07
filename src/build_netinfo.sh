#!/bin/bash
# netinfo 交叉编译脚本（工具链同 build_local.sh，复用其路径）
set -e
TC="$HOME/gcw0-toolchain/usr"
CC="$TC/bin/mipsel-gcw0-linux-uclibc-gcc"
SR="$TC/mipsel-gcw0-linux-uclibc/sysroot"
cd "$(dirname "$0")"
rm -f netinfo netinfo.o
CFLAGS="-O0 -Wall -Wno-unused -march=mips32r2 -mtune=mips32r2 -mhard-float -mfp32 -I$SR/usr/include -I$SR/usr/include/SDL"
echo "CC netinfo.c"
"$CC" $CFLAGS -c netinfo.c -o netinfo.o
echo "LINK netinfo"
"$CC" -L"$SR/usr/lib" -o netinfo netinfo.o -lSDL -lpthread -lm
ls -la netinfo
echo BUILD_OK
