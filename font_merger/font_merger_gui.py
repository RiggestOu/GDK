# -*- coding: utf-8 -*-
"""
字体合并工具 —— PySide6 图形界面
================================
把多套字体（简体/繁体/日文/韩文/英文 …）合并为一个字体文件，并尽量压缩。

功能：
  * 拖拽任意数量字体文件进列表（也支持「添加」按钮 / 文件对话框）。
  * 选择输出路径与字体族名。
  * 点击「开始合并」：后台线程扫描并构建，进度条实时显示。
  * 出现编码冲突（同一码位多源字形不同）时，自动弹出冲突筛选窗口，
    每个冲突码位都带字形预览，用户可逐个（或批量默认）选择采用哪个源。

依赖：fontTools、PySide6。
"""

import os
import sys
import traceback

from PySide6.QtCore import QThread, Signal, Qt, QSize
from PySide6.QtGui import (
    QPainter, QPainterPath, QColor, QBrush, QPen, QFont, QDragEnterEvent, QDropEvent,
)
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QListWidget,
    QListWidgetItem, QPushButton, QLineEdit, QLabel, QFileDialog, QProgressBar,
    QPlainTextEdit, QGroupBox, QRadioButton, QButtonGroup, QComboBox, QDialog,
    QDialogButtonBox, QMessageBox, QSizePolicy, QScrollArea, QFrame,
)

from font_merge_core import FontMerger, glyph_outline_to_path, MergeConflict


# =================================================================== 字形预览
class GlyphPreview(QFrame):
    """用 QPainterPath 把字形画出来（自带适配缩放 + 基线参考线）。"""

    def __init__(self, parent=None, size=140):
        super().__init__(parent)
        self.setFixedSize(size, size)
        self.path = None
        self.bbox = None
        self.setFrameStyle(QFrame.StyledPanel | QFrame.Sunken)
        self._bg = QColor(248, 248, 248)
        self._fg = QColor(20, 20, 20)
        self._line = QColor(200, 200, 200)

    def setGlyph(self, path, bbox):
        self.path = path
        self.bbox = bbox
        self.update()

    def clearGlyph(self):
        self.path = None
        self.bbox = None
        self.update()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.fillRect(self.rect(), self._bg)
        if self.path is None or self.bbox is None:
            return
        x0, y0, x1, y1 = self.bbox
        if x1 <= x0 or y1 <= y0:
            return
        w = self.width()
        h = self.height()
        bw = x1 - x0
        bh = y1 - y0
        s = min(w / bw, h / bh) * 0.82
        cx = (x0 + x1) / 2.0
        cy = (y0 + y1) / 2.0
        p.translate(w / 2.0, h / 2.0)
        p.scale(s, s)
        p.translate(-cx, -cy)
        # 基线参考线（y=0）
        p.setPen(QPen(self._line, 1.0 / s))
        p.drawLine(x0 - 50, 0, x1 + 50, 0)
        # 字形填充
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(self._fg))
        p.drawPath(self.path)


