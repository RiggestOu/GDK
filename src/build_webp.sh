#!/bin/bash
# 交叉编译 libwebp 解码部分为静态库（⛔ 必须 -O0，防 XBurst MXU 指令）
set -e
TC="$HOME/gcw0-toolchain/usr"
CC="$TC/bin/mipsel-gcw0-linux-uclibc-gcc"
AR="$TC/bin/mipsel-gcw0-linux-uclibc-ar"
SRC="$HOME/libwebp-src"
OUT="$HOME/libwebp-build"

if [ ! -d "$SRC" ]; then
    if [ -d /tmp/libwebp ]; then mv /tmp/libwebp "$SRC"; else echo "缺少 libwebp 源码"; exit 1; fi
fi

mkdir -p "$OUT"
cd "$SRC"
CFLAGS="-O0 -march=mips32r2 -mtune=mips32r2 -mhard-float -mfp32 -I. -Isrc -DWEBP_DISABLE_STATS"
objs=()
# 只编译解码所需：dec/ dsp/ utils/（排除编码器 *_enc.c 与 ssim）
for f in src/dec/*.c src/dsp/*.c src/utils/*.c; do
    base=$(basename "$f")
    case "$base" in
        *_enc.c|ssim*.c|*_csp_enc.c) continue;;
    esac
    o="$OUT/${base%.c}.o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        echo "CC $base"
        "$CC" $CFLAGS -c "$f" -o "$o"
    fi
    objs+=("$o")
done
rm -f "$OUT/libwebpdecode.a"
"$AR" rcs "$OUT/libwebpdecode.a" "${objs[@]}"
ls -la "$OUT/libwebpdecode.a"
echo WEBP_BUILD_OK
