#!/bin/bash
# 编译按键测试器 keytest（工具链同 epubreader；⛔必须 -O0，MXU 铁律）
set -e
TC="$HOME/gcw0-toolchain/usr"
CC="$TC/bin/mipsel-gcw0-linux-uclibc-gcc"
SR="$TC/mipsel-gcw0-linux-uclibc/sysroot"
cd "$(dirname "$0")"
rm -f keytest.o keytest
CFLAGS="-O0 -Wall -Wno-unused -march=mips32r2 -mtune=mips32r2 -mhard-float -mfp32 -I$SR/usr/include -I$SR/usr/include/SDL"
"$CC" $CFLAGS -c keytest.c -o keytest.o
"$CC" -L"$SR/usr/lib" -o keytest keytest.o -lSDL -lSDL_ttf -lm
ls -la keytest
echo BUILD_OK