# =================================================================== 冲突筛选窗口
class ConflictResolver(QDialog):
    """冲突筛选窗口：左侧码位列表，右侧逐个预览候选字形并选择。"""

    def __init__(self, merger, conflicts, source_names, parent=None):
        super().__init__(parent)
        self.merger = merger
        self.conflicts = conflicts
        self.source_names = source_names
        self.selections = {}            # codepoint -> font_index（用户单独覆盖）
        self._button_groups = {}        # codepoint -> QButtonGroup
        self._current_cp = None

        self.setWindowTitle("编码冲突筛选")
        self.resize(720, 520)
        self._build_ui()

    def _build_ui(self):
        root = QVBoxLayout(self)

        info = QLabel(
            "检测到 %d 处编码冲突（同一码位在多个字体里字形不同）。\n"
            "请逐个选择要采用的字体；未单独选择者将采用下方「默认采用」指定的源。"
            % len(self.conflicts)
        )
        info.setWordWrap(True)
        root.addWidget(info)

        split = QHBoxLayout()
        root.addLayout(split, 1)

        # ---- 左侧码位列表
        self.list = QListWidget()
        self.list.setFixedWidth(170)
        for c in self.conflicts:
            label = "%s  U+%04X" % (c.char if c.char else "·", c.codepoint)
            item = QListWidgetItem(label)
            item.setData(Qt.UserRole, c.codepoint)
            self.list.addItem(item)
        self.list.currentItemChanged.connect(self._on_select)
        split.addWidget(self.list)

        # ---- 右侧详情（可滚动）
        self.detail = QWidget()
        self.detail_layout = QVBoxLayout(self.detail)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.detail)
        split.addWidget(scroll, 1)

        # ---- 底部默认源 + 按钮
        bottom = QHBoxLayout()
        bottom.addWidget(QLabel("未单独选择的冲突默认采用："))
        self.defaultCombo = QComboBox()
        self.defaultCombo.addItems(self.source_names)
        self.defaultCombo.setCurrentIndex(0)
        bottom.addWidget(self.defaultCombo)
        bottom.addStretch(1)
        root.addLayout(bottom)

        btns = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        root.addWidget(btns)

        if self.conflicts:
            self.list.setCurrentRow(0)

    def _on_select(self, current, _prev):
        if current is None:
            return
        cp = current.data(Qt.UserRole)
        self._populate_detail(cp)

    def _populate_detail(self, cp):
        # 清空旧内容
        while self.detail_layout.count():
            item = self.detail_layout.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()
        self._button_groups.pop(cp, None)

        conflict = next(c for c in self.conflicts if c.codepoint == cp)
        self._current_cp = cp

        title = QLabel("码位 U+%04X  「%s」" % (cp, conflict.char if conflict.char else "·"))
        title.setFont(QFont("", 12, QFont.Bold))
        self.detail_layout.addWidget(title)

        grp = QButtonGroup(self)
        grp.setExclusive(True)
        self._button_groups[cp] = grp
        grp.buttonClicked.connect(lambda _b, c=cp: self._on_radio(c))

        for cand in conflict.candidates:
            fi = cand["font_index"]
            row = QHBoxLayout()
            row.setAlignment(Qt.AlignLeft)

            path, bbox = glyph_outline_to_path(
                self.merger.sources[fi].font, cand["glyph_name"], 1.0
            )
            prev = GlyphPreview(size=120)
            prev.setGlyph(path, bbox)
            row.addWidget(prev)

            col = QVBoxLayout()
            rb = QRadioButton(self.source_names[fi])
            rb.setChecked(self.selections.get(cp) == fi)
            grp.addButton(rb, fi)
            col.addWidget(rb)
            col.addWidget(QLabel("字形名: %s" % cand["glyph_name"]))
            row.addLayout(col)
            row.addStretch(1)
            self.detail_layout.addLayout(row)

        note = QLabel("（勾选上方某个字体即代表该码位采用此源；也可直接靠底部「默认采用」统一处理）")
        note.setWordWrap(True)
        self.detail_layout.addWidget(note)
        self.detail_layout.addStretch(1)

    def _on_radio(self, cp):
        grp = self._button_groups.get(cp)
        if grp is None:
            return
        checked = grp.checkedId()
        if checked >= 0:
            self.selections[cp] = checked

    def get_choices(self):
        """生成 {codepoint: font_index}。"""
        default_fi = self.defaultCombo.currentIndex()
        choices = {}
        for c in self.conflicts:
            fi = self.selections.get(c.codepoint)
            if fi is None:
                fi = default_fi
            cand_fis = [cd["font_index"] for cd in c.candidates]
            if fi not in cand_fis:
                fi = c.candidates[0]["font_index"]
            choices[c.codepoint] = fi
        return choices


# =================================================================== 后台扫描线程
class ScanWorker(QThread):
    loaded = Signal(int, int, str)          # 已加载数, 总数, 名称
    scanProgress = Signal(int, int)         # 已扫描, 总数
    finished = Signal(object, object)       # conflicts, stats
    error = Signal(str)

    def __init__(self, merger, font_paths):
        super().__init__()
        self.merger = merger
        self.font_paths = font_paths

    def run(self):
        try:
            self.merger.clear()
            total = len(self.font_paths)
            for i, path in enumerate(self.font_paths, 1):
                try:
                    src = self.merger.add_font(path)
                    self.loaded.emit(i, total, src.name)
                except Exception as e:
                    self.error.emit("加载失败 %s: %s" % (os.path.basename(path), e))
                    return
            conflicts, stats = self.merger.scan(
                progress_cb=lambda d, t: self.scanProgress.emit(d, t)
            )
            self.finished.emit(conflicts, stats)
        except Exception as e:
            self.error.emit("扫描出错: %s\n%s" % (e, traceback.format_exc()))


