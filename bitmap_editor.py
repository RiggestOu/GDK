#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
内嵌位图手工修字工具 (PySide6)
------------------------------------------------------------------
针对 SourceHanSans-Regular-04.ttf 这类带 EBLC/EBDT 内嵌点阵的字体,
逐字号、逐像素地修正被画错的字。

运行:
    E:\\software\\python3\\python.exe bitmap_editor.py

操作:
    黑底白字: 左键在格子上画白格 / 右键画黑格(擦除)
    按住左/右键拖动 = 连续填充(画笔效果), 鼠标按下~松开 = 一步撤销
    鼠标滚轮 = 以光标为中心放大缩小
    鼠标中键拖动 = 平移画布
    Ctrl+Z 撤销(最多500步)   Ctrl+Y 重做   Ctrl+S 保存
    方向键 = 平移整个位图笔画
    底部数字按钮 = 切换/编辑对应字号 (红=待修正 绿=已修改)
    "悬浮参考"开关 = 居中叠加最大字号(24px)作对照, 默认开
    右上角"使用说明"面板 = 本帮助
"""
import sys, os, json, copy

from PySide6.QtCore import Qt, QSize, QPoint, Signal, QEvent
from PySide6.QtGui import (QPainter, QColor, QPen, QBrush, QKeySequence,
                           QShortcut, QFont, QIcon, QPixmap, QImage, QPainterPath)
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                               QHBoxLayout, QLabel, QLineEdit, QPushButton,
                               QListWidget, QListWidgetItem, QSpinBox, QCheckBox,
                               QGroupBox, QFileDialog, QMessageBox, QSplitter,
                               QGridLayout, QComboBox, QPlainTextEdit, QSlider,
                               QStatusBar, QSizePolicy, QScrollArea)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from _ebdt_raw import EBDTFont                      # noqa: E402
from _outline_ref import OutlineRef, ensure_nobitmap, NOBM   # noqa: E402

DEFAULT_FONT = os.path.join(HERE, "_font_check.ttf")
FIXLIST = os.path.join(HERE, "_fix_list.json")


# ==================================================================
class PixelCanvas(QWidget):
    changed = Signal()
    strokeStart = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.w = self.h = 0
        self.pixels = set()
        self.ref = set()
        self.orig = set()
        self.cell = 22
        self.pad = 2              # 字形四周薄边(格数); 仅字形包围盒是黑底, 边为 UI 灰
        self.show_ref = True
        self.show_orig = False
        self.show_float = True
        self.float_bm = None
        self.float_w = self.float_h = 0
        self.float_color = QColor(255, 120, 200, 130)   # 悬浮参考: 半透明粉色
        self.baseline = None        # 位图内基线所在行(=by), 画一条线
        self._drag = None
        self._pan = None
        self.scroll = None          # 外部注入的 QScrollArea, 用于滚轮缩放/中键平移
        self.on_cell_change = None  # 缩放后回写格子尺寸控件
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)

    def set_bitmap(self, pixels, w, h, baseline=None):
        self.pixels = set(pixels)
        self.w, self.h = w, h
        self.baseline = baseline
        self.updateGeometry()
        self.resize(self.sizeHint())
        self.update()

    def set_ref(self, ref):
        self.ref = set(ref)
        self.update()

    def set_orig(self, o):
        self.orig = set(o)
        self.update()

    def set_float(self, pixels, w, h, color=None):
        self.float_bm = set(pixels)
        self.float_w, self.float_h = w, h
        if color is not None:
            self.float_color = color
        self.update()

    def sizeHint(self):
        return QSize(max(1, self.w + 2 * self.pad) * self.cell + 1,
                     max(1, self.h + 2 * self.pad) * self.cell + 1)

    minimumSizeHint = sizeHint

    def _cellAt(self, pos):
        x = pos.x() // self.cell - self.pad
        y = pos.y() // self.cell - self.pad
        if 0 <= x < self.w and 0 <= y < self.h:
            return (int(x), int(y))
        return None

    def mousePressEvent(self, e):
        if e.button() == Qt.MiddleButton:
            self._pan = e.position()
            return
        c = self._cellAt(e.position().toPoint())
        if c is None:
            return
        if e.button() == Qt.LeftButton:
            self._drag = True
        elif e.button() == Qt.RightButton:
            self._drag = False
        else:
            return
        self.strokeStart.emit()      # 通知编辑器记录本次笔画的撤销前状态
        self._apply(c)

    def mouseMoveEvent(self, e):
        if self._pan is not None:
            if self.scroll is not None:
                hb = self.scroll.horizontalScrollBar()
                vb = self.scroll.verticalScrollBar()
                hb.setValue(hb.value() - int(e.position().x() - self._pan.x()))
                vb.setValue(vb.value() - int(e.position().y() - self._pan.y()))
            self._pan = e.position()
            return
        if self._drag is None:
            return
        c = self._cellAt(e.position().toPoint())
        if c:
            self._apply(c)

    def mouseReleaseEvent(self, e):
        self._drag = None
        self._pan = None

    def wheelEvent(self, e):
        # 光标在画布自身上: 直接用画布局部坐标
        self.zoom_at_viewport(e.angleDelta().y(), e.position().x(), e.position().y())

    def zoom_at_viewport(self, delta_y, vx, vy):
        """以滚动区 viewport 中坐标 (vx,vy) 为焦点缩放,
        空白处(文字外)也能用, 因为滚动区事件过滤器也调用本方法。"""
        if self.scroll is None or self.w == 0:
            return
        hb = self.scroll.horizontalScrollBar()
        vb = self.scroll.verticalScrollBar()
        cg = self.geometry()                 # 画布相对 viewport 的偏移(居中时非零)
        lx = vx - cg.x()                     # 相对画布的坐标
        ly = vy - cg.y()
        cx = lx + hb.value()                 # 内容坐标(缩放前)
        cy = ly + vb.value()
        old = self.cell
        new = old + (3 if delta_y > 0 else -3)
        new = max(4, min(80, new))
        if new == old:
            return
        gx = cx / old
        gy = cy / old
        self.cell = new
        self.updateGeometry()
        self.resize(self.sizeHint())
        hb.setValue(int(gx * new - lx))
        vb.setValue(int(gy * new - ly))
        if self.on_cell_change:
            self.on_cell_change(new)
        self.update()

    def _apply(self, c):
        if self._drag:
            if c in self.pixels:
                return
            self.pixels.add(c)
        else:
            if c not in self.pixels:
                return
            self.pixels.discard(c)
        self.update()
        self.changed.emit()

    def paintEvent(self, ev):
        # 双缓冲: 先在 QImage 上完整绘制, 再贴到控件
        # (PySide6 下把 QPainter 操作放进独立函数会出现第二句 fillRect 失效的怪异,
        #  因此所有绘制逻辑直接内联在此处。)
        sz = self.size()
        img = QImage(sz, QImage.Format_RGB32)
        img.fill(0)
        p = QPainter(img)
        cs = self.cell
        pad = self.pad
        W, H = self.w * cs, self.h * cs
        PW, PH = (self.w + 2 * pad) * cs, (self.h + 2 * pad) * cs
        ox, oy = pad * cs, pad * cs
        # 背景: 外圈 UI 灰, 可编辑的字形包围盒黑底
        p.fillRect(0, 0, PW, PH, QColor(42, 42, 46))
        p.fillRect(ox, oy, W, H, QColor(0, 0, 0))
        # 参考轮廓(粉)
        if self.show_ref and self.ref:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(255, 110, 110, 110))
            for (x, y) in self.ref:
                if 0 <= x < self.w and 0 <= y < self.h:
                    p.drawRect(ox + x * cs, oy + y * cs, cs, cs)
            p.setBrush(Qt.NoBrush)
        # 原始(修改前, 蓝)
        if self.show_orig and self.orig:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(90, 150, 255, 110))
            for (x, y) in self.orig:
                if 0 <= x < self.w and 0 <= y < self.h:
                    p.drawRect(ox + x * cs, oy + y * cs, cs, cs)
            p.setBrush(Qt.NoBrush)
        # 当前墨点(白)
        if self.pixels:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(245, 245, 245))
            for (x, y) in self.pixels:
                p.drawRect(ox + x * cs, oy + y * cs, cs, cs)
            p.setBrush(Qt.NoBrush)   # 后续画线/边框不要意外填充
        # 网格(黑底上需更亮才可见)
        p.setPen(QPen(QColor(75, 75, 95), 1))
        for i in range(self.w + 1):
            p.drawLine(ox + i * cs, oy, ox + i * cs, oy + H)
        for j in range(self.h + 1):
            p.drawLine(ox, oy + j * cs, ox + W, oy + j * cs)
        p.setPen(QPen(QColor(130, 130, 155), 1))
        for i in range(0, self.w + 1, 4):
            p.drawLine(ox + i * cs, oy, ox + i * cs, oy + H)
        for j in range(0, self.h + 1, 4):
            p.drawLine(ox, oy + j * cs, ox + W, oy + j * cs)
        # 基线
        if self.baseline is not None and 0 <= self.baseline <= self.h:
            p.setPen(QPen(QColor(0, 200, 0), 2))
            y = oy + self.baseline * cs
            p.drawLine(ox, y, ox + W, y)
        p.setPen(QPen(QColor(150, 150, 158), 1))
        p.drawRect(ox, oy, W, H)
        # 悬浮参考(居中叠加在字形区域)
        if self.show_float and self.float_bm:
            fw, fh = self.float_w, self.float_h
            if fh > 0:
                scale = max(1, int((self.h * cs * 0.72) / fh))
                dw, dh = fw * scale, fh * scale
                fx = ox + (W - dw) // 2
                fy = oy + (H - dh) // 2
                p.setPen(Qt.NoPen)
                p.setBrush(self.float_color)
                for (x, y) in self.float_bm:
                    if 0 <= x < fw and 0 <= y < fh:
                        p.drawRect(fx + x * scale, fy + y * scale, scale, scale)
                p.setBrush(Qt.NoBrush)
        p.end()
        p2 = QPainter(self)
        p2.drawImage(0, 0, img)

    # 变换
    def shift(self, dx, dy):
        self.pixels = {(x + dx, y + dy) for (x, y) in self.pixels
                       if 0 <= x + dx < self.w and 0 <= y + dy < self.h}
        self.update(); self.changed.emit()

    def clear(self):
        self.pixels = set(); self.update(); self.changed.emit()

    def invert(self):
        self.pixels = {(x, y) for x in range(self.w) for y in range(self.h)
                       if (x, y) not in self.pixels}
        self.update(); self.changed.emit()


# ==================================================================
class Editor(QMainWindow):
    def __init__(self, path):
        super().__init__()
        self.setWindowTitle("内嵌位图修字工具 — " + os.path.basename(path))
        self.resize(1420, 900)
        self.path = path
        self.dirty = False
        self.font = EBDTFont(path)
        self.gi2cp = self.font.cmap_gi()
        self.cp2gi = {}
        for g, cps in self.gi2cp.items():
            for cp in cps:
                self.cp2gi[cp] = g
        try:
            ensure_nobitmap(path)
        except Exception:
            pass
        self.ref = OutlineRef(NOBM) if os.path.exists(NOBM) else None
        self.cur_gi = None
        self.cur_si = 0
        self.undo = {}       # (si,gi) -> [snapshots]
        self.redo = {}
        self.orig_snap = {}  # (si,gi) -> 首次修改前 pixels
        self.tasks = []
        self.task_idx = -1
        self.fix_set = set()      # (gi, si) 待修正集合
        self.modified = set()     # (gi, si) 已修改集合 -> 绿
        self.cur_row = -1         # 当前选中的字号按钮行
        self._build()
        self._load_tasks()
        self.goto_char('\u9631')     # 阱

    # ---------------- UI ----------------
    def _build(self):
        cw = QWidget(); self.setCentralWidget(cw)
        root = QHBoxLayout(cw); root.setContentsMargins(8, 8, 8, 8); root.setSpacing(8)

        # ---- 左栏
        left = QVBoxLayout(); left.setSpacing(6)
        gb = QGroupBox("定位字形")
        gl = QGridLayout(gb)
        self.ed_ch = QLineEdit(); self.ed_ch.setPlaceholderText("输入汉字 / U+9631 / gi=20265")
        self.ed_ch.returnPressed.connect(self._on_locate)
        btn = QPushButton("跳转"); btn.clicked.connect(self._on_locate)
        gl.addWidget(self.ed_ch, 0, 0); gl.addWidget(btn, 0, 1)
        self.lb_info = QLabel("—"); self.lb_info.setWordWrap(True)
        gl.addWidget(self.lb_info, 1, 0, 1, 2)
        left.addWidget(gb)

        gb2 = QGroupBox("待修清单 (_fix_list.json)")
        v2 = QVBoxLayout(gb2)
        self.lst_task = QListWidget()
        self.lst_task.currentRowChanged.connect(self._on_task)
        v2.addWidget(self.lst_task)
        h2 = QHBoxLayout()
        b_prev = QPushButton("← 上一个"); b_prev.clicked.connect(lambda: self._step_task(-1))
        b_next = QPushButton("下一个 →"); b_next.clicked.connect(lambda: self._step_task(1))
        b_done = QPushButton("标记完成"); b_done.clicked.connect(self._mark_done)
        h2.addWidget(b_prev); h2.addWidget(b_next); h2.addWidget(b_done)
        v2.addLayout(h2)
        b_rl = QPushButton("重新载入清单"); b_rl.clicked.connect(self._load_tasks)
        v2.addWidget(b_rl)
        left.addWidget(gb2, 1)
        lw = QWidget(); lw.setLayout(left); lw.setFixedWidth(300)
        root.addWidget(lw)

        # ---- 中间
        mid = QVBoxLayout(); mid.setSpacing(6)
        bar = QHBoxLayout()
        for txt, fn in (("清空", lambda: self._edit(self.canvas.clear)),
                        ("反相", lambda: self._edit(self.canvas.invert)),
                        ("←", lambda: self._edit(lambda: self.canvas.shift(-1, 0))),
                        ("→", lambda: self._edit(lambda: self.canvas.shift(1, 0))),
                        ("↑", lambda: self._edit(lambda: self.canvas.shift(0, -1))),
                        ("↓", lambda: self._edit(lambda: self.canvas.shift(0, 1))),
                        ("撤销", self.do_undo), ("重做", self.do_redo),
                        ("还原本字号", self.restore_one)):
            b = QPushButton(txt); b.clicked.connect(fn); b.setFixedWidth(74 if len(txt) > 2 else 40)
            bar.addWidget(b)
        bar.addStretch(1)
        self.ck_float = QCheckBox("悬浮参考(大字号)"); self.ck_float.setChecked(True)
        self.ck_float.stateChanged.connect(
            lambda: (setattr(self.canvas, 'show_float', self.ck_float.isChecked()), self.canvas.update()))
        bar.addWidget(self.ck_float)
        bar.addWidget(QLabel("格子"))
        self.sp_cell = QSpinBox(); self.sp_cell.setRange(4, 80); self.sp_cell.setValue(22)
        self.sp_cell.valueChanged.connect(self._on_cell)
        bar.addWidget(self.sp_cell)
        mid.addLayout(bar)

        sc = QScrollArea(); sc.setWidgetResizable(False); sc.setAlignment(Qt.AlignCenter)
        self.sc = sc
        self.canvas = PixelCanvas()
        self.canvas.scroll = sc
        self.canvas.on_cell_change = lambda v: self.sp_cell.setValue(v)
        self.canvas.changed.connect(self._on_canvas_changed)
        self.canvas.strokeStart.connect(self._snapshot)
        sc.setWidget(self.canvas)
        sc.installEventFilter(self)          # 让文字外的空白处滚轮也能缩放
        sc.viewport().installEventFilter(self)
        sc.viewport().setStyleSheet("background-color: #2a2a2e;")
        mid.addWidget(sc, 1)

        # 底部: 预览 + 可点击字号数字
        self.lb_prev = QLabel()
        self.lb_prev.setMinimumHeight(120)
        self.lb_prev.setStyleSheet("background:#2a2a2e;border:1px solid #555;")
        self.lb_prev.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        self.size_row = QHBoxLayout(); self.size_row.setSpacing(4)
        self.size_btns = []
        bot = QWidget(); bv = QVBoxLayout(bot); bv.setSpacing(4)
        bv.addWidget(self.lb_prev)
        bl = QLabel("点击下方数字切换/编辑对应字号 (红=待修正 绿=已修改):"); bl.setStyleSheet("font-size:11px;color:#aaa;")
        bv.addWidget(bl)
        bv.addLayout(self.size_row)
        mid.addWidget(bot)
        mw = QWidget(); mw.setLayout(mid)
        root.addWidget(mw, 1)

        # ---- 右栏 (右上角=使用说明)
        right = QVBoxLayout(); right.setSpacing(6)
        gb_help = QGroupBox("使用说明")
        vh = QVBoxLayout(gb_help)
        help_text = ("黑底白字: 左键画白格 / 右键擦黑格\n"
                     "按住拖动 = 连续填充(画笔效果), 按下~松开 = 一步撤销\n"
                     "滚轮 = 以光标为中心放大缩小\n"
                     "中键拖动 = 平移画布\n"
                     "Ctrl+Z 撤销(最多500步)  Ctrl+Y 重做\n"
                     "Ctrl+S 保存字体\n"
                     "方向键 = 平移位图笔画\n"
                     "底部数字 = 切换字号\n"
                     "  红=待修正   绿=已修改\n"
                     "悬浮参考 = 居中叠加最大字号作对照")
        hl = QLabel(help_text); hl.setWordWrap(True); hl.setStyleSheet("font-size:11px;")
        vh.addWidget(hl)
        right.addWidget(gb_help)

        gb4 = QGroupBox("轮廓参考 (真正的字形)")
        v4 = QVBoxLayout(gb4)
        self.ck_ref = QCheckBox("叠加显示(粉)"); self.ck_ref.setChecked(True)
        self.ck_ref.stateChanged.connect(self._refresh_ref)
        self.ck_orig = QCheckBox("叠加修改前(蓝)")
        self.ck_orig.stateChanged.connect(lambda: (setattr(self.canvas, 'show_orig', self.ck_orig.isChecked()), self.canvas.update()))
        v4.addWidget(self.ck_ref); v4.addWidget(self.ck_orig)
        g4 = QGridLayout()
        g4.addWidget(QLabel("阈值"), 0, 0)
        self.sp_th = QSpinBox(); self.sp_th.setRange(1, 254); self.sp_th.setValue(110)
        self.sp_th.valueChanged.connect(self._refresh_ref); g4.addWidget(self.sp_th, 0, 1)
        g4.addWidget(QLabel("dx"), 1, 0)
        self.sp_dx = QSpinBox(); self.sp_dx.setRange(-8, 8)
        self.sp_dx.valueChanged.connect(self._refresh_ref); g4.addWidget(self.sp_dx, 1, 1)
        g4.addWidget(QLabel("dy"), 2, 0)
        self.sp_dy = QSpinBox(); self.sp_dy.setRange(-8, 8)
        self.sp_dy.valueChanged.connect(self._refresh_ref); g4.addWidget(self.sp_dy, 2, 1)
        v4.addLayout(g4)
        b_fill = QPushButton("① 用轮廓填充本字号")
        b_fill.clicked.connect(self.fill_from_outline)
        v4.addWidget(b_fill)
        right.addWidget(gb4)

        gb5 = QGroupBox("从其它字号复制")
        v5 = QVBoxLayout(gb5)
        self.cb_src = QComboBox()
        v5.addWidget(self.cb_src)
        b_sc = QPushButton("② 缩放填充本字号")
        b_sc.clicked.connect(self.fill_from_other)
        v5.addWidget(b_sc)
        right.addWidget(gb5)

        hs = QHBoxLayout()
        b_save = QPushButton("保存为…"); b_save.clicked.connect(self.save_as)
        b_save.setStyleSheet("font-weight:bold;padding:6px;")
        hs.addWidget(b_save)
        right.addLayout(hs)
        rw = QWidget(); rw.setLayout(right); rw.setFixedWidth(280)
        root.addWidget(rw)

        self.setStatusBar(QStatusBar())
        QShortcut(QKeySequence.Undo, self, self.do_undo)
        QShortcut(QKeySequence.Redo, self, self.do_redo)
        QShortcut(QKeySequence.Save, self, self.save_as)

    # ---------------- 事件过滤 ----------------
    def eventFilter(self, obj, ev):
        # 文字外的空白处(滚动区 viewport)滚轮 -> 缩放, 并吞掉避免滚动
        if getattr(self, 'sc', None) is not None and \
                obj in (self.sc, self.sc.viewport()) and ev.type() == QEvent.Wheel:
            self.canvas.zoom_at_viewport(ev.angleDelta().y(),
                                         ev.position().x(), ev.position().y())
            return True
        return super().eventFilter(obj, ev)

    # ---------------- 数据 ----------------
    def _load_tasks(self):
        self.lst_task.clear(); self.tasks = []
        if not os.path.exists(FIXLIST):
            self.lst_task.addItem("(未找到 _fix_list.json)")
            return
        try:
            self.tasks = json.load(open(FIXLIST, encoding='utf-8'))
        except Exception as e:
            self.lst_task.addItem("清单读取失败: %s" % e); return
        for t in self.tasks:
            it = QListWidgetItem("%s  ppem%-3d  显示成「%s」%s"
                                 % (t['ch'], t['ppem'], t['shown_as'],
                                    "  ✔" if t.get('done') else ""))
            self.lst_task.addItem(it)
        # 构建 (si, gi) 待修正集合, 供底部字号按钮标红
        # 注意: 键顺序与 _on_canvas_changed/_snapshot 一致, 均为 (si, gi)
        self.fix_set = {(t['si'], t['gi']) for t in self.tasks
                        if 'gi' in t and 'si' in t}
        self.statusBar().showMessage("清单 %d 项, 待修正 %d 处"
                                     % (len(self.tasks), len(self.fix_set)), 4000)

    def _on_task(self, row):
        if row < 0 or row >= len(self.tasks):
            return
        t = self.tasks[row]
        self.task_idx = row
        self.goto_gi(t['gi'], prefer_si=t['si'])

    def _step_task(self, d):
        n = self.lst_task.count()
        if n == 0: return
        r = max(0, min(n - 1, self.lst_task.currentRow() + d))
        self.lst_task.setCurrentRow(r)

    def _mark_done(self):
        r = self.lst_task.currentRow()
        if 0 <= r < len(self.tasks):
            self.tasks[r]['done'] = True
            t = self.tasks[r]
            self.lst_task.item(r).setText("%s  ppem%-3d  显示成「%s」  ✔"
                                          % (t['ch'], t['ppem'], t['shown_as']))
            try:
                json.dump(self.tasks, open(FIXLIST, 'w', encoding='utf-8'),
                          ensure_ascii=False, indent=1)
            except Exception:
                pass

    def _on_locate(self):
        s = self.ed_ch.text().strip()
        if not s: return
        try:
            if s.lower().startswith('gi='):
                self.goto_gi(int(s[3:])); return
            if s.upper().startswith('U+'):
                self.goto_char(chr(int(s[2:], 16))); return
            self.goto_char(s[0])
        except Exception as e:
            QMessageBox.warning(self, "定位失败", str(e))

    def goto_char(self, ch):
        gi = self.cp2gi.get(ord(ch))
        if gi is None:
            QMessageBox.information(self, "提示", "字体中没有 %s" % ch); return
        self.goto_gi(gi)

    def goto_gi(self, gi, prefer_si=None):
        self.cur_gi = gi
        cps = self.gi2cp.get(gi, [])
        ch = chr(min(cps)) if cps else '?'
        self.lb_info.setText("字形 gi=%d   字符: %s   码位: %s"
                             % (gi, ch, ", ".join("U+%04X" % c for c in sorted(cps)[:6])))
        self.cb_src.clear()
        self.avail = []
        for st in self.font.strikes:
            si = st['idx']
            r = self.font.read_bitmap(si, gi)
            if r is None:
                continue
            self.avail.append(si)
            self.cb_src.addItem("ppem %d (%dx%d)" % (st['ppem'], r['w'], r['h']), si)
        self.float_si = (max(self.avail, key=lambda si: self.font.strikes[si]['ppem'])
                         if self.avail else None)
        self._build_size_buttons()
        target = 0
        if prefer_si is not None and prefer_si in self.avail:
            target = self.avail.index(prefer_si)
        self._on_size(target)

    def _on_size(self, row):
        if self.cur_gi is None or row < 0 or row >= len(self.avail):
            return
        self.cur_si = self.avail[row]
        r = self.font.read_bitmap(self.cur_si, self.cur_gi)
        if r is None:
            self.canvas.set_bitmap(set(), 0, 0)
            self.statusBar().showMessage("该字号无此字形位图")
            return
        m = r['metrics']
        self.canvas.set_bitmap(r['pixels'], r['w'], r['h'], baseline=m['by'])
        key = (self.cur_si, self.cur_gi)
        self.canvas.set_orig(self.orig_snap.get(key, r['pixels']))
        # 字号按钮配色: 红=待修正 绿=已修改 黄框=当前
        self.cur_row = row
        self._refresh_size_buttons()
        # 悬浮参考: 最大字号(非当前)
        if self.float_si is not None and self.float_si != self.cur_si:
            fr = self.font.read_bitmap(self.float_si, self.cur_gi)
            if fr:
                self.canvas.set_float(fr['pixels'], fr['w'], fr['h'])
            else:
                self.canvas.set_float(set(), 0, 0)
        else:
            self.canvas.set_float(set(), 0, 0)
        self._refresh_ref()
        self._refresh_preview()
        self.statusBar().showMessage(
            "ppem=%d  %dx%d  bearing=(%d,%d) adv=%d  imgfmt=%d"
            % (r['ppem'], r['w'], r['h'], m['bx'], m['by'], m['adv'], r['imgfmt']))

    def _build_size_buttons(self):
        for b in self.size_btns:
            b.deleteLater()
        self.size_btns = []
        for i, si in enumerate(self.avail):
            st = self.font.strikes[si]
            b = QPushButton(str(st['ppem']))
            b.setFixedSize(42, 26)
            b.setToolTip("ppem %d" % st['ppem'])
            b.clicked.connect(lambda _, idx=i: self._on_size(idx))
            self.size_row.addWidget(b)
            self.size_btns.append(b)
        self.size_row.addStretch(1)

    def _refresh_size_buttons(self):
        for i, b in enumerate(self.size_btns):
            si = self.avail[i]
            key = (si, self.cur_gi)        # 键顺序 (si, gi) 与修改状态一致
            if key in self.modified:
                bg, fg = "#2e7d32", "#ffffff"        # 绿=已修改
            elif key in self.fix_set:
                bg, fg = "#c62828", "#ffffff"        # 红=待修正
            else:
                bg, fg = "#3a3a3f", "#dddddd"        # 中性
            active = (i == self.cur_row)
            border = "border:2px solid #ffeb3b;" if active else "border:1px solid #444;"
            bold = "font-weight:bold;" if active else ""
            b.setStyleSheet("%sbackground:%s;color:%s;%s" % (bold, bg, fg, border))

    def _refresh_ref(self):
        self.canvas.show_ref = self.ck_ref.isChecked()
        if self.ref is None or self.cur_gi is None:
            self.canvas.set_ref(set()); return
        r = self.font.read_bitmap(self.cur_si, self.cur_gi)
        if r is None:
            self.canvas.set_ref(set()); return
        cps = self.gi2cp.get(self.cur_gi, [])
        if not cps:
            self.canvas.set_ref(set()); return
        ch = chr(min(cps))
        m = r['metrics']
        try:
            pix = self.ref.render_into(ch, r['ppem'], r['w'], r['h'], m['bx'], m['by'],
                                       thresh=self.sp_th.value(),
                                       dx=self.sp_dx.value(), dy=self.sp_dy.value())
        except Exception:
            pix = set()
        self.canvas.set_ref(pix)

    def _refresh_preview(self):
        """底部: 把该字在所有字号的当前位图并排画出来"""
        if self.cur_gi is None: return
        imgs = []
        for st in self.font.strikes:
            r = self.font.read_bitmap(st['idx'], self.cur_gi)
            if r is None or r['w'] == 0: continue
            imgs.append((st['ppem'], r))
        if not imgs:
            self.lb_prev.setPixmap(QPixmap()); return
        pad = 8
        H = max(r['h'] for _, r in imgs) + 24
        W = sum(r['w'] + pad for _, r in imgs) + pad
        img = QImage(W, H, QImage.Format_RGB32)
        img.fill(QColor(0, 0, 0))
        p = QPainter(img)
        x = pad
        f = QFont(); f.setPointSize(6); p.setFont(f)
        for ppem, r in imgs:
            p.setPen(QColor(170, 170, 170))
            p.drawText(x, H - 4, str(ppem))
            p.setPen(Qt.NoPen); p.setBrush(QColor(235, 235, 235))
            for (px, py) in r['pixels']:
                img.setPixelColor(x + px, py + 2, QColor(235, 235, 235))
            x += r['w'] + pad
        p.end()
        self.lb_prev.setPixmap(QPixmap.fromImage(img.scaled(W * 2, H * 2,
                               Qt.KeepAspectRatio, Qt.FastTransformation)))

    # ---------------- 编辑 ----------------
    def _snapshot(self):
        key = (self.cur_si, self.cur_gi)
        if key not in self.orig_snap:
            r = self.font.read_bitmap(self.cur_si, self.cur_gi)
            if r: self.orig_snap[key] = set(r['pixels'])
        st = self.undo.setdefault(key, [])
        st.append(set(self.canvas.pixels))
        if len(st) > 500:
            del st[:len(st) - 500]
        self.redo[key] = []

    def _edit(self, fn):
        if self.cur_gi is None or self.canvas.w == 0: return
        self._snapshot()
        fn()

    def _on_canvas_changed(self):
        key = (self.cur_si, self.cur_gi)
        if key not in self.orig_snap:
            r = self.font.read_bitmap(self.cur_si, self.cur_gi)
            if r: self.orig_snap[key] = set(r['pixels'])
        was_modified = key in self.modified
        self.modified.add(key)            # 标记为已修改 -> 按钮转绿
        try:
            self.font.write_bitmap(self.cur_si, self.cur_gi, self.canvas.pixels)
            self.dirty = True
            self._refresh_preview()
            if not was_modified:
                self._refresh_size_buttons()
        except Exception as e:
            self.statusBar().showMessage("写入失败: %s" % e)

    def do_undo(self):
        key = (self.cur_si, self.cur_gi)
        st = self.undo.get(key)
        if not st:
            self.statusBar().showMessage("无可撤销", 2000); return
        rd = self.redo.setdefault(key, [])
        rd.append(set(self.canvas.pixels))
        if len(rd) > 500:
            del rd[:len(rd) - 500]
        self.canvas.pixels = st.pop()
        self.canvas.update(); self._on_canvas_changed()

    def do_redo(self):
        key = (self.cur_si, self.cur_gi)
        st = self.redo.get(key)
        if not st:
            self.statusBar().showMessage("无可重做", 2000); return
        self.undo.setdefault(key, []).append(set(self.canvas.pixels))
        self.canvas.pixels = st.pop()
        self.canvas.update(); self._on_canvas_changed()

    def restore_one(self):
        key = (self.cur_si, self.cur_gi)
        if key not in self.orig_snap:
            self.statusBar().showMessage("本字号未修改过", 2000); return
        self._snapshot()
        self.canvas.pixels = set(self.orig_snap[key])
        self.canvas.update(); self._on_canvas_changed()

    def fill_from_outline(self):
        if self.cur_gi is None or self.canvas.w == 0: return
        self._snapshot()
        self.canvas.pixels = set(self.canvas.ref)
        self.canvas.update(); self._on_canvas_changed()

    def fill_from_other(self):
        if self.cur_gi is None or self.canvas.w == 0: return
        si = self.cb_src.currentData()
        if si is None or si == self.cur_si: return
        src = self.font.read_bitmap(si, self.cur_gi)
        dst_w, dst_h = self.canvas.w, self.canvas.h
        if not src or src['w'] == 0: return
        self._snapshot()
        out = set()
        for y in range(dst_h):
            sy = min(src['h'] - 1, y * src['h'] // dst_h)
            for x in range(dst_w):
                sx = min(src['w'] - 1, x * src['w'] // dst_w)
                if (sx, sy) in src['pixels']:
                    out.add((x, y))
        self.canvas.pixels = out
        self.canvas.update(); self._on_canvas_changed()

    def _on_cell(self, v):
        self.canvas.cell = v
        self.canvas.updateGeometry(); self.canvas.resize(self.canvas.sizeHint())
        self.canvas.update()

    # ---------------- 保存 ----------------
    def save_as(self):
        d = os.path.join(HERE, "system_fixed.ttf")
        fn, _ = QFileDialog.getSaveFileName(self, "保存修改后的字体", d, "TrueType (*.ttf)")
        if not fn: return
        try:
            self.font.save(fn)
            self.dirty = False
            QMessageBox.information(self, "已保存", "已写出:\n%s" % fn)
        except Exception as e:
            QMessageBox.critical(self, "保存失败", str(e))

    def closeEvent(self, e):
        if self.dirty:
            r = QMessageBox.question(self, "未保存", "有修改尚未保存, 确定退出?",
                                     QMessageBox.Yes | QMessageBox.No)
            if r != QMessageBox.Yes:
                e.ignore(); return
        e.accept()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_FONT
    if not os.path.exists(path):
        print("字体不存在:", path); return 1
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    w = Editor(path)
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
