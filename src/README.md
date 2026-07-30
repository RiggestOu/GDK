# EPUB Reader for GDK mini — 使用与配置文档（v1.1）

一个为 **GDK mini 掌机**（iuxui 固件，君正 Ingenic MIPS，uClibc 运行时，SDL 1.2）
编写的 **EPUB 3.0 / EPUB 2.0 阅读器**。用 OpenDingux gcw0 交叉工具链编译，产物
`EPUBReader.opk` 复制到 SD 卡即可在机器上运行。

> 为什么不用 KOReader：KOReader 官方不发布 MIPS 版本（只有 ARM/x86），而 GDK mini 是
> **MIPS 32-bit little-endian + uClibc**，所以自行编写。

---

## 版本历史

| 版本 | 说明 |
|------|------|
| v1.0 | 基础阅读器：目录/书签/配色/字号/翻页/进度保存、WebP 图片显示 |
| **v1.1** | **新增 7 项功能（见下），改为 OPK 单包分发，使用设备系统字体，配置/快捷键落盘到可写目录** |

### v1.1 新增功能
1. **START 息屏/唤醒**：进入 App 后任意界面按 START，自动保存进度并息屏；息屏后**只有 START 能恢复画面**，其它按键无效。
2. **音量键调亮度**（详见「操作」）：音量± 以 5% 步进，配合肩键可大幅调节。
3. **右上角电量**：实时显示剩余电量百分比（读 `/sys/class/power_supply/*/capacity`）。
4. **顶部居中时间**：24 小时制「时:分」。
5. **底部居中亮度**：实时显示当前亮度百分比。
6. **图片格式兼容**：除 WebP 外，新增支持 **PNG / JPG / GIF / BMP / TIFF** 等 EPUB 可嵌入格式，缩放查看器同步兼容（别人发来的 epub 图片不再空白）。
7. **自定义快捷键**：菜单内可重绑定 7 个逻辑动作，自动冲突检测；SELECT 一键恢复默认；可导出/导入配置。

---

## 目标设备 / 运行环境
- 架构：MIPS 32-bit LE，uClibc（`libc.so.0`）
- 图形：SDL 1.2 + SDL_ttf + SDL_image（均使用设备系统库，无需自带）
- 字体：**设备系统思源黑体**（`/usr/share/fonts/SourceHanSans-Regular-04.ttf`），无需再放 `font.ttf`
- 屏幕：默认 **320×240**（如不符改 `render.h` 的 `SCREEN_W/SCREEN_H`）
- 分发格式：**OPK**（squashfs，只读挂载；配置与快捷键会自动写到可写目录，见下）

---

## 实测物理键码（用别人的 OPK 测得，已写入源码）
| 按键 | SDL keysym（十进制） | 十六进制 |
|------|----------------------|----------|
| 音量 + | 78 | 0x4E |
| 音量 − | 74 | 0x4A |
| KEY_107 | 107 | 0x6B |
| MENU | 102 | 0x66 |

> `KEY_107` 与 `MENU` 两个键**暂未分配功能**，已记录备用，后续版本可扩展。

---

## 操作

### 阅读 / 浏览通用
- **方向键 / WASD**：上下选择、左右翻章
- **A（LCTRL）/ 回车 / 空格**：打开、进入目录、确认
- **B（LALT）/ Esc / Q**：返回、退出
- **X（LSHIFT）/ M**：打开菜单（目录 / 书签 / 正文颜色 / 字号 / 亮度 / 退出App / 自定义快捷键）
- **Y（空格）/ K**：书签
- **START（回车）**：**息屏**（先自动保存进度；息屏后仅 START 可唤醒）
- **L1 + START / L2 + START**：强制退出整个 App（自动加书签并保存进度）

### 亮度调节（音量键）
| 组合 | 效果 |
|------|------|
| 音量 + | 亮度 +5%（上限 100%） |
| 音量 − | 亮度 −5%（下限 0%） |
| 按住 **L1 或 R1** + 音量 ± | 每次 ±50%，直接到 100% / 0% |
| 按住 **L2 或 R2** + 音量 ± | 每次 ±20%，到 100% / 0% |

> 也可在菜单「亮度」项里用 ↑/↓ 连续 ±5% 微调。

### 目录（TOC）
- 阅读中按 **X → 目录** 打开章节列表，A 跳转到对应章节。

### 书签
- 阅读中按 **Y** 添加/查看书签；菜单「书签」列出全部，A 跳转。
- 进度自动保存到电子书同目录的 `<书名>.progress`；书签保存到 `<书名>.bookmark`。

---

## 自定义快捷键
进入 **菜单 → 自定义快捷键**，列出 7 个可绑定的逻辑动作及其当前按键：

| 动作 | 默认键 |
|------|--------|
| 下一页 | L1 |
| 上一页 | L2 |
| 打开/确认 | A |
| 返回/退出 | B |
| 菜单 | Y |
| 书签 | X |
| 息屏 | START |

