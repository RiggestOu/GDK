#!/bin/bash
# 本地 WSL 交叉编译脚本（工具链在 ~/gcw0-toolchain）
set -e
TC="$HOME/gcw0-toolchain/usr"
CC="$TC/bin/mipsel-gcw0-linux-uclibc-gcc"
SR="$TC/mipsel-gcw0-linux-uclibc/sysroot"
cd "$(dirname "$0")"
rm -f ./*.o epubreader
CFLAGS="-O2 -Wall -Wno-unused -march=mips32r2 -mtune=mips32r2 -mhard-float -mfp32 -I$SR/usr/include -I$SR/usr/include/SDL"
for f in main epub zip render util; do
    echo "CC $f.c"
    "$CC" $CFLAGS -c "$f.c" -o "$f.o"
done
echo "LINK epubreader"
"$CC" -L"$SR/usr/lib" -Wl,-rpath,'$ORIGIN/lib:/media/roms/apps/epubreader/lib' \
    -o epubreader main.o epub.o zip.o render.o util.o \
    -lSDL -lSDL_ttf -lSDL_image -lz -lpthread
ls -la epubreader
echo BUILD_OK
