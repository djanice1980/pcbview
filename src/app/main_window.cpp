#include "app/main_window.h"

#include "app/settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMovie>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QDir>
#include <QImage>
#include <QPageLayout>
#include <QPainter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include <functional>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>

#include "app/collapsible_dock.h"
#include "geom/connectivity.h"
#include "app/component_import.h"
#include "app/theme.h"
#include "io/gerber/gerber_project.h"
#include "io/kicad/kicad_importer.h"
#include "model/validate.h"

namespace pcbview::app {
namespace {

constexpr int kMaxRecent = 8;
// Fixed rows in the properties tree; layer-specific rows are appended after.
constexpr int kFixedPropertyRows = 12;

// A QLabel that runs a callback on click -- carries the Ko-fi badge in the
// Help menu and the animated one in the About dialog (QPushButton cannot host
// a QMovie, and these are the only clickable images in the app).
class ClickLabel : public QLabel {
public:
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onClick) onClick();
        QLabel::mousePressEvent(e);
    }
};

// Toolbar icons are DRAWN, not shipped as bitmaps: they stay crisp at any DPI
// or icon size, follow the theme colour, and keep the asset list to the app
// icon alone. Painted at 64px and scaled down by Qt.
QPixmap paintIcon(const std::function<void(QPainter&)>& draw) {
    constexpr int kS = 64;
    QPixmap pm(kS, kS);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    draw(p);
    p.end();
    return pm;
}