# =================================================================== 后台构建线程
class BuildWorker(QThread):
    progress = Signal(int, int)             # 已构建, 总数
    finished = Signal(object)               # result dict
    error = Signal(str)

    def __init__(self, merger, choices, output_path, family_name):
        super().__init__()
        self.merger = merger
        self.choices = choices
        self.output_path = output_path
        self.family_name = family_name

    def run(self):
        try:
            res = self.merger.build(
                self.choices, self.output_path, self.family_name,
                progress_cb=lambda d, t: self.progress.emit(d, t),
            )
            self.finished.emit(res)
        except Exception as e:
            self.error.emit("构建出错: %s\n%s" % (e, traceback.format_exc()))


# =================================================================== 可拖拽列表
class DropList(QListWidget):
    filesDropped = Signal(list)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAcceptDrops(True)
        self.setSelectionMode(QListWidget.ExtendedSelection)

    def dragEnterEvent(self, ev: QDragEnterEvent):
        if ev.mimeData().hasUrls():
            ev.acceptProposedAction()
        else:
            ev.ignore()

    def dragMoveEvent(self, ev):
        if ev.mimeData().hasUrls():
            ev.acceptProposedAction()
        else:
            ev.ignore()

    def dropEvent(self, ev: QDropEvent):
        urls = ev.mimeData().urls()
        paths = [u.toLocalFile() for u in urls if u.isLocalFile()]
        if paths:
            ev.acceptProposedAction()
            self.filesDropped.emit(paths)
        else:
            ev.ignore()


