#include "window.h"
#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
int main(int argc, char **argv) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication app(argc, argv);
    app.setOrganizationName("Fornaio");
    app.setApplicationName("Fornaio");
    app.setWindowIcon(QIcon(":/fornaio.png"));
    {
        // One-time carry-over of preferences saved before the rename.
        QSettings current;
        if (current.allKeys().isEmpty()) {
            QSettings legacy("HairCardAO", "HairCardAO");
            for (const QString &key : legacy.allKeys())
                current.setValue(key, legacy.value(key));
        }
    }
    app.setStyle("Fusion");
    app.setStyleSheet(R"(
QWidget { background:#19222e; color:#e1e8f0; font-family:'Segoe UI'; font-size:12px; }
QPushButton { background:#2c3b4f; border:1px solid #42516a; border-radius:5px; padding:8px 12px; }
QPushButton:hover { background:#3a4d64; } QPushButton:disabled { color:#748095; background:#232e3c; }
QPushButton#primary { background:#4bc4af; color:#10252b; font-weight:700; min-width:110px; }
QPushButton#primary:disabled { background:#2c655f; color:#9aafab; }
QComboBox,QSpinBox,QDoubleSpinBox { background:#101923; border:1px solid #3a495d; border-radius:3px; padding:5px; min-height:20px; }
QProgressBar { border:1px solid #39485d; border-radius:4px; text-align:center; background:#101923; min-height:22px; }
QProgressBar::chunk { background:#358d82; } QTabBar::tab { padding:9px 18px; background:#233145; } QTabBar::tab:selected { background:#3a4b61; }
QScrollArea { border:0; } QToolTip { background:#26364c; color:#eef3ff; border:1px solid #506078; }
QCheckBox::indicator, QTableView::indicator, QTreeView::indicator { width:14px; height:14px; border:1px solid #7f93ad; border-radius:3px; background:#101923; }
QCheckBox::indicator:hover, QTableView::indicator:hover { border-color:#4bc4af; }
QCheckBox::indicator:checked, QTableView::indicator:checked, QTreeView::indicator:checked { background:#4bc4af; border-color:#bff3ea; }
QCheckBox::indicator:disabled { border-color:#4a5568; background:#232e3c; }
QLineEdit { background:#101923; border:1px solid #3a495d; border-radius:3px; padding:5px; min-height:20px; }
QAbstractItemView QLineEdit { padding:0 3px; min-height:0; border-radius:0; }
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus { border-color:#4bc4af; }
QComboBox::drop-down { subcontrol-origin:border; subcontrol-position:top right; width:22px; border:0; border-left:1px solid #3a495d; }
QComboBox::down-arrow { image:url(:/arrow_down.png); width:10px; height:6px; }
QComboBox::down-arrow:disabled { image:url(:/arrow_down_disabled.png); }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin:border; width:22px; border:0; border-left:1px solid #3a495d; background:transparent; }
QAbstractSpinBox::up-button { subcontrol-position:top right; }
QAbstractSpinBox::down-button { subcontrol-position:bottom right; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover, QComboBox::drop-down:hover { background:#2c3b4f; }
QAbstractSpinBox::up-arrow { image:url(:/arrow_up.png); width:10px; height:6px; }
QAbstractSpinBox::down-arrow { image:url(:/arrow_down.png); width:10px; height:6px; }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::up-arrow:off { image:url(:/arrow_up_disabled.png); }
QAbstractSpinBox::down-arrow:disabled, QAbstractSpinBox::down-arrow:off { image:url(:/arrow_down_disabled.png); }
QAbstractItemView { background:#101923; alternate-background-color:#141f2c; border:1px solid #3a495d; selection-background-color:#2f5f5a; selection-color:#eef3ff; outline:0; }
QTableView { gridline-color:#2c3b4f; }
QHeaderView { background:#233145; border:0; }
QHeaderView::section { background:#233145; color:#c9d4e0; border:0; border-right:1px solid #19222e; border-bottom:1px solid #3a495d; padding:4px 6px; }
QTableCornerButton::section { background:#233145; border:0; }
QScrollBar:vertical { background:#151d28; width:12px; margin:0; border:0; }
QScrollBar:horizontal { background:#151d28; height:12px; margin:0; border:0; }
QScrollBar::handle { background:#3a4b61; border-radius:4px; margin:2px; }
QScrollBar::handle:vertical { min-height:28px; } QScrollBar::handle:horizontal { min-width:28px; }
QScrollBar::handle:hover { background:#4f6580; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; border:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:none; }
QSplitter::handle { background:#19222e; } QSplitter::handle:hover { background:#2c3b4f; }
)");
    Window window;
    window.show();
    auto args = app.arguments();
    int mesh = args.indexOf("--mesh"), mask = args.indexOf("--mask");
    if (mesh >= 0 && mesh + 1 < args.size())
        window.loadInputs(args[mesh + 1], mask >= 0 && mask + 1 < args.size() ? args[mask + 1] : QString());
    int occluder = args.indexOf("--occluder");
    if (occluder >= 0 && occluder + 1 < args.size())
        window.loadOccluder(args[occluder + 1]);
    int capture = args.indexOf("--smoke-capture"), bake = args.indexOf("--smoke-bake");
    if (bake >= 0 && bake + 1 < args.size())
        QTimer::singleShot(300, &window, [&window, args, bake, capture] {
            window.smokeBake(args[bake + 1],
                             capture >= 0 && capture + 1 < args.size() ? args[capture + 1] : QString());
        });
    else if (capture >= 0 && capture + 1 < args.size())
        QTimer::singleShot(2000, &window, [&window, &app, args, capture] {
            window.smokeCapture(args[capture + 1]);
            app.quit();
        });
    return app.exec();
}