// A ruler laid diagonally, with alternating long/short graduations -- the
// measure-distance tool.
QIcon rulerIcon() {
    return QIcon(paintIcon([](QPainter& p) {
        const QColor ink(theme::kText);
        p.translate(32, 32);
        p.rotate(-45);
        p.translate(-32, -32);

        QRectF body(6, 25, 52, 14);
        p.setPen(QPen(ink, 3.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(body, 2.5, 2.5);
        // Graduations hang from the top edge; every other one is full length.
        p.setPen(QPen(ink, 2.6));
        for (int i = 1; i <= 5; ++i) {
            const qreal x = body.left() + body.width() * i / 6.0;
            const qreal len = (i % 2 == 1) ? 8.0 : 4.5;
            p.drawLine(QPointF(x, body.top()), QPointF(x, body.top() + len));
        }
    }));
}

// A speed (rafter) square: right triangle with the pivot fence along one leg
// -- the board-dimensions overlay.
QIcon speedSquareIcon() {
    return QIcon(paintIcon([](QPainter& p) {
        const QColor ink(theme::kText);
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        // Outer triangle: right angle bottom-left.
        path.moveTo(10, 55);
        path.lineTo(10, 11);
        path.lineTo(54, 55);
        path.closeSubpath();
        // The window cut through the middle, as on a real speed square.
        path.moveTo(21, 45);
        path.lineTo(21, 27);
        path.lineTo(39, 45);
        path.closeSubpath();

        p.setPen(QPen(ink, 3.2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Graduations along the vertical leg (the fence edge).
        p.setPen(QPen(ink, 2.4));
        for (int i = 1; i <= 3; ++i) {
            const qreal y = 55 - 44.0 * i / 4.0;
            p.drawLine(QPointF(10, y), QPointF(10 + 6.5, y));
        }
    }));
}

// Layer swatches. Not decoration -- in a stackup tree the colour is how you find
// the layer, so it has to match what the viewport draws.
QColor swatchFor(const QString& name) {
    if (name.endsWith(".SilkS")) return QColor(230, 230, 222);
    if (name.endsWith(".Mask")) return QColor(45, 107, 63);
    if (name == "F.Cu" || name == "B.Cu") return QColor(201, 162, 39);
    if (name.startsWith("In")) return QColor(138, 115, 32);
    if (name == "substrate") return QColor(138, 115, 85);
    return QColor(140, 140, 140);
}

QIcon swatchIcon(const QColor& c) {
    QPixmap pm(10, 10);
    pm.fill(c);
    return QIcon(pm);
}

QString mm(double v, int decimals = 3) { return QString::number(v, 'f', decimals); }

}  // namespace

MainWindow::MainWindow(const QString& path) {
    setWindowTitle("pcbview");
    resize(1600, 950);
    setAcceptDrops(true);

    // A native QWindow draws over any Qt widget in the same area, so an overlaid
    // "no board" hint would be invisible. Stack them and swap instead.
    placeholder_ = new QLabel(
        "<div align='center' style='color:#8f8f8f'>"
        "<div style='font-size:15px'>No board loaded</div>"
        "<div style='font-size:12px; margin-top:10px'>"
        "Open with <b>Ctrl+O</b>, or drop a <b>.kicad_pcb</b>, a gerber "
        "<b>.zip</b>, a <b>.gbrjob</b>, or a gerber folder here"
        "</div></div>");
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setStyleSheet(QString("background:%1").arg(theme::kBg0));

    stack_ = new QStackedWidget;
    stack_->addWidget(placeholder_);  // index 0
    buildViewport();                  // index 1: viewport container
    setCentralWidget(stack_);

    buildMenus();
    buildToolbar();
    buildStackupDock();
    buildPropertiesDock();
    buildNetDock();
    buildStatusBar();

    rebuildRecentMenu();
    // PCBVIEW_OPEN lets a headless run load a package the CLI arg can't express
    // (a gerber folder), so the GUI path is verifiable without synthetic input.
    QString openPath = path;
    if (openPath.isEmpty())
        openPath = qEnvironmentVariable("PCBVIEW_OPEN");
    if (!openPath.isEmpty()) loadBoard(openPath);

    // Set the opening view without touching the keyboard. Exists because the
    // GUI cannot be driven by synthetic input from a non-foreground process --
    // SetForegroundWindow is denied, so SendKeys lands in another application
    // entirely and every keyboard-driven "test" silently passes against the
    // default view. This is the only honest way to verify view-dependent
    // rendering (e.g. that the bottom of the board is actually lit).
    const QString startView =
        qEnvironmentVariable("PCBVIEW_START_VIEW").toLower();
    if (startView == "top") viewport_->setViewTop();
    else if (startView == "bottom") viewport_->setViewBottom();
    else if (startView == "iso") viewport_->setViewIso();

    // Headless orthographic-projection hook (the O toggle can't be driven by
    // synthetic input; exists to verify ortho in the traced modes).
    // Headless path-trace hook. PT is a menu toggle the capture harness cannot
    // reach, so verifying anything PT-specific (the net chase, emission, the
    // denoiser) needs a way in from the environment.
    if (qEnvironmentVariable("PCBVIEW_START_PT").toInt() != 0)
        QTimer::singleShot(200, this, [this] { viewport_->setPathTracing(true); });

    if (qEnvironmentVariable("PCBVIEW_START_ORTHO").toInt() != 0) {
        viewport_->camera().orthographic = true;
        if (orthoAction_) {
            const QSignalBlocker block(orthoAction_);
            orthoAction_->setChecked(true);
        }
    }

    // Headless roll hook (right-drag vertical can't be synthesised); radians.
    if (qEnvironmentVariableIsSet("PCBVIEW_START_ROLL"))
        viewport_->camera().roll =
            qEnvironmentVariable("PCBVIEW_START_ROLL").toFloat();
    // Explicit orbit angles in radians, for views the three presets cannot
    // express -- a grazing pitch is how the board EDGE gets inspected.
    if (qEnvironmentVariableIsSet("PCBVIEW_START_YAW"))
        viewport_->camera().yaw =
            qEnvironmentVariable("PCBVIEW_START_YAW").toFloat();
    if (qEnvironmentVariableIsSet("PCBVIEW_START_PITCH"))
        viewport_->camera().pitch =
            qEnvironmentVariable("PCBVIEW_START_PITCH").toFloat();

    // Headless connectivity hook. The Infer button is a click the capture
    // harness cannot make, so nothing about pseudo-nets is verifiable without
    // this. Runs before PCBVIEW_NET so a derived net can then be highlighted
    // by name.
    if (qEnvironmentVariable("PCBVIEW_INFER_NETS").toInt() != 0)
        QTimer::singleShot(300, this, [this] { inferNetsFromCopper(); });

    // Headless net-highlight hook: highlight a net BY NAME (the UI picker
    // cannot be driven by synthetic input).
    if (qEnvironmentVariableIsSet("PCBVIEW_NET")) {
        // Comma-separated for multi-net highlighting, mirroring Ctrl+click.
        const QStringList want =
            qEnvironmentVariable("PCBVIEW_NET").split(',', Qt::SkipEmptyParts);
        QTimer::singleShot(700, this, [this, want] {
            highlightedNets_.clear();
            for (const QString& w : want) {
                for (size_t i = 0; i < mesh_.nets.size(); ++i) {
                    if (QString::fromStdString(mesh_.nets[i].name) ==
                        w.trimmed()) {
                        highlightedNets_.push_back(static_cast<int>(i));
                        break;
                    }
                }
            }
            applyNetHighlights();
        });
    }

    // Headless measurement hook: pin a measurement between two world points
    // (x1,y1,z1,x2,y2,z2 in mm) -- mouse picks can't be synthesised.
    if (qEnvironmentVariableIsSet("PCBVIEW_MEASURE")) {
        const QStringList p = qEnvironmentVariable("PCBVIEW_MEASURE").split(',');
        if (p.size() == 6) {
            viewport_->setMeasurement(p[0].toFloat(), p[1].toFloat(),
                                      p[2].toFloat(), p[3].toFloat(),
                                      p[4].toFloat(), p[5].toFloat());
        }
    }

    // Headless close-up: set the orbit distance AFTER first expose --
    // initialise()'s frameBoard() would clobber a pre-expose value (the usual
    // before-first-frame trap). Needed to reproduce artifacts that only show
    // when zoomed tight (the camera cannot be driven by synthetic input).
    if (qEnvironmentVariableIsSet("PCBVIEW_START_DISTANCE")) {
        const float d = qEnvironmentVariable("PCBVIEW_START_DISTANCE").toFloat();
        QTimer::singleShot(600, this, [this, d] {
            viewport_->camera().distance = d;
            viewport_->requestUpdate();
        });
    }

    if (qEnvironmentVariableIsSet("PCBVIEW_START_EXPLODE")) {
        viewport_->setExplodeProgress(
            qEnvironmentVariable("PCBVIEW_START_EXPLODE").toFloat(),
            /*snap=*/true);
    }
    // Headless hooks for the appearance controls (the dialog can't be driven by
    // synthetic input). PCBVIEW_THICKNESS=0.3 ; PCBVIEW_SUBSTRATE=r,g,b,opacity
    if (qEnvironmentVariableIsSet("PCBVIEW_THICKNESS")) {
        thicknessOverride_ = qEnvironmentVariable("PCBVIEW_THICKNESS").toDouble();
        reassemble();
    }
    if (qEnvironmentVariableIsSet("PCBVIEW_SUBSTRATE")) {
        const QStringList p =
            qEnvironmentVariable("PCBVIEW_SUBSTRATE").split(',');
        if (p.size() == 4) {
            subColor_ = {p[0].toFloat(), p[1].toFloat(), p[2].toFloat()};
            subOpacity_ = p[3].toFloat();
        }
    }
    if (qEnvironmentVariableIsSet("PCBVIEW_MASK")) {
        const QStringList p = qEnvironmentVariable("PCBVIEW_MASK").split(',');
        if (p.size() >= 3)
            maskColor_ = {p[0].toFloat(), p[1].toFloat(), p[2].toFloat()};
        if (p.size() == 4) maskOpacity_ = p[3].toFloat();  // r,g,b,opacity
    }
    applyAppearance();  // no-op if renderer absent; boardUploaded retries

    // PCBVIEW_CAPTURE=<out.bmp> grabs the actual presented Vulkan frame and
    // quits -- the honest, lock-independent way to verify GPU rendering (this is
    // what Renderer::requestCapture is for). Wait a beat so the swapchain exists
    // and any explode easing has settled before the grab.
    if (qEnvironmentVariableIsSet("PCBVIEW_CAPTURE")) {
        const QString cap = qEnvironmentVariable("PCBVIEW_CAPTURE");
        // Default 1500ms lets the swapchain settle; a longer delay lets the path
        // tracer accumulate and denoise several times before the grab.
        const int delay = qEnvironmentVariableIsSet("PCBVIEW_CAPTURE_DELAY_MS")
                              ? qEnvironmentVariable("PCBVIEW_CAPTURE_DELAY_MS").toInt()
                              : 1500;
        // PCBVIEW_CAPTURE_SCENE=1 grabs the OFFSCREEN scene image instead of
        // the swapchain -- the high-resolution export path, which the Save
        // Screenshot dialog cannot be driven into headlessly.
        const bool scene =
            qEnvironmentVariable("PCBVIEW_CAPTURE_SCENE").toInt() != 0;
        QTimer::singleShot(delay, this, [this, cap, scene] {
            if (viewport_->renderer())
                viewport_->renderer()->requestCapture(cap.toStdString(), scene);
            viewport_->requestUpdate();
            QTimer::singleShot(1000, qApp, &QApplication::quit);
        });
    }

    // One-time support notice. Shown once ever (persisted flag), never during
    // a headless run -- a modal dialog would deadlock every capture script.
    if (!appSettings().value("kofiNoticeShown", false).toBool() &&
        !qEnvironmentVariableIsSet("PCBVIEW_CAPTURE") &&
        !qEnvironmentVariableIsSet("PCBVIEW_GPU_REPORT") &&
        !qEnvironmentVariableIsSet("PCBVIEW_ART_DUMP")) {
        appSettings().setValue("kofiNoticeShown", true);
        QTimer::singleShot(1200, this, [this] {
            QMessageBox box(this);
            box.setWindowTitle("Support pcbview");
            box.setTextFormat(Qt::RichText);
            box.setText(
                "<p><b>pcbview is free, open-source software.</b></p>"
                "<p>If it's useful to you, you can support its development "
                "on Ko-fi.</p>"
                "<p style='color:#8f8f8f'>You can always find this later "
                "under <b>Help &rarr; Support on Ko-fi</b>, or in the About "
                "dialog. This notice won't appear again.</p>");
            QPushButton* support =
                box.addButton("Support on Ko-fi", QMessageBox::AcceptRole);
            box.addButton("Close", QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == support) {
                QDesktopServices::openUrl(
                    QUrl("https://ko-fi.com/P5P81EV1M0"));
            }
        });
    }
}

void MainWindow::buildViewport() {
    viewport_ = new VulkanWindow(loaded_ ? &mesh_ : nullptr);
    viewportContainer_ = QWidget::createWindowContainer(viewport_, this);
    viewportContainer_->setMinimumSize(480, 360);
    viewportContainer_->setFocusPolicy(Qt::StrongFocus);
    stack_->insertWidget(1, viewportContainer_);  // placeholder stays index 0

    connect(viewport_, &VulkanWindow::frameRendered, this,
            &MainWindow::onFrameRendered);
    connect(viewport_, &VulkanWindow::statusMessage, this,
            [this](const QString& t) { toolbarInfo_->setText(t); });
    // The renderer's parts only exist after upload, so reconcile then -- and
    // push the substrate appearance, which the renderer resets to default tan on
    // every uploadBoard.
    connect(viewport_, &VulkanWindow::boardUploaded, this, [this] {
        populateStackup();
        applyAppearance();
        // A rebuilt viewport (device switch) brings a fresh renderer at 1.00x,
        // so carry the scale across. Only on a REBUILD: doing it on every load
        // would overwrite the PCBVIEW_RENDER_SCALE headless override, which is
        // applied before the first frame.
        if (pendingRenderScale_ > 0.0f && viewport_->renderer()) {
            viewport_->renderer()->setRenderScale(pendingRenderScale_);
            pendingRenderScale_ = -1.0f;
        }
    });
    connect(viewport_, &VulkanWindow::explodeChanged, this,
            [this](float progress, float maxProgress) {
                if (progress <= 0.0f || maxProgress <= 0.0f) {
                    explodeLabel_->setText("off");
                } else {
                    // Round up: part-way through lifting ring 2 still reads
                    // "2 / 5", which matches what you are looking at.
                    explodeLabel_->setText(
                        QString("%1 / %2")
                            .arg(static_cast<int>(std::ceil(progress)))
                            .arg(static_cast<int>(std::ceil(maxProgress))));
                }
                explodeLabel_->setStyleSheet(
                    QString("color:%1")
                        .arg(progress > 0.0f ? theme::kText : theme::kTextDim));
            });

    // The viewport is a native QWindow, so its keys never reach the QAction
    // shortcut machinery. It forwards what it does not consume; we match the
    // menu shortcuts here. No double-firing: when focus is on a real widget the
    // QAction fires and this signal is never emitted.
    connect(viewport_, &VulkanWindow::unhandledKey, this,
            [this](int key, Qt::KeyboardModifiers mods) {
                const QKeySequence seq(key | static_cast<int>(mods.toInt()));
                if (seq == QKeySequence(QKeySequence::Open)) onOpen();
                else if (seq == QKeySequence(QKeySequence::Refresh)) onReload();
                else if (seq == QKeySequence(QKeySequence::Quit)) close();
            });

    // QUEUED: the request is emitted from inside the viewport's own method, and
    // the rebuild deletes that viewport -- it must not die under its own stack
    // frame.
    connect(viewport_, &VulkanWindow::viewportRebuildRequired, this,
            &MainWindow::rebuildViewport, Qt::QueuedConnection);

    // Measurement readout in the status bar; menu checkbox follows the M key.
    connect(viewport_, &VulkanWindow::measureReadout, this,
            [this](const QString& t) {
                if (t.isEmpty()) statusBar()->clearMessage();
                else statusBar()->showMessage(t);
            });
    connect(viewport_, &VulkanWindow::measureModeChanged, this,
            [this](bool on) {
                if (measureAction_) {
                    const QSignalBlocker block(measureAction_);
                    measureAction_->setChecked(on);
                }
            });
    // Clicking the board picks the net under the cursor.
    connect(viewport_, &VulkanWindow::netPicked, this,
            [this](int net, bool add) {
                if (add) toggleHighlightNet(net);
                else highlightNet(net);
            });
    // A rebuilt viewport (device switch) starts with no highlight.
    if (highlightedNet_ >= 0 && viewport_->renderer())
        viewport_->renderer()->setHighlightNet(highlightedNet_);
    // The O key flips the camera directly, so mirror it onto the action
    // (blocked, or the action would drive the camera straight back).
    connect(viewport_, &VulkanWindow::orthoChanged, this, [this](bool on) {
        if (orthoAction_) {
            const QSignalBlocker block(orthoAction_);
            orthoAction_->setChecked(on);
        }
    });
    // The menus are built once, before the first viewport exists; a REBUILT
    // viewport reloads the persisted dims toggle, so reflect it back.
    if (dimsAction_) {
        const QSignalBlocker block(dimsAction_);
        dimsAction_->setChecked(viewport_->dimensionsOverlay());
    }
    if (measureAction_ && measureAction_->isChecked()) {
        viewport_->setMeasureMode(true);
    }
}

void MainWindow::rebuildViewport() {
    // Carry the view over; every toggle/preference reloads from the persisted
    // settings inside the new window's initialise().
    const Camera cam = viewport_->camera();
    const float explode = viewport_->explodeProgress();
    // The internal-resolution scale lives on the renderer, which is about to
    // die with this viewport; hand it to the replacement.
    if (viewport_->renderer())
        pendingRenderScale_ = viewport_->renderer()->renderScale();

    VulkanWindow* oldViewport = viewport_;
    QWidget* oldContainer = viewportContainer_;
    stack_->removeWidget(oldContainer);

    buildViewport();
    if (loaded_) stack_->setCurrentWidget(viewportContainer_);

    // The new window frames the board on first expose (frameBoard snaps), which
    // would clobber the carried-over camera -- restore it after the first
    // upload. One-shot connection.
    auto* conn = new QMetaObject::Connection;
    *conn = connect(viewport_, &VulkanWindow::boardUploaded, this,
                    [this, cam, explode, conn] {
                        disconnect(*conn);
                        delete conn;
                        viewport_->camera() = cam;
                        viewport_->setExplodeProgress(explode, /*snap=*/true);
                        viewport_->requestUpdate();
                    });

    // The old device's numbers must not linger on screen -- an on-demand raster
    // can go idle before the every-15th-frame refresh fires, leaving the CPU
    // device's frame time displayed under a GPU render (or vice versa).
    statusPerf_->setText(QString());
    frameCounter_ = 14;  // next rendered frame refreshes the readout

    // The container owns the QWindow; deleting it tears down the old surface,
    // swapchain and device with it (releaseResources via PlatformSurface).
    oldContainer->deleteLater();
    Q_UNUSED(oldViewport);
}

bool MainWindow::loadBoard(const QString& path) {
    QApplication::setOverrideCursor(Qt::WaitCursor);

    // A .kicad_pcb has full semantics; anything else (a .zip, a folder, a
    // .gbrjob) is a Gerber package, which resolves straight to LayerArt with no
    // nets, pads, or components to recover.
    const bool isKicad = path.endsWith(".kicad_pcb", Qt::CaseInsensitive);

    BoardModel board;
    geom::LayerArt art;
    bool gerber = false;
    QStringList warnings;
    try {
        if (isKicad) {
            board = kicad::importPcb(path.toStdString());
            art = geom::buildLayerArt(board);
            for (const std::string& w : board.warnings)
                warnings << QString::fromStdString(w);
        } else {
            gerber = true;
            art = gerber::importPackage(path.toStdString());
            for (const std::string& w : art.warnings)
                warnings << QString::fromStdString(w);
        }
    } catch (const std::exception& e) {
        QApplication::restoreOverrideCursor();
        // Leave the current board alone -- a failed open should not blank the
        // viewer.
        QMessageBox::critical(this, "Cannot open",
                              QString("<b>%1</b><p>%2")
                                  .arg(QFileInfo(path).fileName())
                                  .arg(QString::fromUtf8(e.what())));
        return false;
    }

    fromGerber_ = gerber;
    board_ = std::move(board);
    baseArt_ = std::move(art);
    path_ = path;
    loaded_ = true;

    // PCBVIEW_ART_DUMP=<file>: write outline/drill/layer stats for a headless
    // sanity check (counts, areas, bounding boxes). Diagnostic only.
    if (qEnvironmentVariableIsSet("PCBVIEW_ART_DUMP")) {
        std::ofstream d(
            qEnvironmentVariable("PCBVIEW_ART_DUMP").toStdString());
        auto bbox = [](const Clipper2Lib::Paths64& ps) {
            long long xmin = std::numeric_limits<long long>::max();
            long long ymin = std::numeric_limits<long long>::max();
            long long xmax = std::numeric_limits<long long>::min();
            long long ymax = std::numeric_limits<long long>::min();
            for (const auto& p : ps)
                for (const auto& pt : p) {
                    xmin = std::min<long long>(xmin, pt.x);
                    xmax = std::max<long long>(xmax, pt.x);
                    ymin = std::min<long long>(ymin, pt.y);
                    ymax = std::max<long long>(ymax, pt.y);
                }
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "bbox[%.2f,%.2f .. %.2f,%.2f]mm",
                          xmin / 1e6, ymin / 1e6, xmax / 1e6, ymax / 1e6);
            return std::string(buf);
        };
        d << "outline paths=" << baseArt_.outline.size()
          << " area=" << Clipper2Lib::Area(baseArt_.outline) / 1e12 << "mm2 "
          << bbox(baseArt_.outline) << "\n";
        for (size_t i = 0; i < baseArt_.outline.size(); ++i) {
            const auto& p = baseArt_.outline[i];
            Clipper2Lib::Paths64 one{p};
            d << "  outline[" << i << "] pts=" << p.size()
              << " area=" << Clipper2Lib::Area(p) / 1e12 << "mm2 "
              << bbox(one) << "\n";
        }
        d << "drills paths=" << baseArt_.drills.size()
          << " area=" << Clipper2Lib::Area(baseArt_.drills) / 1e12 << "mm2 "
          << bbox(baseArt_.drills) << "\n";
        d << "barrels paths=" << baseArt_.barrels.size()
          << " area=" << Clipper2Lib::Area(baseArt_.barrels) / 1e12 << "mm2\n";
        for (const auto& al : baseArt_.layers)
            d << "layer " << al.name << " kind=" << int(al.kind)
              << " z=" << al.z << " paths=" << al.art.size()
              << " area=" << Clipper2Lib::Area(al.art) / 1e12 << "mm2 "
              << bbox(al.art) << "\n";
        for (const auto& w : baseArt_.warnings) d << "warn: " << w << "\n";
    }

    // A new board keeps any thickness override the user set, unless it is now
    // nonsensical; the override applies on top of baseArt_.
    mesh_ = thicknessOverride_ > 0.0
                ? [&] {
                      geom::LayerArt a = baseArt_;
                      geom::applyThickness(a, thicknessOverride_);
                      return geom::assemble(a);
                  }()
                : geom::assemble(baseArt_);

    // Component bodies, KiCad only. Sourced through kicad-cli (which owns the
    // STEP tessellation) into a cached GLB -- see component_import. Gerbers have
    // no component identity to render. Never fatal: on any failure the board
    // still shows and the reason lands in the status bar.
    componentParts_.clear();
    QString componentMsg;
    if (isKicad && qEnvironmentVariableIsEmpty("PCBVIEW_NO_COMPONENTS")) {
        ComponentImport ci =
            importComponents(path.toStdString(), mesh_.bounds);
        componentParts_ = std::move(ci.parts);
        componentMsg = QString::fromStdString(ci.message);
    }
    appendComponents();

    stack_->setCurrentIndex(1);  // reveal the viewport
    viewport_->setMesh(&mesh_);
    setWindowTitle(QString("pcbview  —  %1").arg(QFileInfo(path).fileName()));

    populateStackup();
    populateProperties();
    populateNets();
    updateStatus();
    rememberRecent(path);
    QApplication::restoreOverrideCursor();

    warnings.removeDuplicates();
    // Keep the full list: the status line can only show the first, and a
    // transient one-of-five with no way to see the rest tells the user a
    // problem exists while withholding what it is.
    importWarnings_ = warnings;
    importNotes_.clear();
    for (const std::string& n : baseArt_.notes)
        importNotes_ << QString::fromStdString(n);
    importNotes_.removeDuplicates();

    // Also to stderr. On a windowed build this goes nowhere unless someone
    // redirects it, so it costs a normal user nothing -- but it makes the
    // list retrievable from a headless run, and "paste me the output" is a
    // far better support question than "what did the status bar say".
    for (const QString& w : warnings)
        std::fprintf(stderr, "[import warning] %s\n", w.toUtf8().constData());
    for (const QString& n : importNotes_)
        std::fprintf(stderr, "[import note] %s\n", n.toUtf8().constData());
    std::fflush(stderr);

    // Lead with warnings. A package where everything was understood must not
    // look like it has problems -- notes get a count, not an alarm.
    if (!warnings.isEmpty()) {
        statusBar()->showMessage(
            QString("%1 import warning(s): %2  —  View ▸ Import report…")
                .arg(warnings.size())
                .arg(warnings.first()),
            10000);
    } else if (!importNotes_.isEmpty()) {
        statusBar()->showMessage(
            QString("Imported cleanly — %1 file(s) recognised but not "
                    "rendered.  View ▸ Import report…")
                .arg(importNotes_.size()),
            8000);
    } else if (!componentMsg.isEmpty()) {
        statusBar()->showMessage(componentMsg, 10000);
    }
    return true;
}

void MainWindow::onOpen() {
    // A gerber package can be a zip, a .gbrjob, or a folder. Offer a file dialog
    // for the first two and a directory dialog on request.
    const QString start = QFileInfo(path_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, "Open board or gerbers", start,
        "All supported (*.kicad_pcb *.zip *.gbrjob);;"
        "KiCad PCB (*.kicad_pcb);;Gerber package (*.zip *.gbrjob);;All files (*)");
    if (!path.isEmpty()) loadBoard(path);
}

void MainWindow::onOpenFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Open gerber folder", QFileInfo(path_).absolutePath());
    if (!dir.isEmpty()) loadBoard(dir);
}

void MainWindow::onReload() {
    if (!path_.isEmpty()) loadBoard(path_);
}

void MainWindow::applyAppearance() {
    if (!viewport_->renderer()) return;
    viewport_->renderer()->setSubstrateAppearance(subColor_[0], subColor_[1],
                                                  subColor_[2], subOpacity_);
    viewport_->renderer()->setMaskColor(maskColor_[0], maskColor_[1],
                                        maskColor_[2], maskOpacity_);
    viewport_->renderer()->setComponentShine(fxComponentShine_ / 100.0f);
    viewport_->renderer()->setPadShine(fxPadShine_ / 100.0f);
    viewport_->renderer()->setShadowSoftness(fxShadowSoftness_ / 100.0f);
    viewport_->renderer()->setNetGlow(fxNetGlow_ * 0.16f);
    viewport_->requestUpdate();
}

void MainWindow::reassemble() {
    if (!loaded_) return;
    geom::LayerArt a = baseArt_;
    if (thicknessOverride_ > 0.0) geom::applyThickness(a, thicknessOverride_);
    mesh_ = geom::assemble(a);
    appendComponents();          // survive the rebuild; ride the new surface
    viewport_->setMesh(&mesh_);  // re-uploads; substrate appearance persists
    updateStatus();
    populateProperties();
}

void MainWindow::appendComponents() {
    if (componentParts_.empty()) return;

    // Components were placed at the design thickness. If the user is previewing a
    // different finished thickness, the top surface has moved, so shift
    // top-mounted parts by the delta to keep them seated. Bottom parts sit on the
    // fixed Z=0 face and never move.
    const double design = baseArt_.thickness;
    const double eff = thicknessOverride_ > 0.0 ? thicknessOverride_ : design;
    const float dz = static_cast<float>(eff - design);

    for (const geom::Part& src : componentParts_) {
        geom::Part p = src;  // fresh copy per assemble
        if (dz != 0.0f && p.mountSide > 0) {
            for (geom::Vertex& v : p.mesh.vertices) v.position[2] += dz;
        }
        for (const geom::Vertex& v : p.mesh.vertices) {
            for (int k = 0; k < 3; ++k) {
                mesh_.bounds.min[k] =
                    std::min(mesh_.bounds.min[k], static_cast<double>(v.position[k]));
                mesh_.bounds.max[k] =
                    std::max(mesh_.bounds.max[k], static_cast<double>(v.position[k]));
            }
        }
        mesh_.parts.push_back(std::move(p));
    }
}

void MainWindow::grabFrame(std::function<void(const QImage&)> then,
                           float exportScale) {
    if (!viewport_->renderer()) {
        statusBar()->showMessage("Renderer not ready yet", 3000);
        return;
    }
    vk::Renderer* r = viewport_->renderer();

    // An export renders the scene at exactly `exportScale` and grabs the
    // OFFSCREEN target rather than the window, so the result is that
    // resolution regardless of where the internal-res slider happens to sit --
    // asking for 1x while the slider is at 4x gives a crisp window-size
    // render, not a downsample of the 4x one. Changing the scale restarts any
    // accumulation, so a traced mode must be allowed to converge before the
    // grab or the export is a one-sample noise field. exportScale <= 0 means
    // "leave everything alone and grab the window" -- what print uses.
    const bool explicitScale = exportScale > 0.0f;
    const float savedScale = r->renderScale();
    if (explicitScale) {
        r->setRenderScale(exportScale);  // no-op when already at that scale
        statusBar()->showMessage("Rendering export…");
    }

    const QString tmp = QDir::temp().filePath("pcbview_grab.bmp");
    QFile::remove(tmp);

    // State shared with the frame handler: whether the capture has been asked
    // for yet, and a frame budget so a mode that never settles cannot hang the
    // export forever.
    auto requested = std::make_shared<bool>(false);
    auto frames = std::make_shared<int>(0);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(
        viewport_, &VulkanWindow::frameRendered, this,
        [this, tmp, then, conn, requested, frames, explicitScale, savedScale]() {
            vk::Renderer* r = viewport_->renderer();
            if (!r) return;
            if (!*requested) {
                // Let a progressive mode converge first. The cap is generous
                // (a CPU path trace at 4x is genuinely slow) but finite.
                if (r->accumulating() && ++*frames < 3000) {
                    viewport_->requestUpdate();
                    return;
                }
                *requested = true;
                r->requestCapture(tmp.toStdString(), explicitScale);
                viewport_->requestUpdate();
                return;
            }
            if (!QFileInfo::exists(tmp)) return;  // capture frame not run yet
            QObject::disconnect(*conn);
            QImage img(tmp);
            QFile::remove(tmp);
            if (explicitScale) r->setRenderScale(savedScale);
            if (img.isNull()) {
                statusBar()->showMessage("Frame capture failed", 4000);
                return;
            }
            // Run `then` on the NEXT event-loop turn, not inside this
            // frameRendered emission -- it fires from within render(), and
            // opening a modal dialog (print preview) re-entrantly there is
            // what left the preview blank/broken.
            QMetaObject::invokeMethod(
                this, [then, img]() { then(img); }, Qt::QueuedConnection);
        });
    viewport_->requestUpdate();
}

void MainWindow::onSaveScreenshot() {
    if (!loaded_) {
        statusBar()->showMessage("Open a board first", 3000);
        return;
    }
    const QString suggested = QFileInfo(path_).absolutePath() + "/" +
                              QFileInfo(path_).completeBaseName() + ".png";
    const QString path = QFileDialog::getSaveFileName(
        this, "Save screenshot", suggested,
        "PNG image (*.png);;JPEG image (*.jpg)");
    if (path.isEmpty()) return;

    // Resolution picker. The internal-resolution machinery already renders the
    // scene off-screen at a multiple of the window, so an export is just that
    // scale plus grabbing the offscreen target -- which is also why the
    // choices are expressed as multiples and the real pixel size is spelled
    // out rather than left to the user to multiply.
    float exportScale = 1.0f;
    {
        const QSize win = viewport_->renderer()
                              ? QSize(static_cast<int>(
                                          viewport_->renderer()->windowExtent().width),
                                      static_cast<int>(
                                          viewport_->renderer()->windowExtent().height))
                              : size();
        // "As on screen" is whatever the INTERNAL RESOLUTION slider is set to,
        // not 1x -- with the slider at 4x the window already shows a 4x render
        // downsampled, so labelling 1x "as on screen" was simply wrong.
        const float current =
            viewport_->renderer() ? viewport_->renderer()->renderScale() : 1.0f;

        std::vector<float> choices = {1.0f, 2.0f, 3.0f, 4.0f};
        // The slider is continuous, so the current scale is often not one of
        // the presets; offer it explicitly rather than silently changing it.
        const bool haveCurrent =
            std::any_of(choices.begin(), choices.end(), [current](float m) {
                return std::abs(m - current) < 0.01f;
            });
        if (!haveCurrent) {
            choices.push_back(current);
            std::sort(choices.begin(), choices.end());
        }

        QStringList labels;
        int currentIndex = 0;
        for (size_t i = 0; i < choices.size(); ++i) {
            const float m = choices[i];
            const bool isCurrent = std::abs(m - current) < 0.01f;
            if (isCurrent) currentIndex = static_cast<int>(i);
            labels << QString("%1×  —  %2 × %3 px%4")
                          .arg(QString::number(m, 'g', 3))
                          .arg(static_cast<int>(win.width() * m))
                          .arg(static_cast<int>(win.height() * m))
                          .arg(isCurrent ? "  (current — as on screen)" : "");
        }

        bool ok = false;
        // A traced mode has to re-converge whenever the export size DIFFERS
        // from what is already rendered; picking the current scale costs
        // nothing. On the CPU device that re-converge is minutes, not seconds.
        const bool traced = viewport_->pathTracing() ||
                            (viewport_->cpuRender() && viewport_->rayTracing());
        const QString note =
            traced ? (viewport_->cpuRender()
                          ? "\n\nNote: any size other than the current one makes "
                            "the CPU renderer re-converge, which can take "
                            "several minutes at the larger scales."
                          : "\n\nNote: any size other than the current one makes "
                            "the path tracer re-converge, which takes a few "
                            "seconds.")
                   : QString();
        const QString choice = QInputDialog::getItem(
            this, "Export resolution", "Render the board at:" + note, labels,
            currentIndex, false, &ok);
        if (!ok) return;
        const int idx = labels.indexOf(choice);
        exportScale = choices[idx < 0 ? static_cast<size_t>(currentIndex)
                                      : static_cast<size_t>(idx)];
    }

    grabFrame(
        [this, path](const QImage& img) {
            if (img.save(path)) {
                statusBar()->showMessage(
                    QString("Saved %1  (%2 × %3)")
                        .arg(path).arg(img.width()).arg(img.height()),
                    6000);
            } else {
                statusBar()->showMessage("Could not save " + path, 5000);
            }
        },
        exportScale);
}

void MainWindow::printView(int mode) {
    if (!loaded_) {
        statusBar()->showMessage("Open a board first", 3000);
        return;
    }

    // For the flat modes, snap to a top orthographic fit, capture, then restore
    // the camera the user was looking at. "As shown" leaves the view untouched.
    const Camera saved = viewport_->camera();
    const bool flat = (mode == 1 || mode == 2);
    if (flat) {
        Camera& c = viewport_->camera();
        c.orthographic = true;
        c.yaw = 0.0f;
        c.pitch = 1.57079633f;  // straight down
        viewport_->frameBoard(/*snap=*/true);
    }

    grabFrame([this, mode, flat, saved](const QImage& img) {
        double mmPerPixel = 0.0;
        if (mode == 2) {
            // Orthographic: the view spans 2*halfH mm vertically, so each
            // pixel is a fixed number of mm -- exactly what a 1:1 print
            // needs. halfH comes from VulkanWindow so the print scale can
            // never drift from the projection actually rendered.
            const double halfH = viewport_->orthoHalfHeight();
            const double halfW = halfH * (static_cast<double>(img.width()) /
                                          std::max(1, img.height()));
            mmPerPixel = 2.0 * halfW / std::max(1, img.width());
        }
        sendToPrinter(img, mode == 2, mmPerPixel);
        if (flat) {
            viewport_->camera() = saved;
            viewport_->requestUpdate();
        }
    });
}

void MainWindow::sendToPrinter(const QImage& img, bool originalSize,
                               double mmPerPixel) {
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageOrientation(img.width() >= img.height()
                                   ? QPageLayout::Landscape
                                   : QPageLayout::Portrait);

    // A real preview: the dialog re-asks us to paint whenever the page/zoom
    // changes, and prints from its own toolbar.
    QPrintPreviewDialog preview(&printer, this);
    preview.setWindowTitle("Print preview");
    preview.resize(900, 700);
    connect(&preview, &QPrintPreviewDialog::paintRequested, this,
            [img, originalSize, mmPerPixel](QPrinter* p) {
                QPainter painter(p);
                const QRectF page = p->pageRect(QPrinter::DevicePixel);
                if (originalSize && mmPerPixel > 0.0) {
                    // 1:1 physical scale: mm per pixel -> inches -> device dots.
                    const double dpi = p->resolution();
                    const double wDots = (img.width() * mmPerPixel / 25.4) * dpi;
                    const double hDots = (img.height() * mmPerPixel / 25.4) * dpi;
                    QRectF target(0, 0, wDots, hDots);
                    target.moveCenter(page.center());
                    painter.drawImage(target, img);
                } else {
                    QSizeF sz = img.size();
                    sz.scale(page.size(), Qt::KeepAspectRatio);
                    QRectF target(QPointF(0, 0), sz);
                    target.moveCenter(page.center());
                    painter.drawImage(target, img);
                }
                painter.end();
            });
    preview.exec();
}

namespace {
bool droppable(const QString& localPath) {
    return localPath.endsWith(".kicad_pcb", Qt::CaseInsensitive) ||
           localPath.endsWith(".zip", Qt::CaseInsensitive) ||
           localPath.endsWith(".gbrjob", Qt::CaseInsensitive) ||
           QFileInfo(localPath).isDir();
}
}  // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasUrls()) return;
    for (const QUrl& url : e->mimeData()->urls()) {
        if (url.isLocalFile() && droppable(url.toLocalFile())) {
            e->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const QUrl& url : e->mimeData()->urls()) {
        if (url.isLocalFile() && droppable(url.toLocalFile())) {
            loadBoard(url.toLocalFile());
            e->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::rememberRecent(const QString& path) {
    QSettings s = appSettings();
    QStringList recent = s.value("recentFiles").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecent) recent.removeLast();
    s.setValue("recentFiles", recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();

    QSettings s = appSettings();
    QStringList recent = s.value("recentFiles").toStringList();
    // Drop entries whose file has since been moved or deleted.
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString& p) { return !QFileInfo::exists(p); }),
                 recent.end());
    s.setValue("recentFiles", recent);

    if (recent.isEmpty()) {
        recentMenu_->addAction("(none)")->setEnabled(false);
        return;
    }
    for (const QString& p : recent) {
        QAction* a = recentMenu_->addAction(QFileInfo(p).fileName());
        a->setToolTip(p);
        connect(a, &QAction::triggered, this, [this, p] { loadBoard(p); });
    }
    recentMenu_->addSeparator();
    recentMenu_->addAction("Clear", this, [this] {
        appSettings().setValue("recentFiles", QStringList());
        rebuildRecentMenu();
    });
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu("&File");
    file->addAction("&Open…", QKeySequence::Open, this, &MainWindow::onOpen);
    file->addAction("Open gerber &folder…", this, &MainWindow::onOpenFolder);
    recentMenu_ = file->addMenu("Open &Recent");
    file->addAction("&Reload", QKeySequence::Refresh, this, &MainWindow::onReload);
    file->addSeparator();
    file->addAction("Save &screenshot…", QKeySequence(Qt::CTRL | Qt::Key_S), this,
                    &MainWindow::onSaveScreenshot);
    QMenu* printMenu = file->addMenu("&Print");
    printMenu->addAction("As &shown…", QKeySequence::Print, this,
                         [this] { printView(0); });
    printMenu->addAction("&Flat (overhead)…", this, [this] { printView(1); });
    printMenu->addAction("Flat at &original size (1:1)…", this,
                         [this] { printView(2); });
    file->addSeparator();
    file->addAction("E&xit", QKeySequence::Quit, this, &QWidget::close);

    // Every menu shortcut must be ApplicationShortcut, not the default
    // WindowShortcut.
    //
    // The viewport is a native QWindow embedded via createWindowContainer, so
    // keyboard focus lives outside this widget's window. A WindowShortcut is
    // only matched when focus is inside the same window, which means Ctrl+O and
    // friends silently never fire -- and the viewport has focus essentially all
    // the time. Do this last, after every action exists.
    const auto applyContext = [this] {
        for (QAction* a : findChildren<QAction*>()) {
            if (!a->shortcut().isEmpty()) {
                a->setShortcutContext(Qt::ApplicationShortcut);
            }
        }
    };

    QMenu* view = menuBar()->addMenu("&View");
    view->addAction("&Top", QKeySequence(Qt::Key_T), this,
                    [this] { viewport_->setViewTop(); });
    view->addAction("&Bottom", QKeySequence(Qt::Key_B), this,
                    [this] { viewport_->setViewBottom(); });
    view->addAction("&Isometric", QKeySequence(Qt::Key_I), this,
                    [this] { viewport_->setViewIso(); });
    view->addSeparator();
    view->addAction("&Fit to board", QKeySequence(Qt::Key_F), this,
                    [this] { viewport_->frameBoard(); });
    orthoAction_ = view->addAction("&Orthographic");
    orthoAction_->setCheckable(true);
    orthoAction_->setShortcut(QKeySequence(Qt::Key_O));
    connect(orthoAction_, &QAction::toggled, this, [this](bool on) {
        viewport_->camera().orthographic = on;
        viewport_->requestUpdate();
    });

    view->addSeparator();
    // Measurement tools. The M shortcut lives in the viewport's own
    // keyPressEvent (native QWindow -- QAction shortcuts never fire while it
    // has focus, see unhandledKey); the actions here are the discoverable
    // path and stay in sync via measureModeChanged.
    measureAction_ = view->addAction("&Measure distance");
    measureAction_->setCheckable(true);
    measureAction_->setToolTip(
        "Click two points to measure (M). Snaps to pads, holes and the board "
        "edge; Esc clears.");
    connect(measureAction_, &QAction::toggled, this,
            [this](bool on) { viewport_->setMeasureMode(on); });
    dimsAction_ = view->addAction("Board &dimensions");
    dimsAction_->setCheckable(true);
    dimsAction_->setToolTip("Width and height callouts around the board");
    connect(dimsAction_, &QAction::toggled, this,
            [this](bool on) { viewport_->setDimensionsOverlay(on); });
    dimsAction_->setChecked(
        appSettings().value("dimensionsOverlay", false).toBool());

    view->addSeparator();
    // Hide the stackup / properties docks for a clean, full-width view of the
    // board -- an inspection tool spends most of its time just looking. Backslash
    // toggles it without hunting through the menu.
    QAction* panels = view->addAction("Side &panels");
    panels->setCheckable(true);
    panels->setChecked(true);
    panels->setShortcut(QKeySequence(Qt::Key_Backslash));
    connect(panels, &QAction::toggled, this, [this](bool on) {
        if (stackupDock_) stackupDock_->setVisible(on);
        if (propertiesDock_) propertiesDock_->setVisible(on);
    });

    QMenu* layers = menuBar()->addMenu("&Layers");
    layers->addAction("Show &all", this, [this] {
        for (int i = 0; i < stackup_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = stackup_->topLevelItem(i);
            for (int j = 0; j < top->childCount(); ++j) {
                if (top->child(j)->flags() & Qt::ItemIsUserCheckable) {
                    top->child(j)->setCheckState(0, Qt::Checked);
                }
            }
        }
    });
    layers->addAction("&Hide inner copper", this, [this] {
        for (int i = 0; i < stackup_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = stackup_->topLevelItem(i);
            for (int j = 0; j < top->childCount(); ++j) {
                if (top->child(j)->text(0).startsWith("In")) {
                    top->child(j)->setCheckState(0, Qt::Unchecked);
                }
            }
        }
    });
    layers->addAction("Toggle &components", this, [this] {
        // The "Components" row is top-level (no children), so flip it directly.
        for (int i = 0; i < stackup_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = stackup_->topLevelItem(i);
            if (top->data(0, Qt::UserRole).toString() == "Components") {
                top->setCheckState(0, top->checkState(0) == Qt::Checked
                                          ? Qt::Unchecked
                                          : Qt::Checked);
            }
        }
    });

    QMenu* render = menuBar()->addMenu("&Render");
    render->addAction("&Board appearance…", this,
                      &MainWindow::showAppearanceDialog);
    render->addSeparator();

    QAction* rt = render->addAction("&Ray-traced shadows + AO");
    rt->setCheckable(true);
    connect(rt, &QAction::toggled, this,
            [this](bool on) { viewport_->setRayTracing(on); });

    QAction* pt = render->addAction("&Path tracing (full-scene lighting)");
    pt->setCheckable(true);
    connect(pt, &QAction::toggled, this,
            [this](bool on) { viewport_->setPathTracing(on); });

    QAction* oidn = render->addAction("Neural &denoise (Open Image Denoise)");
    oidn->setCheckable(true);
    connect(oidn, &QAction::toggled, this,
            [this](bool on) { viewport_->setDenoising(on); });

    render->addSeparator();
    QAction* fastMove =
        render->addAction("&Fast movement (raster while moving)");
    fastMove->setCheckable(true);
    fastMove->setToolTip(
        "While orbiting, panning, zooming or exploding, drop to plain raster and "
        "restore ray tracing / path tracing when motion stops. Keeps low-power "
        "GPUs and the CPU device responsive.");
    connect(fastMove, &QAction::toggled, this,
            [this](bool on) { viewport_->setFastMovement(on); });

    QMenu* gpuMenu = render->addMenu("&Graphics device");

    // The GPU list and RT capability are known only after the renderer exists
    // (first expose), so fill these in each time the menu opens.
    connect(render, &QMenu::aboutToShow, this,
            [this, rt, pt, oidn, fastMove, gpuMenu] {
        const bool haveRenderer = viewport_->renderer() != nullptr;
        const bool rtAvail = haveRenderer && viewport_->rtAvailable();
        const bool ptAvail = haveRenderer && viewport_->ptAvailable();
        const bool ptOn = viewport_->pathTracing();
        // RT shadows are a RASTER enhancement, GPU-only (Vulkan ray query). While
        // path tracing owns the frame the toggle has no effect at all, so grey it
        // out -- otherwise both read "on" at once and imply they compose. The
        // checked state is kept, so it comes back when path tracing is off.
        rt->setEnabled(rtAvail && !ptOn);
        pt->setEnabled(ptAvail);
        oidn->setEnabled(ptAvail && ptOn);
        rt->setToolTip(!rtAvail
                           ? "This device cannot ray trace"
                       : ptOn
                           ? "Not applicable while path tracing is on -- the "
                             "path tracer computes full-scene lighting itself"
                       : viewport_->cpuRender()
                           ? "Contact shadows + ambient occlusion via Intel "
                             "Embree on the CPU (assembled board only)"
                           : "Contact shadows + ambient occlusion, ray-traced "
                             "(shown on the assembled board, not while exploded)");
        pt->setToolTip(ptAvail
                           ? (viewport_->cpuRender()
                                  ? "Full path tracing on the CPU via Intel Embree; "
                                    "converges while the view is still"
                                  : "Full progressive path tracing — accurate global "
                                    "illumination; converges while the view is still")
                           : "This device cannot path trace");
        oidn->setToolTip("Intel Open Image Denoise — clean the path-traced image "
                         "in a fraction of the samples");
        {
            const QSignalBlocker b1(rt), b2(pt), b3(oidn), b4(fastMove);
            rt->setChecked(viewport_->rayTracing());
            pt->setChecked(viewport_->pathTracing());
            oidn->setChecked(viewport_->denoising());
            fastMove->setChecked(viewport_->fastMovement());
        }

        gpuMenu->clear();
        if (!viewport_->renderer()) {
            gpuMenu->addAction("(initialising…)")->setEnabled(false);
            return;
        }
        const bool autoPick = appSettings().value("gpuName").toString().isEmpty();
        const QString active = viewport_->activeGpuName();
        auto* group = new QActionGroup(gpuMenu);
        group->setExclusive(true);

        QAction* autoAct = gpuMenu->addAction("Automatic (discrete + RT preferred)");
        autoAct->setCheckable(true);
        autoAct->setChecked(autoPick);
        group->addAction(autoAct);
        connect(autoAct, &QAction::triggered, this,
                [this] { viewport_->setPreferredGpu(QString()); });
        gpuMenu->addSeparator();

        for (const QString& name : viewport_->availableGpuNames()) {
            // The software CPU driver enumerates as "llvmpipe (LLVM …)"; show it
            // as a plain "CPU Rendering (llvm)". Selection still keys off the real
            // device name.
            const QString label =
                name.contains("llvmpipe", Qt::CaseInsensitive)
                    ? QStringLiteral("CPU Rendering (llvm)")
                    : name;
            QAction* a = gpuMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(!autoPick && name == active);
            group->addAction(a);
            connect(a, &QAction::triggered, this,
                    [this, name] { viewport_->setPreferredGpu(name); });
        }
    });

    // --- Effects: stylised looks, deliberately NOT physically accurate ---
    // Component reflections turn IC/cap bodies chrome-like so they mirror the
    // board around them in the path tracer -- showing off what PT can do.
    fxComponentShine_ = appSettings().value("fxComponentShine", 0).toInt();
    fxPadShine_ = appSettings().value("fxPadShine", 94).toInt();
    fxShadowSoftness_ = appSettings().value("fxShadowSoftness", 15).toInt();
    fxNetGlow_ = appSettings().value("fxNetGlow", 20).toInt();
    if (qEnvironmentVariableIsSet("PCBVIEW_FX_GLOW"))
        fxNetGlow_ = qEnvironmentVariable("PCBVIEW_FX_GLOW").toInt();
    if (qEnvironmentVariableIsSet("PCBVIEW_FX_COMPONENT"))
        fxComponentShine_ = qEnvironmentVariable("PCBVIEW_FX_COMPONENT").toInt();
    if (qEnvironmentVariableIsSet("PCBVIEW_FX_PADS"))
        fxPadShine_ = qEnvironmentVariable("PCBVIEW_FX_PADS").toInt();
    if (qEnvironmentVariableIsSet("PCBVIEW_FX_SHADOW"))
        fxShadowSoftness_ = qEnvironmentVariable("PCBVIEW_FX_SHADOW").toInt();

    QMenu* effects = menuBar()->addMenu("&Effects");
    // Each slider shows its CURRENT VALUE in the label ("Pad shine — 94"), so a
    // setting can be read, reported, and reproduced exactly rather than
    // eyeballed from the handle position.
    const auto addFxSlider = [&](const QString& label, int initial,
                                 const std::function<void(int)>& apply) {
        auto* box = new QWidget(effects);
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(12, 6, 12, 6);
        lay->setSpacing(2);
        auto* text =
            new QLabel(QString("%1 — %2").arg(label).arg(initial), box);
        auto* slider = new QSlider(Qt::Horizontal, box);
        slider->setRange(0, 100);
        slider->setValue(initial);
        slider->setMinimumWidth(200);
        lay->addWidget(text);
        lay->addWidget(slider);
        auto* action = new QWidgetAction(effects);
        action->setDefaultWidget(box);
        effects->addAction(action);
        connect(slider, &QSlider::valueChanged, this,
                [text, label, apply](int v) {
                    text->setText(QString("%1 — %2").arg(label).arg(v));
                    apply(v);
                });
    };
    addFxSlider("Component reflections (mirror finish)", fxComponentShine_,
                [this](int v) {
                    fxComponentShine_ = v;
                    appSettings().setValue("fxComponentShine", v);
                    if (viewport_->renderer())
                        viewport_->renderer()->setComponentShine(v / 100.0f);
                    viewport_->requestUpdate();
                });
    addFxSlider("Pad shine", fxPadShine_, [this](int v) {
        fxPadShine_ = v;
        appSettings().setValue("fxPadShine", v);
        if (viewport_->renderer())
            viewport_->renderer()->setPadShine(v / 100.0f);
        viewport_->requestUpdate();
    });
    addFxSlider("Shadow softness (sun size, path tracing)", fxShadowSoftness_,
                [this](int v) {
                    fxShadowSoftness_ = v;
                    appSettings().setValue("fxShadowSoftness", v);
                    if (viewport_->renderer())
                        viewport_->renderer()->setShadowSoftness(v / 100.0f);
                    viewport_->requestUpdate();
                });
    // 0-100 maps to 0-16x emission. In the path tracer this is real
    // radiosity: the highlighted net throws that much more light onto its
    // surroundings, which is what makes a hot setting read as plasma rather
    // than paint.
    addFxSlider("Net glow (highlight brightness)", fxNetGlow_, [this](int v) {
        fxNetGlow_ = v;
        appSettings().setValue("fxNetGlow", v);
        if (viewport_->renderer())
            viewport_->renderer()->setNetGlow(v * 0.16f);
        viewport_->requestUpdate();
    });

    {
        auto* warn = view->addAction("Import report…");
        warn->setStatusTip(
            "Everything the importer could not use from this package");
        connect(warn, &QAction::triggered, this, &MainWindow::showImportWarnings);
    }
    view->addSeparator();

    // The chase, on by default -- it is the fastest way to see which way a
    // signal runs, but it is motion, so it needs an off switch.
    {
        auto* chase = view->addAction("Animate net highlight");
        chase->setCheckable(true);
        chase->setChecked(appSettings().value("fxNetChase", true).toBool());
        chase->setStatusTip(
            "Sweep a highlight along the net, then cycle a gradient "
            "(raster and ray-traced modes)");
        connect(chase, &QAction::toggled, this, [this](bool on) {
            appSettings().setValue("fxNetChase", on);
            if (viewport_->renderer()) viewport_->renderer()->setNetAnimate(on);
            viewport_->requestUpdate();
        });
        if (viewport_->renderer())
            viewport_->renderer()->setNetAnimate(chase->isChecked());
    }

    QMenu* help = menuBar()->addMenu("&Help");
    // The Ko-fi badge as an actual graphic in the menu -- a plain text entry
    // was too easy to overlook. Clicking opens the page and closes the menu.
    {
        auto* badge = new ClickLabel;
        badge->setPixmap(QPixmap(":/kofi_badge.png")
                             .scaledToWidth(170, Qt::SmoothTransformation));
        badge->setAlignment(Qt::AlignCenter);
        badge->setCursor(Qt::PointingHandCursor);
        badge->setContentsMargins(8, 6, 8, 6);
        badge->setToolTip("Support pcbview on Ko-fi");
        badge->onClick = [help] {
            QDesktopServices::openUrl(QUrl("https://ko-fi.com/P5P81EV1M0"));
            help->close();
        };
        auto* badgeAction = new QWidgetAction(help);
        badgeAction->setDefaultWidget(badge);
        help->addAction(badgeAction);
    }
    help->addSeparator();
    help->addAction("&About pcbview…", this, &MainWindow::showAbout);

    applyContext();
}

void MainWindow::buildToolbar() {
    QToolBar* tb = addToolBar("Main");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    // Text beside icon: the view/preset buttons carry no icon and still show
    // their label, while the measurement tools read as icon + word.
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto addBtn = [&](const QString& text, const QString& tip, auto fn) {
        QAction* a = tb->addAction(text);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };

    addBtn("Open", "Open a board (Ctrl+O)", [this] { onOpen(); });
    tb->addSeparator();
    addBtn("Top", "Top view (T)", [this] { viewport_->setViewTop(); });
    addBtn("Bottom", "Bottom view (B)", [this] { viewport_->setViewBottom(); });
    addBtn("Iso", "Isometric view (I)", [this] { viewport_->setViewIso(); });
    tb->addSeparator();
    addBtn("Fit", "Fit board to window (F)", [this] { viewport_->frameBoard(); });

    // Same action the View menu owns -- see orthoAction_.
    if (orthoAction_) {
        orthoAction_->setIconText("Ortho");
        orthoAction_->setToolTip("Orthographic projection (O)");
        tb->addAction(orthoAction_);
    }

    // Measurement tools. These are the SAME QActions the View menu owns, so
    // the toolbar buttons, the menu checkboxes and the M key all stay in sync
    // for free -- no duplicate state to keep aligned.
    tb->addSeparator();
    if (measureAction_) {
        measureAction_->setIcon(rulerIcon());
        measureAction_->setIconText("Measure");
        tb->addAction(measureAction_);
    }
    if (dimsAction_) {
        dimsAction_->setIcon(speedSquareIcon());
        dimsAction_->setIconText("Dimensions");
        tb->addAction(dimsAction_);
    }

    tb->addSeparator();
    tb->addWidget(new QLabel("  Internal res  "));
    QSlider* scale = new QSlider(Qt::Horizontal);
    scaleSlider_ = scale;
    scale->setRange(25, 400);  // 0.25x .. 4.00x, default 1.00x (native)
    scale->setValue(100);
    scale->setFixedWidth(130);
    scale->setTickPosition(QSlider::TicksBelow);
    scale->setTickInterval(25);
    scale->setToolTip("Internal render resolution. Supersamples the board only;\n"
                      "the interface always draws at native resolution.");
    connect(scale, &QSlider::valueChanged, this, &MainWindow::onRenderScaleChanged);
    tb->addWidget(scale);

    // The handle sits 20% along a 0.25-4.00 range at 1.00x, which reads as
    // "already turned down". Show the number so it is unambiguous.
    scaleLabel_ = new QLabel("1.00×");
    scaleLabel_->setFixedWidth(46);
    scaleLabel_->setStyleSheet(QString("color:%1").arg(theme::kText));
    tb->addWidget(scaleLabel_);

    QAction* resetScale = tb->addAction("↺");
    resetScale->setToolTip("Reset internal resolution to 1.00× (native)");
    connect(resetScale, &QAction::triggered, this,
            [scale] { scale->setValue(100); });

    tb->addSeparator();
    tb->addWidget(new QLabel("  Peel  "));
    explodeLabel_ = new QLabel("off");
    explodeLabel_->setFixedWidth(50);
    explodeLabel_->setToolTip(
        "Ctrl + scroll wheel over the board to peel the stackup.\n"
        "Layers separate one ring at a time, outermost first, pausing\n"
        "between each so you can stop and look. Collapse reassembles it.");
    explodeLabel_->setStyleSheet(QString("color:%1").arg(theme::kTextDim));
    tb->addWidget(explodeLabel_);

    QAction* collapse = tb->addAction("Collapse");
    collapse->setToolTip("Reassemble the board (Ctrl+scroll down also works)");
    connect(collapse, &QAction::triggered, this,
            [this] { viewport_->setExplodeProgress(0.0f); });

    toolbarInfo_ = new QLabel("  no board loaded  ");
    toolbarInfo_->setStyleSheet(QString("color:%1").arg(theme::kTextDim));
    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addWidget(toolbarInfo_);
}

void MainWindow::buildStackupDock() {
    auto* dock = new CollapsibleDock("STACKUP", Qt::LeftDockWidgetArea, this);

    stackup_ = new QTreeWidget;
    stackup_->setHeaderHidden(true);
    stackup_->setRootIsDecorated(true);
    stackup_->setIndentation(12);

    connect(stackup_, &QTreeWidget::itemChanged, this, &MainWindow::onLayerToggled);
    connect(stackup_, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::onLayerSelected);

    dock->setContent(stackup_);
    dock->setMinimumWidth(200);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    stackupDock_ = dock;
}

void MainWindow::populateStackup() {
    if (!loaded_) return;

    // Rebuilding fires itemChanged for every insert; suppress so a repopulate
    // does not stomp renderer visibility flags.
    const QSignalBlocker block(stackup_);
    stackup_->clear();

    QTreeWidgetItem* stack = new QTreeWidgetItem(stackup_, {"Layers"});
    stack->setExpanded(true);
    QTreeWidgetItem* other = new QTreeWidgetItem(stackup_, {"Geometry"});
    other->setExpanded(true);

    // Ordered top-to-bottom the way the board is physically built, not the way
    // KiCad numbers layers -- see ARCHITECTURE.md on layer ordinals lying.
    size_t componentTris = 0;
    bool addedSubstrate = false;
    for (const geom::Part& part : mesh_.parts) {
        // All component parts share the name "Components" and collapse into one
        // toggle row below -- keep them out of the per-layer list.
        if (part.material == geom::Material::Component) {
            componentTris += part.mesh.triangleCount();
            continue;
        }
        // The substrate is now several dielectric slabs (one between each pair of
        // copper foils), all named "substrate". One toggle row drives them all.
        if (part.material == geom::Material::Substrate) {
            if (addedSubstrate) continue;
            addedSubstrate = true;
        }
        const QString name = QString::fromStdString(part.name);
        QTreeWidgetItem* parent =
            (part.material == geom::Material::Substrate) ? other : stack;
        QTreeWidgetItem* item = new QTreeWidgetItem(parent, {name});
        item->setCheckState(0, Qt::Checked);
        item->setIcon(0, swatchIcon(swatchFor(name)));
        item->setData(0, Qt::UserRole, name);
        item->setToolTip(0, QString("%1 triangles").arg(part.mesh.triangleCount()));
    }

    const size_t drillCount =
        fromGerber_ ? baseArt_.drills.size() : board_.drills.size();
    QTreeWidgetItem* drills =
        new QTreeWidgetItem(other, {QString("Drills — %1").arg(drillCount)});
    drills->setFlags(drills->flags() & ~Qt::ItemIsUserCheckable);

    // Components: one checkable top-level row driving every colour/side part.
    if (componentTris > 0) {
        QTreeWidgetItem* comp = new QTreeWidgetItem(stackup_, {"Components"});
        comp->setCheckState(0, Qt::Checked);
        comp->setData(0, Qt::UserRole, QStringLiteral("Components"));
        comp->setIcon(0, swatchIcon(QColor(90, 90, 95)));
        comp->setToolTip(0, QString("%1 triangles").arg(componentTris));
    }
}

void MainWindow::buildPropertiesDock() {
    auto* dock = new CollapsibleDock("PROPERTIES", Qt::RightDockWidgetArea, this);

    properties_ = new QTreeWidget;
    properties_->setColumnCount(2);
    properties_->setHeaderLabels({"Property", "Value"});
    properties_->header()->setStretchLastSection(true);
    properties_->setRootIsDecorated(false);

    dock->setContent(properties_);
    dock->setMinimumWidth(230);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    propertiesDock_ = dock;
}

void MainWindow::buildNetDock() {
    auto* dock = new CollapsibleDock("NETS", Qt::RightDockWidgetArea, this);

    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    netFilter_ = new QLineEdit;
    netFilter_->setPlaceholderText("Filter nets…");
    netFilter_->setClearButtonEnabled(true);
    layout->addWidget(netFilter_);

    // Derive connectivity from copper when the package has no netlist. On
    // DEMAND, deliberately: inferred data must never appear on load looking
    // like ground truth. Hidden unless it would actually do something.
    inferNetsBtn_ = new QPushButton("Infer nets from copper");
    inferNetsBtn_->setToolTip(
        "Work out what is electrically connected by following the copper and "
        "its plated holes.\n"
        "These are NOT a netlist: they have no real names, an unrouted net "
        "appears as several,\nand two shorted nets appear as one.");
    inferNetsBtn_->setVisible(false);
    connect(inferNetsBtn_, &QPushButton::clicked, this,
            &MainWindow::inferNetsFromCopper);
    layout->addWidget(inferNetsBtn_);

    // Provenance banner, shown only while pseudo-nets are on display.
    pseudoNetNote_ = new QLabel(
        "Derived from copper geometry — not a netlist. Names are arbitrary.");
    pseudoNetNote_->setWordWrap(true);
    pseudoNetNote_->setStyleSheet(
        QString("color:%1; padding:2px 4px;").arg(theme::kTextDim));
    pseudoNetNote_->setVisible(false);
    layout->addWidget(pseudoNetNote_);

    netList_ = new QTreeWidget;
    netList_->setColumnCount(2);
    netList_->setHeaderLabels({"Net", "Routed"});
    netList_->header()->setStretchLastSection(false);
    netList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    netList_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    netList_->setRootIsDecorated(false);
    netList_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(netList_);

    // Selecting a row highlights that net; the same call drives the renderer
    // and the status line, so board clicks and the headless hook land here too.
    netList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(netList_, &QTreeWidget::itemSelectionChanged, this, [this] {
        // Selection order is not preserved by Qt, so keep nets already
        // highlighted in their existing slots -- otherwise adding a net would
        // reshuffle every colour.
        std::vector<int> picked;
        for (QTreeWidgetItem* it : netList_->selectedItems())
            picked.push_back(it->data(0, Qt::UserRole).toInt());
        std::vector<int> ordered;
        for (int n : highlightedNets_)
            if (std::find(picked.begin(), picked.end(), n) != picked.end())
                ordered.push_back(n);
        for (int n : picked)
            if (std::find(ordered.begin(), ordered.end(), n) == ordered.end())
                ordered.push_back(n);
        highlightedNets_ = ordered;
        applyNetHighlights();
    });
    connect(netFilter_, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (int i = 0; i < netList_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = netList_->topLevelItem(i);
            it->setHidden(!t.isEmpty() &&
                          !it->text(0).contains(t, Qt::CaseInsensitive));
        }
    });

    dock->setContent(panel);
    dock->setMinimumWidth(230);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    netDock_ = dock;
}

