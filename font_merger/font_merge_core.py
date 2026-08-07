# -*- coding: utf-8 -*-
"""
字体合并核心引擎（无 GUI 依赖）
================================
把多套字体（简体/繁体/日文/韩文/英文 …）合并为一个字体文件，并尽量压缩：

1. 去重：用 RecordingPen 录制每个字形的轮廓指令生成"指纹"。
   两个码位对应的字形若轮廓完全相同（统一缩放到目标 unitsPerEm 后、坐标取整），
   则只保留一份字形数据，多个码位统一指向同一个字形 —— 这就是"统一指向"省空间。
   例如：繁体字体里与简体字体轮廓相同的共用汉字、日语字体里与简/繁相同的汉字。

2. 冲突：同一 Unicode 码位在多个源字体里出现了"不同"的字形（轮廓不同），
   这类码位无法自动决定保留哪个，记录下来交给上层 UI 让用户人工筛选。

3. 格式：输出统一为 TrueType 轮廓（glyf）。源可以是 glyf(TTF) 或 CFF(OTF)，
   经 TTGlyphPen 转成 TrueType 轮廓（CFF 的三次曲线会被近似成二次，对 CJK 影响极小）。
   同时去除 TrueType hinting 指令以进一步压缩体积。

4. unitsPerEm 归一：不同字体 unitsPerEm 可能不同（1000/1024/2048/256），
   全部缩放至第一个源的 unitsPerEm，保证合并后字形大小一致。
"""

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph, table__g_l_y_f
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.misc.transform import Transform


class ExpandingPen(TransformPen):
    """把 composite(组件) 字形递归展开为绝对轮廓，避免目标字体里残留组件引用。
    同时承担坐标缩放（TransformPen 的职责）。"""

    def __init__(self, out_pen, transform, glyphset):
        super().__init__(out_pen, transform)
        self.gs = glyphset
        self._out = out_pen
        self._tf = transform if isinstance(transform, Transform) else Transform(*transform)

    def addComponent(self, glyphName, transformation):
        comp = Transform(*transformation)
        new = self._tf.transform(comp)
        sub = ExpandingPen(self._out, new, self.gs)
        self.gs[glyphName].draw(sub)


def _fp_key(rec_value):
    """把 RecordingPen 录制的指令序列规范化成一个可哈希的指纹 key。
    坐标统一取整（目标 em 单位），避免浮点误差导致本应相同的字形被判为不同。"""
    key = []
    for cmd, args in rec_value:
        if cmd in ("moveTo", "lineTo", "qCurveTo", "curveTo"):
            norm = []
            for pt in args:
                if isinstance(pt, tuple) and len(pt) == 2:
                    norm.append((int(round(pt[0])), int(round(pt[1]))))
                else:
                    norm.append(pt)
            key.append((cmd, tuple(norm)))
        elif cmd == "closePath":
            key.append((cmd, ()))
        elif cmd == "addComponent":
            gname = args[0]
            tf = tuple(round(float(v), 4) for v in args[1])
            key.append((cmd, (gname, tf)))
        else:
            key.append((cmd, args))
    return tuple(key)


class MergeConflict:
    """单个冲突：某码位在多个源里有不同字形。"""

    __slots__ = ("codepoint", "char", "candidates")

    def __init__(self, codepoint, char, candidates):
        self.codepoint = codepoint
        self.char = char
        self.candidates = candidates  # list of dict: {font_index, font_name, glyph_name, fp_key}


class FontSource:
    __slots__ = ("path", "font", "upm", "name", "cmap", "glyphset", "fmt")

    def __init__(self, path, font):
        self.path = path
        self.font = font
        self.upm = font["head"].unitsPerEm
        self.name = self._detect_name(font)
        self.cmap = font.getBestCmap()  # {codepoint: glyphName}
        self.glyphset = font.getGlyphSet()
        self.fmt = "CFF(OTF)" if "CFF " in font else "glyf(TTF)"

    @staticmethod
    def _detect_name(font):
        try:
            name = font["name"]
            for rec in name.names:
                if rec.nameID in (4, 1) and rec.platformID in (3, 1, 0):
                    try:
                        return rec.toUnicode()
                    except Exception:
                        pass
        except Exception:
            pass
        return "未知字体"


