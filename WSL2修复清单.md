# WSL2 报错 HCS_E_HYPERV_NOT_INSTALLED — 修复清单

> 含义：Hyper-V / 虚拟机监控程序层没真正加载起来。
> 顺序：先跑自动脚本 → 按需进 BIOS → 重启 → 验证。多数人到第 2 步就好了。

## ✅ 第 0 步：跑自动修复脚本（我已写好，帮你自动做 4 件事）
1. 开始菜单搜 **PowerShell** → 右键 → **以管理员身份运行**
2. 执行：
   ```powershell
   cd E:\WorkBuddy\GDKmini
   powershell -ExecutionPolicy Bypass -File .\fix-wsl2.ps1
   ```
3. 脚本会自动：启用「虚拟机平台 / WSL / HypervisorPlatform（有则含 Hyper-V）」、把 `bcdedit hypervisorlaunchtype` 设为 Auto、修好被禁用的 `vmcompute` 服务、`wsl --update`。
4. 看脚本结尾提示：它会告诉你**要不要进 BIOS**、**要不要重启**。

> 脚本会读取你 CPU 的 `VirtualizationFirmwareEnabled`。若显示 False → 说明 BIOS 虚拟化没开，必须做第 1 步。

## ☐ 第 1 步：BIOS 里开启虚拟化（脚本无法代劳，只能你来）
- [ ] 重启电脑，开机时**狂按进 BIOS 的键**（常见：`Del` / `F2` / `F10` / `Esc`，看主板品牌）
- [ ] 找到虚拟化选项（在 **Advanced → CPU Configuration** 附近），名字可能是：
  - Intel 机：**Intel Virtualization Technology (VT-x)** / **Intel VMX**
  - AMD 机：**SVM Mode** / **AMD-V**
- [ ] 设为 **Enabled**
- [ ] （如有）**VT-d / IOMMU** 也一并开启
- [ ] **Save & Exit**（通常 F10）

## ☐ 第 2 步：重启后验证
- [ ] 重启完成后，管理员 PowerShell 里跑：
  ```powershell
  wsl -l -v          # 确认 Ubuntu 在
  wsl -d Ubuntu      # 能进到 Linux 提示符就成功了
  ```
- [ ] 进去后测一下：`uname -a`，看到 Linux 内核信息即 OK。

## ☐ 第 3 步（兜底）：还报同样错？用「Hyper-V 关了再开」大法
> 这是社区里点赞最多的偏方，对"功能显示已启用但就是不生效"特别有效。
- [ ] 控制面板 → 程序 → **启用或关闭 Windows 功能**
- [ ] **取消勾选** Hyper-V（及其全部子项）→ 确定 → **重启**
- [ ] 再进去 → **重新勾选** Hyper-V（全部子项）→ 确定 → **重启**
- [ ] 重启后再 `wsl -d Ubuntu`

## ☐ 第 4 步（可选）：系统版本
- [ ] Windows 11 21H2 有已知 Hyper-V 兼容 bug。设置 → Windows Update → 检查更新，升到 **23H2 或更高**（版本号 ≥ 26100）。
      （你当前 Windows 版本 10.0.26200，已经够新，此步一般可跳过。）

---

## 进 Ubuntu 之后（回到正题：编译阅读器）
```bash
cd /mnt/e/WorkBuddy/GDKmini      # E: 盘在 WSL 里就是 /mnt/e
bash setup-ubuntu.sh             # 装 OpenDingux 交叉工具链
cd src
make -f Makefile.gcw0            # 交叉编译
# 放一款中文 TTF 命名为 font.ttf 到 src/ 后：
./package.sh                     # 产出 EPUBReader.opk
bash validate-opk.sh             # 校验是 MIPS + 链了 SDL
```
把 `EPUBReader.opk` 复制到 SD 卡（H: 盘）`APPS` 目录即可在 GDK mini 上运行。
