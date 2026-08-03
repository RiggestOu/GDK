import shutil, os, zipfile
root = "E:/WorkBuddy/GDKmini/GDK"
fd = os.path.join(root, "folder_deploy")
opkbuild = os.path.join(root, "src", "opk_build")   # 四件套+system.ttf 真源
src = os.path.join(root, "src")                     # EPUBReader.opk 真源(由 mksquashfs 产出)
# 散件来自 opk_build（含随包去位图字体 system.ttf）
pieces = ["default.gcw0.desktop","epubreader","epubreader_icon.png","launch.sh","system.ttf"]
os.makedirs(fd, exist_ok=True)
stray = os.path.join(fd, "icon.png")
if os.path.exists(stray):
    os.remove(stray)
# 复制散件到 folder_deploy
for f in pieces:
    shutil.copy(os.path.join(opkbuild, f), os.path.join(fd, f))
# 复制 OPK（真源在 src 根，避免把 OPK 自身塞回 opk_build 造成嵌套打包）
shutil.copy(os.path.join(src, "EPUBReader.opk"), os.path.join(fd, "EPUBReader.opk"))
# zip 扁平条目：OPK + 四件套 + system.ttf + 快捷键说明
entries = ["EPUBReader.opk"] + pieces + ["快捷键说明.txt"]
zpath = os.path.join(root, "EPUBReader-v1.1.3.zip")
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
    for e in entries:
        p = os.path.join(fd, e)
        if not os.path.exists(p):
            p2 = os.path.join(root, e)
            p = p2 if os.path.exists(p2) else p
        if not os.path.exists(p):
            raise SystemExit("MISSING: " + e)
        z.write(p, e)
print("folder_deploy:", sorted(os.listdir(fd)))
print("zip entries:", z.namelist())
print("zip size:", os.path.getsize(zpath))