# =================================================================== 主窗口
class MainWindow(QMainWindow):
    FONT_FILTER = "字体文件 (*.ttf *.otf *.ttc *.otc);;所有文件 (*.*)"

    def __init__(self):
        super().__init__()
        self.merger = FontMerger()
        self.font_paths = []
        self.scan_worker = None
        self.build_worker = None
        self._init_ui()

    def _init_ui(self):
        self.setWindowTitle("字体合并工具")
        self.resize(760, 560)
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # ---------- 字体列表区
        list_box = QGroupBox("字体列表（拖拽文件到此处，或点「添加字体」）")
        lb_layout = QVBoxLayout(list_box)
        self.list = DropList()
        self.list.filesDropped.connect(self._on_dropped)
        lb_layout.addWidget(self.list, 1)

        btn_row = QHBoxLayout()
        self.btn_add = QPushButton("添加字体")
        self.btn_add.clicked.connect(self._add_fonts)
        self.btn_remove = QPushButton("移除选中")
        self.btn_remove.clicked.connect(self._remove_selected)
        self.btn_clear = QPushButton("清空")
        self.btn_clear.clicked.connect(self._clear_list)
        btn_row.addWidget(self.btn_add)
        btn_row.addWidget(self.btn_remove)
        btn_row.addWidget(self.btn_clear)
        btn_row.addStretch(1)
        lb_layout.addLayout(btn_row)
        root.addWidget(list_box, 1)

        # ---------- 输出设置区
        set_box = QGroupBox("输出设置")
        set_layout = QVBoxLayout(set_box)

        h1 = QHBoxLayout()
        h1.addWidget(QLabel("字体族名:"))
        self.family_edit = QLineEdit("Merged Font")
        h1.addWidget(self.family_edit, 1)
        set_layout.addLayout(h1)

        h2 = QHBoxLayout()
        h2.addWidget(QLabel("输出文件:"))
        self.output_edit = QLineEdit("")
        self.output_edit.setPlaceholderText("选择保存位置，例如 merged.ttf")
        h2.addWidget(self.output_edit, 1)
        self.btn_browse = QPushButton("浏览…")
        self.btn_browse.clicked.connect(self._browse_output)
        h2.addWidget(self.btn_browse)
        set_layout.addLayout(h2)
        root.addWidget(set_box)

        # ---------- 进度 + 开始
        self.progress = QProgressBar()
        self.progress.setValue(0)
        root.addWidget(self.progress)

        run_row = QHBoxLayout()
        self.btn_start = QPushButton("开始合并")
        self.btn_start.clicked.connect(self._start)
        self.btn_start.setDefault(True)
        run_row.addWidget(self.btn_start)
        run_row.addStretch(1)
        root.addLayout(run_row)

        # ---------- 日志
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(140)
        root.addWidget(self.log)

    # -------------------------------------------------- 列表操作
    def _on_dropped(self, paths):
        self._add_paths(paths)

    def _add_fonts(self):
        paths, _ = QFileDialog.getOpenFileNames(self, "选择字体文件", "", self.FONT_FILTER)
        if paths:
            self._add_paths(paths)

    def _add_paths(self, paths):
        exts = (".ttf", ".otf", ".ttc", ".otc")
        added = 0
        for p in paths:
            if not os.path.isfile(p):
                continue
            if os.path.splitext(p)[1].lower() not in exts:
                self._log("跳过非字体文件: %s" % os.path.basename(p))
                continue
            if p in self.font_paths:
                continue
            self.font_paths.append(p)
            self.list.addItem(os.path.basename(p))
            added += 1
        if added:
            self._log("已加入 %d 个字体文件。" % added)

    def _remove_selected(self):
        for item in self.list.selectedItems():
            row = self.list.row(item)
            del self.font_paths[row]
            self.list.takeItem(row)
        self._log("已移除选中项。")

    def _clear_list(self):
        self.font_paths = []
        self.list.clear()
        self._log("列表已清空。")

    def _browse_output(self):
        path, _ = QFileDialog.getSaveFileName(
            self, "保存合并字体", "merged.ttf", "TrueType 字体 (*.ttf)"
        )
        if path:
            if not path.lower().endswith(".ttf"):
                path += ".ttf"
            self.output_edit.setText(path)

    # -------------------------------------------------- 运行流程
    def _start(self):
        if not self.font_paths:
            QMessageBox.warning(self, "提示", "请先添加至少一个字体文件。")
            return
        out = self.output_edit.text().strip()
        if not out:
            QMessageBox.warning(self, "提示", "请先选择输出文件路径。")
            return
        self.output_path = out
        self.family_name = self.family_edit.text().strip() or "Merged Font"

        self._set_running(True)
        self.progress.setValue(0)
        self._log("开始加载并扫描 %d 个字体…" % len(self.font_paths))

        self.scan_worker = ScanWorker(self.merger, self.font_paths)
        self.scan_worker.loaded.connect(
            lambda i, t, n: self._log("  [%d/%d] 已加载: %s" % (i, t, n))
        )
        self.scan_worker.scanProgress.connect(self._on_scan_progress)
        self.scan_worker.finished.connect(self._on_scan_finished)
        self.scan_worker.error.connect(self._on_error)
        self.scan_worker.start()

    def _on_scan_progress(self, done, total):
        self.progress.setValue(int(done / total * 50))  # 扫描占进度条前 50%

    def _on_scan_finished(self, conflicts, stats):
        self.progress.setValue(50)
        self._log("扫描完成：共 %d 个码位，其中 %d 处冲突。"
                  % (stats["total_codepoints"], stats["conflict_count"]))
        for s in stats["sources"]:
            self._log("  源: %s [%s] upm=%d 字符数=%d"
                      % (s["name"], s["fmt"], s["upm"], s["chars"]))

        if conflicts:
            names = [s["name"] for s in stats["sources"]]
            dlg = ConflictResolver(self.merger, conflicts, names, self)
            if dlg.exec() == QDialog.Accepted:
                choices = dlg.get_choices()
                self._log("已收集 %d 处冲突的选择，开始构建…" % len(choices))
                self._run_build(choices)
            else:
                self._log("已取消。")
                self._set_running(False)
        else:
            self._log("无冲突，直接开始构建…")
            self._run_build({})

    def _run_build(self, choices):
        self.build_worker = BuildWorker(
            self.merger, choices, self.output_path, self.family_name
        )
        self.build_worker.progress.connect(self._on_build_progress)
        self.build_worker.finished.connect(self._on_build_finished)
        self.build_worker.error.connect(self._on_error)
        self.build_worker.start()

    def _on_build_progress(self, done, total):
        base = 50
        if total > 0:
            self.progress.setValue(base + int(done / total * 50))

    def _on_build_finished(self, res):
        self.progress.setValue(100)
        self._log("构建完成！")
        self._log("  输出: %s" % res["output"])
        self._log("  码位总数: %d" % res["total_codepoints"])
        self._log("  去重后字形数: %d" % res["unique_glyphs"])
        self._log("  因去重节省的字形: %d" % res["dedup_saved"])
        self._log("  目标 unitsPerEm: %d" % res["target_upm"])
        QMessageBox.information(
            self, "完成",
            "字体合并完成！\n输出文件：%s\n去重后字形：%d（节省 %d 个）"
            % (res["output"], res["unique_glyphs"], res["dedup_saved"])
        )
        self._set_running(False)

    def _on_error(self, msg):
        self._log("错误：%s" % msg)
        QMessageBox.critical(self, "出错", msg)
        self._set_running(False)

    def _set_running(self, running):
        for w in (self.btn_add, self.btn_remove, self.btn_clear,
                  self.btn_browse, self.btn_start):
            w.setEnabled(not running)

    def _log(self, text):
        self.log.appendPlainText(text)


# =================================================================== 入口
def main():
    app = QApplication(sys.argv)
    app.setApplicationName("字体合并工具")
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