void MainWindow::showImportWarnings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Import report");
    auto* lay = new QVBoxLayout(&dlg);
    if (importWarnings_.isEmpty() && importNotes_.isEmpty()) {
        lay->addWidget(new QLabel("Nothing to report — every file in the "
                                  "package was recognised and used.",
                                  &dlg));
    } else {
        // Two sections, because they mean opposite things. A warning may make
        // the render disagree with the board; a note is the importer saying it
        // understood a file and chose not to draw it.
        QString body;
        if (!importWarnings_.isEmpty()) {
            body += QString("WARNINGS (%1) — these may make the render "
                            "disagree with the board:\n")
                        .arg(importWarnings_.size());
            for (const QString& w : importWarnings_) body += "  - " + w + "\n";
        }
        if (!importNotes_.isEmpty()) {
            if (!body.isEmpty()) body += "\n";
            body += QString("NOTES (%1) — recognised and deliberately not "
                            "rendered. Nothing is wrong:\n")
                        .arg(importNotes_.size());
            for (const QString& n : importNotes_) body += "  - " + n + "\n";
        }
        auto* list = new QPlainTextEdit(&dlg);
        list->setReadOnly(true);
        list->setPlainText(body);
        list->setMinimumSize(780, 300);
        lay->addWidget(list);
    }
    auto* close = new QPushButton("Close", &dlg);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    lay->addWidget(close);
    dlg.exec();
}

