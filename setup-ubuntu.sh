#!/usr/bin/env bash
# ============================================================
# 在 WSL2 Ubuntu 内运行一次，安装 GDK mini 交叉编译工具链。
# 用法:  bash setup-ubuntu.sh
# ============================================================
set -e

TOOLCHAIN_URL="https://github.com/OpenDingux/buildroot/releases/download/od-2022.09.22/opendingux-gcw0-toolchain.2022-09.22.tar.xz"
INSTALL_DIR="/opt/gcw0-toolchain"

echo "==> 更新 apt 并安装打包工具 (squashfs-tools) ..."
sudo apt-get update -y
sudo apt-get install -y squashfs-tools wget xz-utils

if [ -d "$INSTALL_DIR" ]; then
  echo "==> 工具链已存在于 $INSTALL_DIR，跳过下载。"
else
  echo "==> 下载 OpenDingux gcw0 工具链 (约 153MB) ..."
  wget -O /tmp/gcw0-tc.tar.xz "$TOOLCHAIN_URL"
  echo "==> 解压到 /opt ..."
  sudo tar -xJf /tmp/gcw0-tc.tar.xz -C /opt
  sudo chmod -R a+rX /opt/gcw0-toolchain
  rm -f /tmp/gcw0-tc.tar.xz
fi

echo
echo "工具链就绪: $INSTALL_DIR"
echo "交叉编译器:"
ls "$INSTALL_DIR/bin" | grep gcc || true
echo
echo "接下来编译阅读器:"
echo "  cd src"
echo "  make -f Makefile.gcw0      # 仅编译 (用于本地测试链接)"
echo "  ./package.sh               # 打包成 EPUBReader.opk"
