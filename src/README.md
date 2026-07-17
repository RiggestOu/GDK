# EPUB Reader for GDK mini (MIPS / uClibc / SDL1.2)

一个为 **GDK mini 掌机**（iuxui 固件，君正 MIPS，uClibc 运行时，SDL 1.2）编写的
**EPUB 3.0 / EPUB 2.0 阅读器** MVP。用 OpenDingux gcw0 交叉工具链编译，产物 `.opk`
放进 SD 卡 `APPS` 目录即可在机器上运行。

> 为什么不用 KOReader：KOReader 官方不发布 MIPS 版本（只有 ARM/x86），而 GDK mini
> 实测是 **MIPS 32-bit little-endian + uClibc**，所以只能自己写。

## 目标设备
- 架构：MIPS 32-bit LE，uClibc（`libc.so.0`）
- 图形：SDL 1.2 + SDL_ttf + SDL_image
- 屏幕：默认 **320×240**（如你的机器不同，改 `render.h` 里的 `SCREEN_W/SCREEN_H`）
- 安装位置：SD 卡（`H:` 盘）的 `APPS` 目录

## 目录结构
```
src/
  main.c          文件浏览 / 目录 / 翻页 / 进度 / 输入(键盘+摇杆)
  epub.c          解析 OPF / spine / nav(EPUB3) 或 NCX(EPUB2) / 文本提取
  zip.c           用 zlib 解 EPUB(ZIP)：存储 / deflate 均可
  render.c        SDL1.2 文本折行分页 + CJK 字体 + 进度条
  util.c          字符串 / XML 标签 / 实体解码 辅助函数
  render.h epub.h zip.h util.h
  Makefile.gcw0   交叉编译（mipsel-gcw0-linux-uclibc）
  package.sh      打包成 EPUBReader.opk (squashfs)
  EPUBReader.gcw0.desktop  启动器
setup-ubuntu.sh  在 WSL2 Ubuntu 里一键装好工具链 + squashfs-tools
validate-opk.sh  编译后校验 MIPS/uClibc/SDL 链接
```

## 在 WSL2 Ubuntu 中构建
```bash
# 1) 一次性安装工具链（约 153MB 下载）
bash setup-ubuntu.sh

# 2) 编译
cd src
make -f Makefile.gcw0

# 3) 打包（需要一款 CJK 字体，见下）
#    把一款开源中文字体(文泉驿/Noto CJK)重命名为 font.ttf 放到 src/ 再执行：
./package.sh
```
产物：`src/EPUBReader.opk`。

## CJK 字体（必须）
阅读器需要一款含中文的 TTF/OTF。把字体文件重命名为 **`font.ttf`** 放到 `src/`
（或在运行时用 `epubreader --font /路径/字体.ttf` 指定）。否则中文会是方块。

## 部署到 GDK mini
把 `EPUBReader.opk` 复制到 SD 卡（`H:` 盘）的 `APPS` 目录，在机器菜单里即可看到
「EPUB Reader」。电子书（`.epub`）放到 SD 卡的任意目录，程序启动后会扫描并列出。

## 操作
- 方向键 / WASD：上下选择、左右翻章
- A / 回车 / 空格：打开、进入目录
- B / Esc / Q：返回、退出
- 在阅读时按 **A** 打开目录（Table of Contents）
- 进度会自动保存到电子书同目录的 `<书名>.progress`

## 校验（编译后，仍在 Ubuntu 里）
```bash
cd src
bash validate-opk.sh
```
会检查二进制确实是 **MIPS** 架构、且链接了 `libSDL / SDL_ttf / SDL_image / libz / uClibc`。

## 已知限制（MVP）
- **图片尚未渲染**（EPUB 内 `<img>` 暂以空白跳过），下一版加入 SDL_image 显示。
- 仅做最基础排版：按宽度折行 + CJK 逐字断行，未实现完整 CSS 样式。
- 真机未实测（需在 GDK mini 上跑）；逻辑已尽量按设备约束编写，请在机器上验证后反馈。
- 屏幕分辨率默认 320×240，若不符请改 `render.h`。

## 本地快速排错
若 `make` 报错，多半是工具链路径问题：`setup-ubuntu.sh` 默认把工具链装到
`/opt/gcw0-toolchain`；如你改了位置，编辑 `Makefile.gcw0` 顶部的 `TOOLCHAIN=` 即可。