void MainWindow::inferNetsFromCopper() {
    if (!loaded_) return;

    // A progress dialog rather than a wait cursor: on a dense board this takes
    // long enough to look like a hang, and a frozen window with no explanation
    // is indistinguishable from a crash. Cancellable, because the user who
    // starts it on a huge board is exactly the one who needs a way out --
    // extractPseudoNets does not touch the board until its final step, so
    // cancelling leaves everything as it was.
    QProgressDialog dlg("Analysing copper connectivity…", "Cancel", 0, 100, this);
    dlg.setWindowTitle("Infer nets");
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setValue(0);

    // Runs on baseArt_ so it survives a thickness change or any other
    // reassemble -- recomputing per rebuild would be wasted work, and the
    // connectivity does not depend on Z.
    const geom::PseudoNetStats found = geom::extractPseudoNets(
        baseArt_, [&dlg](const std::string& stage, int pct) {
            dlg.setLabelText(QString::fromStdString(stage) + "…");
            dlg.setValue(pct);
            QApplication::processEvents();
            return !dlg.wasCanceled();
        });

    if (dlg.wasCanceled()) {
        dlg.close();
        statusBar()->showMessage("Net inference cancelled — board unchanged.",
                                 5000);
        return;
    }

    // The rebuild is a second, comparable wait: the mesh carries per-triangle
    // net ids, so every triangle is regenerated. Say so rather than freezing
    // again right after the progress dialog claimed to be finished.
    dlg.setLabelText("Rebuilding board with the new nets…");
    dlg.setValue(99);
    QApplication::processEvents();

    if (found.groups <= 0) {
        dlg.close();
        statusBar()->showMessage(
            "No connectivity could be derived — the board has no copper, or "
            "already has a netlist.",
            6000);
        return;
    }
    reassemble();      // netArt changed, so the mesh must carry the new net ids
    populateNets();
    dlg.close();
    // Quote both numbers. The total alone reads as an explosion on a dense
    // board, when most of what it found is copper that really is isolated.
    statusBar()->showMessage(
        QString("Derived %1 groups from copper connectivity — %2 join two or "
                "more pieces of copper, %3 are isolated (a lone pad or a "
                "fragment). Not a netlist; names are arbitrary.")
            .arg(found.groups)
            .arg(found.connecting)
            .arg(found.groups - found.connecting),
        12000);
}

