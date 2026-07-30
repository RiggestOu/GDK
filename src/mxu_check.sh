#!/bin/bash
BIN=/mnt/e/WorkBuddy/GDKmini/GDK/src/epubreader
OBJDUMP=/home/ocean/gcw0-toolchain/usr/bin/mipsel-gcw0-linux-uclibc-objdump
"$OBJDUMP" -d "$BIN" > /tmp/dis.txt 2>/dev/null
python3 - <<PY
import re
sp1=sp2=illegal=0
lxw=0
pat=re.compile(r"^\s*[0-9a-f]+:\s*([0-9a-f]{8})\s")
functs={}
with open("/tmp/dis.txt") as f:
    for ln in f:
        m=pat.match(ln)
        if not m: continue
        op=int(m.group(1),16)
        o=(op>>26)&0x3f
        if o==0: sp1+=1
        elif o==0x1c:
            sp2+=1
            funct=op&0x3f
            functs[funct]=functs.get(funct,0)+1
            if funct not in (0,1,2,4,5,0x20,0x21,0x3f):
                illegal+=1
with open("/tmp/dis.txt") as f:
    for ln in f:
        if "\tlxw" in ln: lxw+=1
print("SPECIAL(op0)=",sp1,"SPECIAL2(op0x1c)=",sp2,"illegal-MXU=",illegal,"lxw=",lxw)
if lxw:
    print("--- lxw 出现位置 ---")
    with open("/tmp/dis.txt") as f:
        for ln in f:
            if "\tlxw" in ln: print(ln.rstrip())
PY
