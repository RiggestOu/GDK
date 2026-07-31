EPUB Reader for GDK mini —— 交付说明
=====================================

版本：v1.1.2（对外固定版本号；除非用户主动要求，不再变更）

构建环境：Ingenic JZ47xx (XBurst MIPS) / OpenDingux / uClibc
图形栈：SDL 1.2（帧缓冲 fbcon）
编译：交叉工具链 -O0（规避 XBurst MXU 私有指令 lxw → SIGILL）
MXU 校验：scan_mxu.py 扫描 .text 段，lxw = 0 条，干净。

本包内容：
  EPUBReader.opk           —— OPK 安装包（squashfs，含 epubreader 二进制 + launch.sh + icon.png + default.gcw0.desktop）
  default.gcw0.desktop     —— 桌面项元数据（Name=EPUB Reader v1.1.2）
  icon.png                 —— 64x64 应用图标
  launch.sh                —— OPK 内启动脚本（设 SDL 环境变量、自动注册到 Esoteric 菜单）
  VERSION                  —— 版本真值（v1.1.2）

安装方式（任选其一）：
  A. 将 EPUBReader.opk 拷贝到 SD 卡（设备 /media/roms/apps/），在 Esoteric 中打开即自动注册到「应用程序」。
  B. 手动注册：在 $HOME/.esoteric/sections/applications/ 下放置链接文件，exec 指向
     /usr/bin/opkrun，params=-m default.gcw0.desktop "/media/roms/apps/EPUBReader.opk"。

按键模型（KeyTest 20 键权威实测，evdev 直读方案已废弃，全部走 SDL）：
  十字键    → JOYHAT 上/左/下/右
  A/B/X/Y   → JOYBTN b1/b0/b2/b3
  L1/R1     → JOYBTN b6/b7
  START/SEL → JOYBTN b5/b4
  L2/R2     → 键盘 sym=280/281（PAGEUP/PAGEDOWN，无 JOYBTN）
  音量+/-   → 键盘 sym=270/269
  圆3/圆4   → 键盘 sym=279/278
  圆1/圆2   → 与 A/B 完全相同的硬件复制键（事件不可区分）

功能绑定（v1.4 实现）：
  A=选择/打开  B=返回  X=书签  Y=菜单  SELECT=返回
  START=息屏/挂起（抬起且无组合时）
  L1+L2 或 R1+R2 = 图片缩放切换（A_PIC）
  L2+START 或 L1+START = 强制退出（A_QUIT_FORCE）
  L1/L2 默认绑定为翻页（下一页/上一页），圆1/圆2 等价于 A/B。
  （具体功能以设备实机表现为准；可用 KeyTest.opk 复核各键 SDL 事件。）

已知限制：
  - 圆1/圆2 与 A/B 硬件不可区分，仅作 A/B 副本。
  - 设备 WiFi 疑似无硬件；注册/部署走 SD 卡（U 盘模式）完成。
  - 应用图标（externalAppPath）为该机型悬案，暂不影响使用。