void MainWindow::populateNets() {
    if (!netList_) return;
    netList_->clear();
    highlightedNet_ = -1;
    if (viewport_->renderer()) viewport_->renderer()->setHighlightNet(-1);

    // Say WHY the list is empty rather than showing a blank panel that looks
    // like a failure -- but say it accurately. Gerbers absolutely can carry
    // nets (X2 %TO.N% object attributes, which pcbview reads); this particular
    // package simply has none, which for a KiCad export means it was written
    // without "Include advanced X2 features".
    // Offer inference only when there is no netlist to overwrite, and say so
    // whenever the list is showing derived data.
    if (inferNetsBtn_)
        inferNetsBtn_->setVisible(loaded_ && mesh_.nets.empty());
    if (pseudoNetNote_)
        pseudoNetNote_->setVisible(baseArt_.netsArePseudo && !mesh_.nets.empty());

    if (mesh_.nets.empty()) {
        auto* none = new QTreeWidgetItem(
            netList_,
            {fromGerber_ ? "(no net attributes in this package)" : "(no nets)",
             ""});
        if (fromGerber_)
            none->setToolTip(
                0,
                "This Gerber package has no X2 net attributes (%TO.N%).\n"
                "Re-export with X2 attributes enabled -- in KiCad, Plot -> "
                "\"Include advanced X2 features\" -- and nets will appear "
                "here, with highlighting and routed length.");
        none->setForeground(0, QColor(theme::kTextDim));
        none->setFlags(Qt::NoItemFlags);
        return;
    }
    // Derived nets have no track centrelines to sum a routed length from --
    // Gerber copper is filled regions, not routes -- so the column reports
    // copper AREA instead of printing a 0.0 mm that was never measured.
    const bool pseudo = baseArt_.netsArePseudo;
    netList_->setHeaderLabels({"Net", pseudo ? "Copper" : "Routed"});
    for (size_t i = 0; i < mesh_.nets.size(); ++i) {
        const auto& n = mesh_.nets[i];
        const QString metric =
            pseudo ? QString::number(n.copperMm2, 'f', 2) + " mm²"
                   : QString::number(n.routedMm, 'f', 1) + " mm";
        auto* it = new QTreeWidgetItem(
            netList_, {QString::fromStdString(n.name), metric});
        it->setData(0, Qt::UserRole, static_cast<int>(i));
        it->setForeground(1, QColor(theme::kTextDim));
    }
    // Real nets sort by name, which is what you want when hunting for one you
    // can already name. Derived nets have no meaningful names, so insertion
    // order is kept instead -- extractPseudoNets emits them largest-copper
    // first, putting the pours and power planes at the top. Sorting them by
    // name would order them "~1, ~10, ~100", which is worse than useless.
    if (!pseudo) netList_->sortItems(0, Qt::AscendingOrder);
}