class FontMerger:
    def __init__(self):
        self.sources = []          # List[FontSource]
        self.target_upm = None
        self._fp_cache = {}        # (font_index, glyphName) -> fp_key

    # ---------------------------------------------------------------- 加载
    def add_font(self, path):
        font = TTFont(path, fontNumber=0)
        src = FontSource(path, font)
        if self.target_upm is None:
            self.target_upm = src.upm
        self.sources.append(src)
        return src

    def clear(self):
        for s in self.sources:
            try:
                s.font.close()
            except Exception:
                pass
        self.sources = []
        self.target_upm = None
        self._fp_cache = {}

    # ---------------------------------------------------------------- 指纹
    def _glyph_fp(self, font_index, glyph_name):
        cache_key = (font_index, glyph_name)
        if cache_key in self._fp_cache:
            return self._fp_cache[cache_key]
        src = self.sources[font_index]
        scale = self.target_upm / src.upm
        rec = RecordingPen()
        pen = ExpandingPen(rec, (scale, 0, 0, scale, 0, 0), src.glyphset)
        src.glyphset[glyph_name].draw(pen)
        key = _fp_key(rec.value)
        self._fp_cache[cache_key] = key
        return key

    # ---------------------------------------------------------------- 扫描
    def scan(self, progress_cb=None):
        """扫描所有源，检测冲突。返回 (conflicts, stats)。"""
        if not self.sources:
            raise RuntimeError("尚未添加任何字体")

        cp_entries = {}   # cp -> list[(fp, fi)]

        total_steps = sum(len(s.cmap) for s in self.sources)
        done = 0
        for fi, src in enumerate(self.sources):
            for cp, gname in src.cmap.items():
                fp = self._glyph_fp(fi, gname)
                cp_entries.setdefault(cp, []).append((fp, fi))
                done += 1
                if progress_cb and (done % 2000 == 0):
                    progress_cb(done, total_steps)
        if progress_cb:
            progress_cb(total_steps, total_steps)

        conflicts = []
        for cp, entries in cp_entries.items():
            fps = set(e[0] for e in entries)
            if len(fps) >= 2:
                seen = {}
                for fp, fi in entries:
                    if fp not in seen:
                        seen[fp] = fi
                candidates = []
                for fp, fi in seen.items():
                    candidates.append({
                        "font_index": fi,
                        "font_name": self.sources[fi].name,
                        "glyph_name": self.sources[fi].cmap.get(cp, ".notdef"),
                        "fp_key": fp,
                    })
                conflicts.append(MergeConflict(
                    cp, chr(cp) if cp < 0x110000 else "", candidates))

        stats = {
            "total_codepoints": len(cp_entries),
            "conflict_count": len(conflicts),
            "sources": [
                {"name": s.name, "fmt": s.fmt, "upm": s.upm, "chars": len(s.cmap)}
                for s in self.sources
            ],
        }
        return conflicts, stats

    # ---------------------------------------------------------------- 构建
    def build(self, choices, output_path, family_name="Merged Font",
              progress_cb=None):
        """根据 choices 构建并保存合并字体。
        choices: dict{ codepoint -> chosen_font_index } 仅冲突码位需要；
                 其余码位自动取去重后的唯一字形。"""
        if not self.sources:
            raise RuntimeError("尚未添加任何字体")

        cp_candidates = {}
        for fi, src in enumerate(self.sources):
            for cp, gname in src.cmap.items():
                fp = self._glyph_fp(fi, gname)
                cp_candidates.setdefault(cp, []).append((fp, fi, gname))

        plan = {}
        for cp, cands in cp_candidates.items():
            fps = {}
            for fp, fi, gn in cands:
                if fp not in fps:
                    fps[fp] = (fi, gn)
            if len(fps) == 1:
                plan[cp] = next(iter(fps.values()))
            else:
                chosen = choices.get(cp)
                if chosen is None:
                    chosen = cands[0][1]
                pick = None
                for fp, fi, gn in cands:
                    if fi == chosen:
                        pick = (fi, gn)
                        break
                plan[cp] = pick if pick else cands[0][1:]

        # 直接原地修改第一个源字体对象再保存，绕开 "save 到 BytesIO 再 TTFont(buf)
        # 克隆" 在该 fontTools 版本下 save 不落盘内存改动的坑。
        out = self.sources[0].font

        # 必须先捕获各源 hmtx 度量：下面删除循环会把 hmtx 从 out.reader 一并删掉，
        # 之后就无法再读取源0的原始度量(如 'A' 的 1366/-3)，ensure_glyph 会回退默认值。
        src_metrics = [s.font["hmtx"].metrics for s in self.sources]

        # 只保留渲染必需表，丢弃引用旧 glyph 名的排版表(GSUB/GPOS/GDEF/kern/cvt/fpgm/prep...)
        # 否则保存时这些表引用的旧字形名会触发 KeyError('glyph00001' 之类)
        KEEP = {"head", "hhea", "maxp", "OS/2", "post", "name"}
        for tag in list(out.keys()):
            if tag not in KEEP:
                del out[tag]

        if "CFF " in out:
            del out["CFF "]
        if "VORG" in out:
            del out["VORG"]

        glyf = table__g_l_y_f()
        glyf.glyphOrder = []
        glyf.glyphs = {}
        notdef = Glyph()
        notdef.numberOfContours = 0
        notdef.xMin = notdef.yMin = notdef.xMax = notdef.yMax = 0
        notdef.data = b""
        glyf[".notdef"] = notdef
        out["glyf"] = glyf
        out["loca"] = newTable("loca")
        out["cmap"] = newTable("cmap")

        from fontTools.ttLib.tables._h_m_t_x import table__h_m_t_x
        # 注：src_metrics 已在删除循环之前捕获（见上方），此处仅创建新 hmtx 表。
        hmtx = table__h_m_t_x()
        hmtx.metrics = {".notdef": (self.target_upm // 2, 0)}
        out["hmtx"] = hmtx

        if "post" in out:
            # 用 format 2.0 显式写入 g0/g1… 字形名，使名字与 glyf 物理顺序(GID)严格对应。
            # （format 3.0 不存名字，fontTools 重载时会生成与轮廓不符的标签名，导致名实错乱。）
            out["post"].formatType = 2.0
            out["post"].extraNames = []
            out["post"].mapping = {}

        fp_to_name = {}
        name_counter = [0]

        def alloc_name():
            n = "g%d" % name_counter[0]
            name_counter[0] += 1
            return n

        def ensure_glyph(font_index, glyph_name):
            key = self._glyph_fp(font_index, glyph_name)
            if key in fp_to_name:
                return fp_to_name[key]
            src = self.sources[font_index]
            scale = self.target_upm / src.upm
            pen = TTGlyphPen(None)
            exp = ExpandingPen(pen, (scale, 0, 0, scale, 0, 0), src.glyphset)
            src.glyphset[glyph_name].draw(exp)
            g = pen.glyph()
            g.recalcBounds(glyf)
            name = alloc_name()
            glyf[name] = g
            adv, lsb = src_metrics[font_index].get(glyph_name, (src.upm // 2, 0))
            hmtx.metrics[name] = (int(round(adv * scale)), int(round(lsb * scale)))
            fp_to_name[key] = name
            return name

        cmap4 = {}
        cmap12 = {}
        total = len(plan)
        done = 0
        for cp, (fi, gn) in plan.items():
            tname = ensure_glyph(fi, gn)
            if cp <= 0xFFFF:
                cmap4[cp] = tname
            else:
                cmap12[cp] = tname
            done += 1
            if progress_cb and (done % 2000 == 0):
                progress_cb(done, total)
        if progress_cb:
            progress_cb(total, total)

        from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
        t4 = CmapSubtable.newSubtable(4)
        t4.platformID = 3
        t4.platEncID = 1
        t4.language = 0x409
        t4.cmap = cmap4
        tables = [t4]
        if cmap12:
            # format 12 必须放“全部”码位（含 BMP），而不仅是 >BMP 部分；
            # 否则优先选用 format 12(UCS-4) 的渲染器会漏掉所有 BMP 字符。
            t12 = CmapSubtable.newSubtable(12)
            t12.platformID = 3
            t12.platEncID = 10
            t12.language = 0x409
            full12 = dict(cmap4)
            full12.update(cmap12)
            t12.cmap = full12
            tables.append(t12)
        out["cmap"].tables = tables
        out["cmap"].tableVersion = 1 if cmap12 else 0

        maxp = out["maxp"]
        maxp.tableVersion = 0x00010000
        maxp.numGlyphs = len(glyf.glyphOrder)
        if not hasattr(maxp, "maxZones") or maxp.maxZones is None:
            maxp.maxZones = 1
        for attr, val in dict(
            maxTwilightPoints=0, maxStorage=0, maxFunctionDefs=0,
            maxInstructionDefs=0, maxStackElements=0, maxSizeOfInstructions=0,
            maxComponentElements=0, maxComponentDepth=0,
        ).items():
            setattr(maxp, attr, val)
        try:
            maxp.recalc(glyf)
        except Exception:
            pass

        out["head"].unitsPerEm = self.target_upm
        out["head"].indexToLocFormat = 0
        out["head"].glyphDataFormat = 0

        src0 = self.sources[0]
        hhea = out["hhea"]
        hhea.numberOfHMetrics = len(glyf.glyphOrder)
        asc = src0.font["hhea"].ascent if "hhea" in src0.font else int(self.target_upm * 0.8)
        desc = src0.font["hhea"].descent if "hhea" in src0.font else -int(self.target_upm * 0.2)
        hhea.ascent = int(round(asc * (self.target_upm / src0.upm)))
        hhea.descent = int(round(desc * (self.target_upm / src0.upm)))

        if "OS/2" in out:
            os2 = out["OS/2"]
            try:
                os2.ulUnicodeRange1 = 0xFFFFFFFF
                os2.ulUnicodeRange2 = 0xFFFFFFFF
                os2.ulUnicodeRange3 = 0xFFFFFFFF
                os2.ulUnicodeRange4 = 0xFFFFFFFF
                os2.usFirstCharIndex = 0
                os2.usLastCharIndex = 0xFFFF
            except Exception:
                pass

        try:
            nm = out["name"]
            nm.setName(family_name, 1, 3, 1, 0x409)
            nm.setName("Regular", 2, 3, 1, 0x409)
            nm.setName(family_name + " Regular", 4, 3, 1, 0x409)
            nm.setName(family_name.replace(" ", ""), 6, 3, 1, 0x409)
        except Exception:
            pass

        # fontTools 用 TTFont.glyphOrder 实例属性作为权威字形顺序；
        # 它是从源字体克隆来的(数字占位名列表)，必须同步成我们重建的 glyf.glyphOrder
        out.glyphOrder = glyf.glyphOrder
        # 刷新反向映射缓存，否则 cmap 编译时 getGlyphID 仍用旧的数字名映射，
        # 导致大量 cmap 条目丢失
        out.getReverseGlyphMap(rebuild=True)
        out.save(output_path)

        used_glyphs = len(fp_to_name)
        return {
            "output": output_path,
            "total_codepoints": len(plan),
            "unique_glyphs": used_glyphs,
            "dedup_saved": sum(len(s.cmap) for s in self.sources) - used_glyphs,
            "target_upm": self.target_upm,
        }


# ---------------------------------------------------------------- 字形预览
def glyph_outline_to_path(font, glyph_name, scale=1.0):
    """把某个字形绘制成 QPainterPath（供 GUI 预览）。返回 (QPainterPath, bbox)。"""
    from PySide6.QtGui import QPainterPath
    from fontTools.pens.recordingPen import RecordingPen

    gs = font.getGlyphSet()
    rec = RecordingPen()
    pen = ExpandingPen(rec, (scale, 0, 0, scale, 0, 0), gs)
    gs[glyph_name].draw(pen)

    path = QPainterPath()
    xmin = ymin = float("inf")
    xmax = ymax = float("-inf")

    def upd(x, y):
        nonlocal xmin, ymin, xmax, ymax
        xmin, ymin, xmax, ymax = min(xmin, x), min(ymin, y), max(xmax, x), max(ymax, y)

    for cmd, args in rec.value:
        if cmd == "moveTo":
            x, y = args[0]
            path.moveTo(x, -y)
            upd(x, y)
        elif cmd == "lineTo":
            x, y = args[0]
            path.lineTo(x, -y)
            upd(x, y)
        elif cmd == "qCurveTo":
            pts = args
            for i in range(len(pts) - 1):
                c, e = pts[i], pts[i + 1]
                cx, cy = c
                ex, ey = e
                path.quadTo(cx, -cy, ex, -ey)
                upd(cx, cy)
                upd(ex, ey)
        elif cmd == "curveTo":
            pts = args
            end = pts[-1]
            path.cubicTo(pts[0][0], -pts[0][1], pts[1][0], -pts[1][1], end[0], -end[1])
            for p in pts:
                upd(p[0], p[1])
        elif cmd == "closePath":
            path.closeSubpath()
    if xmax < xmin:
        bbox = (0, 0, 0, 0)
    else:
        bbox = (xmin, ymin, xmax, ymax)
    return path, bbox


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 3:
        print("用法: font_merge_core.py 输出.ttf 字体1 字体2 [...]")
        sys.exit(1)
    outp = sys.argv[1]
    m = FontMerger()
    for p in sys.argv[2:]:
        m.add_font(p)
        print("已加载:", m.sources[-1].name, m.sources[-1].fmt, "upm", m.sources[-1].upm)
    conflicts, stats = m.scan()
    print("扫描完成:", stats)
    print("冲突数:", len(conflicts))
    choices = {c.codepoint: c.candidates[0]["font_index"] for c in conflicts}
    res = m.build(choices, outp, family_name="MergedCLI")
    print("构建完成:", res)
