import sys, os, tempfile, time
from fontTools.ttLib import TTFont
from PIL import ImageFont

SRC = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
OUT = "E:/WorkBuddy/GDKmini/GDK/_bitmap_audit.txt"

T1_SELF = 0.35     # 自身(位图 vs 轮廓)低于此值 -> 嫌疑（阈值压低以缩小嫌疑集）
TDUP    = 0.90     # 与"另一个字"的位图相似度高于此值 -> 判定画错
MARGIN  = 0.15     # 且 另一字相似度 必须 > 自身相似度 + MARGIN

t0 = time.time()
f = TTFont(SRC)
cmap = f.getBestCmap()
rev = {}
for cp, name in cmap.items():
    rev.setdefault(name, cp)
eblc = f['EBLC']
ebdt = f['EBDT']
sd = ebdt.strikeData

def ch(cp):
    try: return chr(cp)
    except Exception: return "U+%04X" % (cp if cp else 0)

def emb_ink(bgd):
    m = bgd.metrics
    w, h = m.width, m.height
    s = set()
    for y in range(h):
        row = bgd.getRow(y)
        for x in range(w):
            if (row[x >> 3] >> (7 - (x & 7))) & 1:
                s.add((x, y))
    return s

def iou_trans(A, B, dmax=1):
    best = 0.0
    for dx in range(-dmax, dmax+1):
        for dy in range(-dmax, dmax+1):
            Bs = {(x+dx, y+dy) for (x, y) in B}
            inter = len(A & Bs)
            if inter == 0: continue
            union = len(A) + len(Bs) - inter
            v = inter / union
            if v > best: best = v
    return best

def iou(A, B):
    if not A or not B: return 0.0
    inter = len(A & B)
    union = len(A) + len(B) - inter
    return inter / union if union else 0.0

# 临时无位图字体（PIL 用轮廓渲染）
tmp = TTFont(SRC)
for tag in ("EBLC","EBDT","bhed"):
    if tag in tmp: del tmp[tag]
fd, tmp_path = tempfile.mkstemp(suffix=".ttf")
os.close(fd)
tmp.save(tmp_path)

# ---- Phase A: strike 0 找嫌疑 ----
si0 = 0
ppem0 = eblc.strikes[si0].bitmapSizeTable.ppemX
sdi0 = sd[si0]
names0 = list(sdi0.keys())
font0 = ImageFont.truetype(tmp_path, ppem0)
emb0 = {}
outl0 = {}
for name in names0:
    cp = rev.get(name)
    if cp is None: continue
    try: emb0[name] = emb_ink(sdi0[name])
    except Exception: continue
    try:
        mask = font0.getmask(ch(cp))
        px = mask.load(); w, h = mask.size
        s = set()
        for y in range(h):
            for x in range(w):
                if px[x, y] > 128: s.add((x, y))
        outl0[name] = s
    except Exception: continue

candidates = {}
for name in emb0:
    if name not in outl0 or not outl0[name]: continue
    selfv = iou_trans(emb0[name], outl0[name])
    if selfv >= T1_SELF: continue
    best_v, best_n = 0.0, None
    for other in emb0:
        if other == name: continue
        v = iou(emb0[name], emb0[other])
        if v > best_v: best_v, best_n = v, other
    if best_n is not None and best_v >= TDUP and best_v > selfv + MARGIN:
        candidates[name] = {'cp': rev.get(name), 'best': best_n,
                            'best_cp': rev.get(best_n), 'best_iou': best_v, 'self_iou': selfv}
print(f"Phase A 完成: strike0 扫 {len(emb0)} 字, 候选画错字 {len(candidates)} (耗时 {time.time()-t0:.1f}s)", flush=True)

# ---- Phase B: 候选字跨全部 strike 确认 ----
final = {}
for cname, info in candidates.items():
    yname = info['best']
    hits = set()
    for si in range(len(eblc.strikes)):
        sdi = sd[si]
        if cname not in sdi or yname not in sdi: continue
        try:
            v = iou(emb_ink(sdi[cname]), emb_ink(sdi[yname]))
        except Exception:
            continue
        if v >= 0.85:
            hits.add(eblc.strikes[si].bitmapSizeTable.ppemX)
    if hits:
        final[info['cp']] = {'char': ch(info['cp']), 'best_char': ch(info['best_cp']),
                             'strikes': hits, 'best_iou': info['best_iou'], 'self_iou': info['self_iou']}
print(f"Phase B 完成: 确认画错字 {len(final)} (总耗时 {time.time()-t0:.1f}s)", flush=True)

try: os.remove(tmp_path)
except Exception: pass

lines = []
lines.append("==== SourceHanSans-Regular-04.ttf 内嵌位图[画错成别的字]全量排查 ====")
lines.append(f"候选阈值: 自身IoU<{T1_SELF}; 判定: 与另一字位图IoU>={TDUP} 且 >自身+{MARGIN}")
lines.append(f"PhaseA基于 ppem={ppem0}; PhaseB 跨全部 {len(eblc.strikes)} 个 strike 用 IoU>=0.85 确认")
lines.append("")
if not final:
    lines.append("未发现[位图被画成别的字]的字。")
else:
    lines.append(f"共发现 {len(final)} 个字的位图疑似画成了别的字：")
    lines.append("")
    lines.append(f"{'字符':<4}{'码点':<8}{'被画成':<6}{'相似度':<7}{'自身度':<7}出现strike(ppem)")
    lines.append("-"*72)
    for cp in sorted(final, key=lambda c: final[c]['best_iou'], reverse=True):
        a = final[cp]
        pps = ",".join(str(p) for p in sorted(a['strikes']))
        lines.append(f"{a['char']:<4}U+{cp:04X:<6}{a['best_char']:<6}{a['best_iou']:<7}{a['self_iou']:<7}{pps}")

txt = "\n".join(lines)
print(txt)
with open(OUT, "w", encoding="utf-8") as fh:
    fh.write(txt + "\n")
print("\n已写入:", OUT)