// A distinct, high-contrast palette. Red first because a single highlight
// should look the way it always has; the rest are chosen to stay separable
// against gold copper, green laminate and each other.
static const std::array<std::array<float, 3>, 8> kNetPalette = {{
    {{1.00f, 0.09f, 0.06f}},  // red
    {{0.05f, 0.85f, 1.00f}},  // cyan
    {{0.25f, 1.00f, 0.20f}},  // green
    {{1.00f, 0.10f, 0.85f}},  // magenta
    {{1.00f, 0.62f, 0.05f}},  // amber
    {{0.55f, 0.35f, 1.00f}},  // violet
    {{1.00f, 1.00f, 0.30f}},  // yellow
    {{0.15f, 0.45f, 1.00f}},  // blue
}};

void MainWindow::toggleHighlightNet(int net) {
    if (net < 0) return;
    const auto it = std::find(highlightedNets_.begin(), highlightedNets_.end(),
                              net);
    if (it != highlightedNets_.end()) highlightedNets_.erase(it);
    else highlightedNets_.push_back(net);
    applyNetHighlights();
}

// Push the current selection to the renderer, colour the list rows to match,
// and summarise in the status bar. Every entry point routes through here so
// the list, the board and the readout cannot disagree.
void MainWindow::applyNetHighlights() {
    std::vector<std::array<float, 3>> colours;
    for (size_t i = 0; i < highlightedNets_.size(); ++i)
        colours.push_back(kNetPalette[i % kNetPalette.size()]);

    highlightedNet_ = highlightedNets_.empty() ? -1 : highlightedNets_.front();
    if (viewport_->renderer()) {
        viewport_->renderer()->setHighlightNets(highlightedNets_, colours);
        viewport_->requestUpdate();
    }

    // Row colours mirror the glow, so the list doubles as the legend.
    if (netList_) {
        const QSignalBlocker block(netList_);
        for (int i = 0; i < netList_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = netList_->topLevelItem(i);
            const int n = it->data(0, Qt::UserRole).toInt();
            const auto f = std::find(highlightedNets_.begin(),
                                     highlightedNets_.end(), n);
            if (f == highlightedNets_.end()) {
                it->setForeground(0, QColor(theme::kText));
                it->setSelected(false);
            } else {
                const auto& c =
                    kNetPalette[(f - highlightedNets_.begin()) %
                                kNetPalette.size()];
                it->setForeground(0, QColor::fromRgbF(c[0], c[1], c[2]));
                it->setSelected(true);
            }
        }
    }

    if (highlightedNets_.empty()) {
        statusBar()->clearMessage();
        return;
    }
    if (highlightedNets_.size() > 1) {
        QStringList names;
        double total = 0.0;
        for (int n : highlightedNets_) {
            if (n < 0 || n >= static_cast<int>(mesh_.nets.size())) continue;
            names << QString::fromStdString(mesh_.nets[n].name);
            total += mesh_.nets[n].routedMm;
        }
        statusBar()->showMessage(QString("%1 nets: %2  ·  %3 mm routed total")
                                     .arg(names.size())
                                     .arg(names.join(", "))
                                     .arg(total, 0, 'f', 1));
        return;
    }
    highlightNet(highlightedNets_.front());
}

