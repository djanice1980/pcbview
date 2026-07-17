#include "app/theme.h"

#include <QPalette>
#include <QStyleFactory>

namespace pcbview::app::theme {

void apply(QApplication& app) {
    // Fusion is Qt's own style; unlike the native Windows style it honours a
    // custom palette completely, which is what makes a consistent dark theme
    // possible without fighting the platform.
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, QColor(kBg1));
    p.setColor(QPalette::WindowText, QColor(kText));
    p.setColor(QPalette::Base, QColor(kBg0));
    p.setColor(QPalette::AlternateBase, QColor(kBg1));
    p.setColor(QPalette::Text, QColor(kText));
    p.setColor(QPalette::Button, QColor(kBg2));
    p.setColor(QPalette::ButtonText, QColor(kText));
    p.setColor(QPalette::Highlight, QColor(kAccent));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase, QColor(kBg2));
    p.setColor(QPalette::ToolTipText, QColor(kText));
    p.setColor(QPalette::PlaceholderText, QColor(kTextDim));
    p.setColor(QPalette::Link, QColor(kAccent));

    // Disabled states, or greyed-out controls stay fully legible and look enabled.
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#5a5a5a"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#5a5a5a"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#5a5a5a"));
    app.setPalette(p);

    app.setStyleSheet(QString(R"(
        QMainWindow, QDockWidget { background: %1; }
        QMenuBar { background: %1; border-bottom: 1px solid %4; }
        QMenuBar::item { padding: 4px 9px; background: transparent; }
        QMenuBar::item:selected { background: %6; }
        QMenu { background: %2; border: 1px solid %4; padding: 4px; }
        QMenu::item { padding: 5px 26px 5px 22px; }
        QMenu::item:selected { background: %6; }
        QMenu::separator { height: 1px; background: %4; margin: 4px 6px; }

        QToolBar { background: %2; border: 0; border-bottom: 1px solid %4; spacing: 2px; padding: 3px; }
        QToolButton { background: %1; border: 1px solid %4; border-radius: 2px; padding: 3px 7px; }
        QToolButton:hover { background: %3; }
        QToolButton:checked { background: %6; border-color: %6; color: #fff; }

        QDockWidget::title { background: %2; padding: 5px 8px; border-bottom: 1px solid %4;
                             font-size: 10px; font-weight: 600; }
        QTreeWidget, QTreeView { background: %1; border: 0; outline: 0;
                                 alternate-background-color: %1; }
        QTreeWidget::item { padding: 3px 2px; }
        QTreeWidget::item:selected { background: %6; color: #fff; }
        QHeaderView::section { background: %2; border: 0; border-bottom: 1px solid %4;
                               padding: 4px; color: %5; font-size: 10px; font-weight: 600; }

        QStatusBar { background: %2; border-top: 1px solid %4; color: %5; }
        QStatusBar::item { border: 0; }
        QLabel { color: %7; }
        QGroupBox { border: 1px solid %4; border-radius: 2px; margin-top: 16px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 7px; padding: 0 4px;
                           color: %5; font-size: 10px; font-weight: 600; }

        QSlider::groove:horizontal { height: 3px; background: %4; border-radius: 2px; }
        QSlider::handle:horizontal { width: 11px; margin: -5px 0; border-radius: 2px; background: %5; }
        QSlider::handle:horizontal:hover { background: %6; }
        QSlider::sub-page:horizontal { background: %6; border-radius: 2px; }

        QComboBox, QDoubleSpinBox, QSpinBox { background: %1; border: 1px solid %4;
                                              border-radius: 2px; padding: 2px 6px; }
        QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #6a6a6a;
                               background: %1; border-radius: 2px; }
        QCheckBox::indicator:checked { background: %6; border-color: %6; }
        QSplitter::handle { background: %4; }
        QScrollBar:vertical { background: %1; width: 11px; }
        QScrollBar::handle:vertical { background: #4a4a4a; border-radius: 5px; min-height: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
    )")
                           .arg(kBg1)     // %1
                           .arg(kBg2)     // %2
                           .arg(kBg3)     // %3
                           .arg(kLine)    // %4
                           .arg(kTextDim) // %5
                           .arg(kAccent)  // %6
                           .arg(kText));  // %7
}

}  // namespace pcbview::app::theme
