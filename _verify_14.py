import json, sys
sys.path.insert(0, r'E:/WorkBuddy/GDKmini/GDK')
from _ebdt_raw import EBDTFont
from fontTools.ttLib import TTFont

fixed_path = r'E:/WorkBuddy/GDKmini/GDK/system_fixed.ttf'
f = EBDTFont(fixed_path)
ft = TTFont(fixed_path)
best = ft.getBestCmap()
order = ft.getGlyphOrder()
cp2gi = {cp: order.index(name) for cp, name in best.items()}
glyf = ft['glyf']

fix = json.load(open(r'E:/WorkBuddy/GDKmini/GDK/_fix_list.json', encoding='utf-8'))

def outline_bytes(gi):
    g = glyf[order[gi]]
    # 用 glyph 的 bounding/coords 作为轮廓指纹; 直接取 glyf 原始数据
    return g.__class__

def glyf_sig(gi):
    g = glyf[order[gi]]
    if g.numberOfContours == 0:
        return b'EMPTY'
    # 用端点和坐标做签名
    try:
        coords = g.getCoordinates(glyf)[0]
        return (g.numberOfContours, tuple(coords[:20]))
    except Exception as e:
        return ('ERR', str(e))

print("=== 14 个 STILL-shows 的轮廓交叉验证 ===")
not_fixed = []
for t in fix:
    si, gi = t['si'], t['gi']
    shown_gi = cp2gi.get(t['shown_cp'])
    kt = f.bitmap_key(si, gi)
    ks = f.bitmap_key(si, shown_gi)
    if kt is not None and ks is not None and kt == ks:
        # 轮廓是否真不同?
        st = glyf_sig(gi); ss = glyf_sig(shown_gi)
        same_outline = (st == ss)
        not_fixed.append((t['ch'], t['shown_as'], si, same_outline, st != 'EMPTY' and ss != 'EMPTY'))
        print("  %s->%s @strike%d  轮廓相同?=%s  两者均有轮廓=%s" % (
            t['ch'], t['shown_as'], si, same_outline, (st != 'EMPTY' and ss != 'EMPTY')))

print()
print("=== 抽查 3 个已修复(FIXED OK)确认机制正确 ===")
sample = [t for t in fix if t['ch'] in ('阱','丟','晚')]
for t in sample:
    si, gi = t['si'], t['gi']
    shown_gi = cp2gi.get(t['shown_cp'])
    kt = f.bitmap_key(si, gi); ks = f.bitmap_key(si, shown_gi)
    rt = f.read_bitmap(si, gi); rs = f.read_bitmap(si, shown_gi)
    print("  %s @strike%d  target_px=%d shown_px=%d  bytes_equal=%s" % (
        t['ch'], si, len(rt['pixels']), len(rs['pixels']), kt == ks))