void MainWindow::highlightNet(int net) {
    highlightedNet_ = net;
    if (highlightedNets_.size() != 1 || highlightedNets_.front() != net) {
        highlightedNets_.clear();
        if (net >= 0) highlightedNets_.push_back(net);
        applyNetHighlights();
        if (highlightedNets_.size() > 1) return;
    }
    if (net < 0 || net >= static_cast<int>(mesh_.nets.size())) {
        statusBar()->clearMessage();
        return;
    }
    const auto& n = mesh_.nets[net];
    statusBar()->showMessage(QString("Net %1  ·  %2 mm routed  ·  %3 via%4")
                                 .arg(QString::fromStdString(n.name))
                                 .arg(n.routedMm, 0, 'f', 3)
                                 .arg(n.viaCount)
                                 .arg(n.viaCount == 1 ? "" : "s"));

    // Keep the list in step when the highlight came from somewhere else.
    if (netList_) {
        const QSignalBlocker block(netList_);
        for (int i = 0; i < netList_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = netList_->topLevelItem(i);
            if (it->data(0, Qt::UserRole).toInt() == net) {
                netList_->setCurrentItem(it);
                netList_->scrollToItem(it);
                break;
            }
        }
    }
}

void MainWindow::populateProperties() {
    properties_->clear();
    if (!loaded_) return;

    auto add = [&](const QString& k, const QString& v) {
        QTreeWidgetItem* i = new QTreeWidgetItem(properties_, {k, v});
        i->setForeground(0, QColor(theme::kTextDim));
    };

    if (fromGerber_) {
        // Gerber has geometry, not semantics: no tracks, pads, vias, or
        // components exist to report. Show what LayerArt actually carries.
        int copperLayers = 0;
        for (const geom::ArtLayer& al : baseArt_.layers)
            if (al.kind == LayerKind::Copper) ++copperLayers;
        const double th =
            thicknessOverride_ > 0.0 ? thicknessOverride_ : baseArt_.thickness;
        add("Source", "Gerber package");
        add("Package", QFileInfo(path_).fileName());
        add("Thickness",
            mm(th, 3) + " mm" + (thicknessOverride_ > 0.0 ? " (override)" : ""));
        add("Copper foil", mm(baseArt_.copperThickness, 4) + " mm");
        add("Mask film", mm(baseArt_.maskThickness, 4) + " mm");
        add("Copper layers", QString::number(copperLayers));
        add("Drills", QString::number(baseArt_.drills.size()));
        add("Triangles", QString::number(mesh_.totalTriangles()));
        return;
    }

    const double th =
        thicknessOverride_ > 0.0 ? thicknessOverride_ : board_.thickness;
    add("Board", QFileInfo(path_).fileName());
    add("Thickness",
        mm(th, 3) + " mm" + (thicknessOverride_ > 0.0 ? " (override)" : ""));
    add("Copper foil", mm(board_.copperThickness, 4) + " mm");
    add("Mask film", mm(board_.maskThickness, 4) + " mm");
    add("Dielectric", mm(board_.dielectricThickness, 4) + " mm");
    add("Copper layers", QString::number(board_.copperLayers().size()));
    add("Tracks", QString::number(board_.tracks.size()));
    add("Vias", QString::number(board_.vias.size()));
    add("Pads", QString::number(board_.pads.size()));
    add("Drills", QString::number(board_.drills.size()));
    add("Components", QString::number(board_.components.size()));
    add("Triangles", QString::number(mesh_.totalTriangles()));
}

void MainWindow::buildStatusBar() {
    statusFile_ = new QLabel;
    statusBoard_ = new QLabel;
    statusChecks_ = new QLabel;
    statusPerf_ = new QLabel;

    statusBar()->addWidget(statusFile_);
    statusBar()->addWidget(new QLabel(" · "));
    statusBar()->addWidget(statusBoard_);
    statusBar()->addWidget(new QLabel(" · "));
    statusBar()->addWidget(statusChecks_);
    statusBar()->addPermanentWidget(statusPerf_);
}

void MainWindow::updateStatus() {
    if (!loaded_) return;
    statusFile_->setText(QFileInfo(path_).fileName());

    int copperLayers = 0;
    double thickness = 0.0;
    if (fromGerber_) {
        for (const geom::ArtLayer& al : baseArt_.layers)
            if (al.kind == LayerKind::Copper) ++copperLayers;
        thickness = baseArt_.thickness;
    } else {
        copperLayers = static_cast<int>(board_.copperLayers().size());
        thickness = board_.thickness;
    }
    statusBoard_->setText(
        QString("%1 copper layers  ·  %2 × %3 × %4 mm")
            .arg(copperLayers)
            .arg(mm(mesh_.bounds.max[0] - mesh_.bounds.min[0], 1))
            .arg(mm(mesh_.bounds.max[1] - mesh_.bounds.min[1], 1))
            .arg(mm(thickness, 2)));

    if (fromGerber_) {
        // The connectivity and short checks need per-PAD nets. Gerber X2 gives
        // us net names on copper (so highlighting and routed length work), but
        // a flash is not distinguishable from any other exposure, so there is
        // no pad table to check against. Say which of the two situations this
        // is rather than a blanket "no nets", and never show a green zero that
        // means "no data" rather than "verified clean".
        statusChecks_->setText(mesh_.nets.empty()
                                   ? "gerber — no net attributes for checks"
                                   : "gerber — nets loaded; pad checks need a "
                                     "board file");
        statusChecks_->setStyleSheet(QString("color:%1").arg(theme::kTextDim));
        return;
    }

    const GeometryReport geom = validateNetGeometry(board_);
    const OverlapReport overlaps = validatePadOverlaps(board_);
    const bool clean = geom.clean() && overlaps.clean();
    statusChecks_->setText(QString("%1 orphans  ·  %2 shorts")
                               .arg(geom.orphanEndpoints)
                               .arg(overlaps.shorts));
    statusChecks_->setStyleSheet(clean ? "color:#5eb85e" : "color:#d9534f");
}

