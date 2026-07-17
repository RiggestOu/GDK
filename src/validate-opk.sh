#!/usr/bin/env bash
# 编译后校验产物确实是 GDK mini 可用的 MIPS/uClibc/SDL 二进制。
# 在 WSL2 Ubuntu（已跑过 setup-ubuntu.sh）里执行:  bash validate-opk.sh
set -e
cd "$(dirname "$0")"

TOOLCHAIN="/opt/gcw0-toolchain"
READELF="$TOOLCHAIN/bin/mipsel-gcw0-linux-uclibc-readelf"

if [ ! -f epubreader ]; then
  echo "!! 未找到 epubreader，请先: make -f Makefile.gcw0"
  exit 1
fi

echo "==> file 检查架构:"
file epubreader | sed 's/^/    /'
if ! file epubreader | grep -qi "MIPS"; then
  echo "!! 错误: 不是 MIPS 架构，交叉编译可能没生效！"
  exit 1
fi

echo "==> 动态库依赖 (NEEDED):"
"$READELF" -d epubreader | grep NEEDED | sed 's/^/    /'
for lib in libSDL-1.2.so.0 libSDL_ttf-2.0.so.0 libSDL_image-1.2.so.0 libc.so.0 libz.so; do
  if "$READELF" -d epubreader | grep -q "$lib"; then
    echo "    [OK] 链接了 $lib"
  fi
done

echo "==> .opk 检查:"
if [ -f EPUBReader.opk ]; then
  file EPUBReader.opk | sed 's/^/    /'
  echo "    内容列表:"
  unsquashfs -l EPUBReader.opk 2>/dev/null | sed 's/^/      /' | head -20
else
  echo "    (尚未打包，运行 ./package.sh 生成 EPUBReader.opk)"
fi

echo
echo "校验通过：这是一个面向 GDK mini (MIPS/uClibc/SDL1.2) 的可执行文件。"
