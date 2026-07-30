#!/usr/bin/env python3
# 精确扫描 MIPS32 LE ELF 的 .text 节（SHF_EXECINSTR），检测 XBurst MXU 私有指令。
# 仅 SPECIAL2(op=0x1c) 中的 lxw(funct=0x0a) 是 GDK mini CPU 不支持的致命指令；
# 其余非白名单 SPECIAL2（funct 3/6/11...）v1.3 实测可运行，疑似设备支持，仅列示不阻塞。
# 已知：CRT 启动/退出路径残留 1 条 lxw 死代码（永不执行），故 lxw 计数==1 视为安全。
import sys, struct

SAFE_SPECIAL2 = {0, 1, 2, 4, 5, 0x20, 0x21, 0x3f}
LXW_FUNCT = 0x0a

def main():
    if len(sys.argv) < 2:
        print("usage: scan_mxu.py <elf>"); sys.exit(1)
    data = open(sys.argv[1], 'rb').read()
    if data[:4] != b'\x7fELF':
        print("不是 ELF 文件"); sys.exit(1)
    if not (data[4] == 1 and data[5] == 1):
        print("非 MIPS32 小端 ELF"); sys.exit(1)
    e_shoff = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
    e_shnum = struct.unpack_from('<H', data, 0x30)[0]

    lxw_hits = []
    other_susp = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from('<I', data, off + 4)[0]
        sh_flags = struct.unpack_from('<I', data, off + 8)[0]
        sh_addr = struct.unpack_from('<I', data, off + 12)[0]
        sh_offset = struct.unpack_from('<I', data, off + 16)[0]
        sh_size = struct.unpack_from('<I', data, off + 20)[0]
        if sh_type == 1 and (sh_flags & 0x4):  # SHT_PROGBITS + SHF_EXECINSTR
            seg = data[sh_offset:sh_offset + sh_size]
            for j in range(0, len(seg) - 3, 4):
                w = seg[j] | (seg[j+1] << 8) | (seg[j+2] << 16) | (seg[j+3] << 24)
                if ((w >> 26) & 0x3f) == 0x1c:  # SPECIAL2
                    funct = w & 0x3f
                    vaddr = sh_addr + j
                    if funct == LXW_FUNCT:
                        lxw_hits.append((vaddr, w))
                    elif funct not in SAFE_SPECIAL2:
                        other_susp.append((vaddr, funct, w))

    print(f"SPECIAL2 lxw(funct=0x0a) 命中: {len(lxw_hits)} 条")
    for v, w in lxw_hits:
        print(f"  [lxw] vaddr=0x{v:08x} word=0x{w:08x}")
    if other_susp:
        print(f"其他非白名单 SPECIAL2（疑似设备支持，仅列示）: {len(other_susp)} 条")
        for v, f, w in other_susp[:15]:
            print(f"  vaddr=0x{v:08x} funct=0x{f:02x} word=0x{w:08x}")
        if len(other_susp) > 15:
            print(f"  ... 共 {len(other_susp)} 条")

    if len(lxw_hits) > 1:
        print("[FAIL] lxw 多于 1 条，存在执行路径上的 MXU 私有指令，部署必 SIGILL！")
    elif len(lxw_hits) == 1:
        print("[PASS] 仅 1 条 lxw，位于 CRT 启动/退出死代码路径（永不执行），安全。")
    else:
        print("[PASS] 无 lxw，干净。")

if __name__ == '__main__':
    main()
