#!/bin/sh
# EPUBReader 图标诊断脚本 - 在 GDK mini 本地终端运行
# 用法: sh /media/roms/diag.sh
# 运行后结果写入 SD 卡 diag.log，拔卡插回电脑即可分析

OUT=/media/roms/diag.log
if ! touch "$OUT" 2>/dev/null; then OUT=/tmp/diag.log; fi

{
echo "========== EPUBReader 图标诊断 $(date) =========="
echo

echo "## [1] 网络监听端口 (/proc/net/tcp, 16进制端口)"
awk 'NR>1{print $2}' /proc/net/tcp 2>/dev/null | sort -u
echo

echo "## [2] 挂载信息 (SD卡相关)"
mount 2>/dev/null | grep -iE 'roms|sdcard|mmcblk|/media' 
echo

echo "## [3] esoteric / 前端进程"
ps w 2>/dev/null | grep -iE 'esoteric|n |gmenu|frontend' | grep -v grep
echo

echo "## [4] .esoteric 配置目录树"
ESO_HOME=$(find /usr/local/home /home -maxdepth 4 -type d -name '.esoteric' 2>/dev/null | head -1)
echo "ESO_HOME=$ESO_HOME"
if [ -n "$ESO_HOME" ]; then find "$ESO_HOME" -maxdepth 3 2>/dev/null | head -100; fi
echo

echo "## [5] sections/applications 全部条目内容"
if [ -n "$ESO_HOME" ]; then
  for f in "$ESO_HOME"/sections/applications/*; do
    echo "----- $f -----"; cat "$f" 2>/dev/null
  done
fi
echo

echo "## [6] esoteric 扫描目录/设置配置"
if [ -n "$ESO_HOME" ]; then
  find "$ESO_HOME" -maxdepth 2 \( -iname '*.cfg' -o -iname '*.ini' -o -iname '*.conf' -o -iname 'settings*' \) 2>/dev/null | while read cf; do
    echo "----- $cf -----"; cat "$cf" 2>/dev/null
  done
fi
echo

echo "## [7] EPUBReader 部署文件夹 (设备端)"
ls -la /media/roms/apps/EPUBReader/ 2>/dev/null
echo "--- desktop 内容 ---"
cat /media/roms/apps/EPUBReader/default.gcw0.desktop 2>/dev/null
echo

echo "## [8] esoteric 二进制里图标/扫描相关字符串"
ESOBIN=$(find /usr/share/n /usr/bin /usr/local/bin /usr/games -name 'esoteric' -type f 2>/dev/null | head -1)
echo "ESOBIN=$ESOBIN"
if [ -n "$ESOBIN" ]; then
  strings "$ESOBIN" 2>/dev/null | grep -iE 'icon|\.desktop|skin|/media|/apps|/roms|categories|exec|png' | head -100
fi
echo

echo "## [9] 皮肤目录位置"
find "$ESO_HOME" /usr -type d -iname '*skin*' 2>/dev/null | head
echo

echo "## [10] 其它应用如何显图标 (参照 apps/ 下各条目)"
ls -la /media/roms/apps/ 2>/dev/null
for d in /media/roms/apps/*/; do
  echo "--- $d ---"
  ls -la "$d" 2>/dev/null
  cat "$d"default.gcw0.desktop 2>/dev/null
done
echo

echo "========== 诊断完成 =========="
echo "输出文件: $OUT"
} > "$OUT" 2>&1

sync
echo "诊断完成 -> $OUT"