void MainWindow::onLayerToggled(QTreeWidgetItem* item, int column) {
    if (column != 0 || !viewport_->renderer()) return;
    const QString name = item->data(0, Qt::UserRole).toString();
    if (name.isEmpty()) return;

    const bool on = (item->checkState(0) == Qt::Checked);
    // Through the setter, not a raw PartInfo::visible write: the traced
    // geometry (path tracing + RT shadows) bakes visibility into the BLAS and
    // must be told to rebuild.
    viewport_->renderer()->setPartVisible(name.toStdString(), on);
    viewport_->requestUpdate();
}

void MainWindow::onLayerSelected() {
    const auto selected = stackup_->selectedItems();
    if (selected.isEmpty() || !loaded_) return;
    const QString name = selected.first()->data(0, Qt::UserRole).toString();
    if (name.isEmpty()) return;

    const Layer* layer = board_.findLayer(name.toStdString());
    if (!layer) return;

    while (properties_->topLevelItemCount() > kFixedPropertyRows) {
        delete properties_->takeTopLevelItem(properties_->topLevelItemCount() - 1);
    }
    auto add = [&](const QString& k, const QString& v) {
        QTreeWidgetItem* i = new QTreeWidgetItem(properties_, {k, v});
        i->setForeground(0, QColor(theme::kTextDim));
    };
    add("", "");
    add(name + " — z bottom", mm(layer->z, 4) + " mm");
    add(name + " — z top", mm(layer->z + layer->thickness, 4) + " mm");
    add(name + " — thickness", mm(layer->thickness, 4) + " mm");
    if (layer->stackIndex >= 0) {
        add(name + " — stack index", QString::number(layer->stackIndex));
    }
    add(name + " — KiCad id", QString::number(layer->kicadId));
}

void MainWindow::onRenderScaleChanged(int sliderValue) {
    const float scale = sliderValue / 100.0f;
    if (scaleLabel_) scaleLabel_->setText(QString::number(scale, 'f', 2) + "×");
    if (!viewport_->renderer()) return;
    viewport_->renderer()->setRenderScale(scale);
    viewport_->requestUpdate();
}

void MainWindow::onFrameRendered() {
    // Refreshing every frame would spend more time in Qt than in the renderer.
    if (++frameCounter_ % 15 != 0) return;
    auto* r = viewport_->renderer();
    if (!r) return;

    const double ms = viewport_->frameMs();
    const VkExtent2D scene = r->sceneExtent();
    statusPerf_->setText(
        QString("%1 × %2 @ %3×  ·  %4 tris  ·  %5 draws  ·  %6 ms  ·  %7 fps")
            .arg(scene.width)
            .arg(scene.height)
            .arg(QString::number(r->renderScale(), 'f', 2))
            .arg(r->stats().triangles)
            .arg(r->stats().drawCalls)
            .arg(QString::number(ms, 'f', 2))
            .arg(ms > 0.0 ? QString::number(1000.0 / ms, 'f', 0) : "—"));
}

void MainWindow::showAppearanceDialog() {
    if (!loaded_) {
        QMessageBox::information(this, "Board appearance",
                                 "Open a board first.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Board appearance");
    auto* form = new QFormLayout(&dlg);

    // --- Thickness override ---
    const double designThickness =
        fromGerber_ ? baseArt_.thickness : board_.thickness;
    auto* thick = new QDoubleSpinBox;
    thick->setRange(0.05, 6.4);
    thick->setSingleStep(0.1);
    thick->setDecimals(3);
    thick->setSuffix(" mm");
    thick->setValue(thicknessOverride_ > 0.0 ? thicknessOverride_
                                             : designThickness);
    auto* thickRow = new QHBoxLayout;
    thickRow->addWidget(thick);
    auto* thickReset = new QPushButton("Design (" + mm(designThickness, 2) + ")");
    thickRow->addWidget(thickReset);
    form->addRow("Finished thickness", thickRow);

    // Common fab thicknesses + a couple of flex presets.
    auto* preset = new QComboBox;
    preset->addItems({"—", "1.60 mm FR4", "0.80 mm FR4", "0.40 mm",
                      "0.20 mm flex", "0.10 mm flex"});
    form->addRow("Preset", preset);

    // --- Substrate colour + translucency ---
    // Start from the current known values (renderer holds them but we mirror the
    // defaults here; the swatch just needs a sensible initial colour).
    auto* colorBtn = new QPushButton("Choose…");
    QColor current = QColor::fromRgbF(subColor_[0], subColor_[1], subColor_[2]);
    const auto paintSwatch = [&](const QColor& c) {
        colorBtn->setStyleSheet(
            QString("background:%1; color:%2")
                .arg(c.name())
                .arg(c.lightnessF() > 0.5 ? "#000" : "#fff"));
    };
    paintSwatch(current);
    form->addRow("Substrate colour", colorBtn);

    auto* opacity = new QSlider(Qt::Horizontal);
    opacity->setRange(15, 100);
    opacity->setValue(static_cast<int>(subOpacity_ * 100.0f + 0.5f));
    auto* opacityVal = new QLabel(QString::number(opacity->value()) + "%");
    auto* opRow = new QHBoxLayout;
    opRow->addWidget(opacity);
    opRow->addWidget(opacityVal);
    form->addRow("Substrate opacity", opRow);

    // --- Soldermask colour ---
    auto* maskBtn = new QPushButton("Choose…");
    QColor maskCurrent =
        QColor::fromRgbF(maskColor_[0], maskColor_[1], maskColor_[2]);
    const auto paintMask = [&](const QColor& c) {
        maskBtn->setStyleSheet(QString("background:%1; color:%2")
                                   .arg(c.name())
                                   .arg(c.lightnessF() > 0.5 ? "#000" : "#fff"));
    };
    paintMask(maskCurrent);
    form->addRow("Soldermask colour", maskBtn);

    auto* maskOp = new QSlider(Qt::Horizontal);
    maskOp->setRange(15, 100);
    maskOp->setValue(static_cast<int>(maskOpacity_ * 100.0f + 0.5f));
    auto* maskOpVal = new QLabel(QString::number(maskOp->value()) + "%");
    auto* maskOpRow = new QHBoxLayout;
    maskOpRow->addWidget(maskOp);
    maskOpRow->addWidget(maskOpVal);
    form->addRow("Soldermask opacity", maskOpRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    form->addRow(buttons);

    // --- live wiring ---
    auto applyColor = [this, &current, opacity] {
        subColor_ = {static_cast<float>(current.redF()),
                     static_cast<float>(current.greenF()),
                     static_cast<float>(current.blueF())};
        subOpacity_ = opacity->value() / 100.0f;
        applyAppearance();
    };
    connect(thick, &QDoubleSpinBox::valueChanged, this, [this, thick] {
        thicknessOverride_ = thick->value();
        reassemble();
    });
    connect(thickReset, &QPushButton::clicked, this,
            [this, thick, designThickness] {
                thicknessOverride_ = 0.0;
                thick->setValue(designThickness);
                reassemble();
            });
    connect(preset, &QComboBox::currentTextChanged, this,
            [thick](const QString& t) {
                // Parse the leading number, if any.
                bool ok = false;
                const double v = t.left(4).trimmed().toDouble(&ok);
                if (ok) thick->setValue(v);
            });
    connect(colorBtn, &QPushButton::clicked, &dlg,
            [this, &current, paintSwatch, applyColor, &dlg] {
                const QColor c = QColorDialog::getColor(current, &dlg,
                                                        "Substrate colour");
                if (c.isValid()) {
                    current = c;
                    paintSwatch(c);
                    applyColor();
                }
            });
    connect(opacity, &QSlider::valueChanged, this,
            [opacityVal, applyColor](int v) {
                opacityVal->setText(QString::number(v) + "%");
                applyColor();
            });
    connect(maskBtn, &QPushButton::clicked, &dlg,
            [this, &maskCurrent, paintMask, &dlg] {
                const QColor c = QColorDialog::getColor(maskCurrent, &dlg,
                                                        "Soldermask colour");
                if (c.isValid()) {
                    maskCurrent = c;
                    paintMask(c);
                    maskColor_ = {static_cast<float>(c.redF()),
                                  static_cast<float>(c.greenF()),
                                  static_cast<float>(c.blueF())};
                    applyAppearance();
                }
            });
    connect(maskOp, &QSlider::valueChanged, this,
            [this, maskOpVal](int v) {
                maskOpVal->setText(QString::number(v) + "%");
                maskOpacity_ = v / 100.0f;
                applyAppearance();
            });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);

    dlg.exec();
}

void MainWindow::showAbout() {
    // Custom dialog rather than QMessageBox::about: it hosts the ANIMATED
    // Ko-fi button (QMovie needs a real QLabel). The licence text is
    // unchanged -- LGPL-3.0 4(c): this displays copyright notices during
    // execution, so Qt's copyright must appear here and the user must be
    // directed to the licences.
    QDialog dlg(this);
    dlg.setWindowTitle("About pcbview");

    auto* text = new QLabel(
        "<h3>pcbview 1.16.0</h3>"
        "<p>Standalone 3D PCB viewer. Renders what the fab will build.</p>"
        "<p>Copyright © 2026 pcbview contributors.<br>"
        "pcbview is free software under the <b>GNU General Public License "
        "version 3</b> or later. See <code>LICENSE</code> beside the "
        "executable.</p>"
        "<p><b>Third-party software</b></p>"
        "<p>Silkscreen text uses the <b>Newstroke</b> stroke font.<br>"
        "Copyright © 2010 Vladimir Uryvaev; Copyright The KiCad Developers.<br>"
        "Licensed GPL-2.0-or-later, used here under GPL-3.0.</p>"
        "<p>This program uses the <b>Qt</b> framework.<br>"
        "Copyright © The Qt Company Ltd. and other contributors.<br>"
        "Qt, and its use in pcbview, are covered by the "
        "<b>GNU Lesser General Public License version 3</b>.</p>"
        "<p>Qt is dynamically linked and its libraries may be replaced with any "
        "interface-compatible build. Qt source is available from "
        "<a href=\"https://download.qt.io/\">download.qt.io</a>.</p>"
        "<p>Copies of the GNU GPL v3 and GNU LGPL v3 are distributed with this "
        "program in the <code>LICENSES</code> folder beside the executable, "
        "alongside <code>NOTICE.md</code>.</p>"
        "<p>Also uses Clipper2 (BSL-1.0), earcut.hpp (ISC) and glm (MIT).</p>");
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);

    // The animated Ko-fi button. QMovie loops the GIF for as long as the
    // dialog is open; clicking opens the page.
    auto* kofi = new ClickLabel;
    auto* movie = new QMovie(":/kofi_support.gif", QByteArray(), kofi);
    movie->setScaledSize(QSize(276, 63));  // 690x158 at 40%
    kofi->setMovie(movie);
    kofi->setCursor(Qt::PointingHandCursor);
    kofi->setToolTip("Support pcbview on Ko-fi");
    kofi->setAlignment(Qt::AlignCenter);
    kofi->onClick = [] {
        QDesktopServices::openUrl(QUrl("https://ko-fi.com/P5P81EV1M0"));
    };
    movie->start();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(text);
    layout->addWidget(kofi, 0, Qt::AlignHCenter);
    layout->addWidget(buttons);
    dlg.exec();
}

}  // namespace pcbview::app
