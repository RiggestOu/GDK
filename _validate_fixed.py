import json, sys
sys.path.insert(0, r'E:/WorkBuddy/GDKmini/GDK')
from _ebdt_raw import EBDTFont
from fontTools.ttLib import TTFont

fixed_path = r'E:/WorkBuddy/GDKmini/GDK/system_fixed.ttf'
f = EBDTFont(fixed_path)

# cmap: codepoint -> glyph index
ft = TTFont(fixed_path)
best = ft.getBestCmap()            # {codepoint: glyphName}
order = ft.getGlyphOrder()         # list, index = glyph id
cp2gi = {cp: order.index(name) for cp, name in best.items()}

fix = json.load(open(r'E:/WorkBuddy/GDKmini/GDK/_fix_list.json', encoding='utf-8'))

not_fixed = []
fixed_ok = []
for t in fix:
    si, gi = t['si'], t['gi']
    shown_cp = t['shown_cp']
    shown_gi = cp2gi.get(shown_cp)
    kt = f.bitmap_key(si, gi)
    ks = f.bitmap_key(si, shown_gi) if shown_gi is not None else None
    if kt is None or ks is None:
        not_fixed.append((t['ch'], si, 'bitmap_missing(target=%s shown=%s)' % (kt is None, ks is None)))
        continue
    if kt == ks:
        not_fixed.append((t['ch'], si, 'STILL shows %s' % t['shown_as']))
    else:
        fixed_ok.append((t['ch'], si))

print("total fixes:", len(fix))
print("FIXED OK:", len(fixed_ok))
for c, si in fixed_ok:
    print("   OK  %s @strike%d" % (c, si))
print("NOT FIXED / ambiguous:", len(not_fixed))
for c, si, why in not_fixed:
    print("   ??  %s @strike%d  %s" % (c, si, why))