- **重绑定**：选中某动作按 A，再按任意键即绑定；若与已有动作冲突会提示「冲突:xxx」且不保存。
- **恢复默认**：在自定义快捷键界面按 **SELECT（Esc）** 一键恢复出厂默认键位。
- **导出配置**：选中「导出配置」→ 写入 `epub_reader_1…9`（从 1 递增，最多 9 个，满则覆盖第 9 个），并存下当前默认打开路径。
- **导入配置**：选中「导入配置」→ 列出已存在的 `epub_reader_*` 文件，A 载入；导入后默认打开路径 = 该配置记录的路径。

> 配置文件保存位置见下文「数据与配置落盘」。

---

## 数据与配置落盘
OPK 以**只读**方式挂载，因此所有可写数据均存放在 **`$HOME/.epubreader/`**（设备即
`/media/home/.epubreader/`，首次运行自动创建）：
- `config.cfg`：字号 / 正文颜色 / 亮度
- `epub_reader_keys.cfg`：自定义快捷键与默认打开路径
- `epub_reader_1 … epub_reader_9`：导出的快捷键方案
- `run.log`：运行日志（便于排查；launch.sh 写入此处）

电子书进度/书签与电子书同目录（`<书名>.progress` / `<书名>.bookmark`）。

---

## 部署到 GDK mini
> G 盘（U 盘模式）= 设备 `/media/roms`（SD 卡）；H 盘（若有）= 设备 `/media/data`（内部存储）。

**最简单：放进内部存储 apps 目录，图标自动出现在系统菜单**
1. 把 `EPUBReader.opk` 复制到 **`H:/apps/`**（设备内 `/media/data/apps/`，与出厂应用同目录，启动器会自动扫描并显示图标）。
2. 同时保留一份在 `G:/apps/`（SD 卡）作为备用；从文件管理器直接打开 `.opk` 也能运行。

**若图标未自动出现（不同固件扫描策略不同），两种兜底：**
- 方式 A（自动注册，推荐）：进系统后从文件管理器**打开一次** `EPUBReader.opk`，本 App 的 `launch.sh` 会在首次运行时自动把图标注册进 `$HOME/.esoteric/sections/applications/`；之后进系统即直接可见，无需再操作。
- 方式 B（手动注册）：在 iuxui 的 sections 注册目录 `$HOME/.esoteric/sections/applications/` 下新建无扩展名文件 `epubreader`，内容：
  ```
  title=EPUB Reader
  description=EPUB 电子书阅读器 (SDL1.2, MIPS) v1.1
  exec=/usr/bin/opkrun
  params=-m default.gcw0.desktop "/media/roms/apps/EPUBReader.opk"
  icon=/media/roms/apps/icon.png
  selectorbrowser=false
  ```
  （图标 `icon.png` 已随包放在 `G:/apps/icon.png`）

3. 电子书（`.epub`）随便放 SD 卡某目录，程序启动后浏览打开即可。

---

## 在 WSL2 Ubuntu 中重新构建（进阶）
> ⚠️ 编译**必须 -O0**。此工具链 gcc 打过 XBurst 补丁，-O1/-O2/-Os 会无条件生成 MXU 私有
> 指令（`lxw` 等），GDK mini 的 CPU 不支持 → 运行即 `SIGILL` 闪退；唯 -O0 干净。
> 因此**不要用 `Makefile.gcw0`（它写死 -O2）**，请用下面的脚本。

```bash
# 1) 一次性准备工具链 + squashfs-tools（已就绪可跳过）
bash setup-ubuntu.sh

# 2) 编译（-O0，已内嵌 libwebpdecode.a）
cd src
bash build_local.sh
# 产物: src/epubreader

# 3) 打包 OPK（mksquashfs）
#    把 epubreader / icon.png / launch.sh / default.gcw0.desktop 放入同一目录后:
mksquashfs <该目录> EPUBReader.opk -all-root -noappend -no-progress
```
- `launch.sh`：设置 `SDL_VIDEODRIVER=fbcon`、`SDL_FBDEV=/dev/fb0`、`SDL_NOMOUSE=1`，日志写到 `$HOME/.epubreader/run.log`。
- `default.gcw0.desktop`：OPK 元数据，`Exec=launch.sh`、`Icon=icon`。

---

## 校验（编译后）
```bash
cd src
bash validate-opk.sh
```
会检查二进制确为 **MIPS** 架构、且链接了 `libSDL / SDL_ttf / SDL_image / libz / uClibc`。

---

## 已知限制
- 仅做基础排版：按宽度折行 + CJK 逐字断行，未实现完整 CSS 样式。
- 图片缩放查看器支持 PNG/JPG/GIF/BMP/WebP/TIFF；超大图会等比缩放到屏幕。
- `KEY_107` / `MENU` 两键暂未分配功能（键码已记录）。
- 真机实测请以你的 GDK mini 为准；遇到闪退把 `$HOME/.epubreader/run.log` 反馈给我。

## 排错
- 若菜单不显示 App：确认 `EPUBReader.opk` 路径与注册文件 `exec` 指向一致（含 `default.gcw0.desktop`）。
- 若中文是方块：检查设备 `/usr/share/fonts/` 下是否存在 SourceHanSans（系统字体缺失才会回退失败）。
- 若亮度/息屏无反应：确认音量键键码（78/74）与你的固件一致；不一致时改 `main.c` 顶部 `KEY_VOLUME_*` 宏后重新 `build_local.sh`。
