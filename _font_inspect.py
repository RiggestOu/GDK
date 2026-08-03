from fontTools.ttLib import TTFont
import os, sys

src = "E:/WorkBuddy/GDKmini/GDK/_font_check.ttf"
f = TTFont(src)

# cmap -> 阱
cmap = f.getBestCmap()
cp = 0x9631
gname = cmap.get(cp)
print("U+9631 阱 -> glyph name:", gname)
gid = f.getGlyphID(gname)
print("glyph index:", gid)

eblc = f['EBLC']
ebdt = f['EBDT']
print("EBLC strikes 数:", len(eblc.strikes))
print("EBDT strikeData 类型:", type(ebdt.strikeData))

for i, strike in enumerate(eblc.strikes):
    bst = strike.bitmapSizeTable
    print(f"\n--- strike[{i}] ppemX={bst.ppemX} ppemY={bst.ppemY} bitDepth={bst.bitDepth} "
          f"indexSubTables={len(strike.indexSubTables)} ---")
    # locate 阱 data
    sd = ebdt.strikeData
    strike_data = sd[i] if isinstance(sd, (list, dict)) else None
    if isinstance(sd, dict):
        strike_data = sd.get(i)
    if strike_data is None:
        print("  (无 strikeData)")
        continue
    # strike_data 可能是 {glyphName: data} 或 {glyphID: data}
    key = gname if gname in strike_data else (gid if gid in strike_data else None)
    if key is None:
        # 尝试用 str(gid)
        key = str(gid) if str(gid) in strike_data else None
    if key is None:
        print("  阱 不在该 strike 的位图里（可能用轮廓）")
        continue
    bgd = strike_data[key]
    m = bgd.metrics
    print(f"  阱 位图: key={key} w={m.width} h={m.height} "
          f"bearingX={m.horiBearingX} bearingY={m.horiBearingY} advance={m.horiAdvance}")
    # ASCII 预览 (1bpp, MSB first, 每行按 width 取 bit)
    data = bgd.data
    bits = []
    for byte in data:
        for b in range(7, -1, -1):
            bits.append((byte >> b) & 1)
    rowbytes = (m.width + 7) // 8
    out = []
    for y in range(m.height):
        row = bits[y*rowbytes*8 : y*rowbytes*8 + m.width]
        out.append("".join('#' if v else '.' for v in row))
    for line in out:
        print("   " + line)
