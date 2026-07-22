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
#include <QListWidget>
#include <QShortcut>
#include <QSpinBox>
#include <QCheckBox>
#include <QToolButton>
#include <QMenu>
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
#include "io/altium/altium_pcb.h"
#include "io/gerber/gerber_project.h"
#include "video/mf_encoder.h"
#include "io/ipc2581/ipc2581.h"
#include "io/kicad/kicad_importer.h"
#include "io/odb/odb_project.h"
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
        "<b>.zip</b>, a <b>.gbrjob</b>, an ODB++ job (<b>.tgz</b> or "
        "folder), or a gerber folder here"
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
    buildShowcaseDock();
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
        QTimer::singleShot(200, this, [this] {
            viewport_->setPathTracing(true);
            applyInternalResForMode();
        });

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

    // Headless layer-hiding hook: comma-separated part names to switch off
    // after load. Layer visibility is a checkbox a capture harness cannot
    // click, which makes "is that copper or silkscreen?" unanswerable
    // headlessly -- and answering it by eye is exactly how a wrong material
    // gets confirmed rather than caught.
    if (qEnvironmentVariableIsSet("PCBVIEW_HIDE")) {
        const QStringList hide =
            qEnvironmentVariable("PCBVIEW_HIDE").split(',', Qt::SkipEmptyParts);
        QTimer::singleShot(500, this, [this, hide] {
            for (const QString& h : hide)
                viewport_->renderer()->setPartVisible(h.trimmed().toStdString(),
                                                      false);
            // Keep the stackup checkboxes honest about what is showing.
            for (int i = 0; i < stackup_->topLevelItemCount(); ++i)
                syncStackupChecks(stackup_->topLevelItem(i), hide);
            viewport_->requestUpdate();
        });
    }

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

    // Headless/demo showcase hook: "top:2;iso:3;bottom:2" (kind defaults to
    // "view" when a step has two fields) loads that playlist and starts it
    // on repeat. Deferred like PCBVIEW_NET so the board is loaded first.
    if (qEnvironmentVariableIsSet("PCBVIEW_SHOWCASE")) {
        const QString packed = qEnvironmentVariable("PCBVIEW_SHOWCASE");
        QTimer::singleShot(900, this, [this, packed] {
            showcaseSteps_.clear();
            for (const QString& part :
                 packed.split(';', Qt::SkipEmptyParts)) {
                const QStringList f = part.split(':');
                ShowcaseStep st;
                if (f.size() == 3) {
                    st.kind = f[0];
                    st.param = f[1];
                    st.holdSec = f[2].toDouble();
                } else if (f.size() == 2) {
                    st.param = f[0];
                    st.holdSec = f[1].toDouble();
                } else {
                    continue;
                }
                showcaseSteps_.push_back(st);
            }
            refreshShowcaseList();
            if (showcaseForever_) showcaseForever_->setChecked(true);
            startShowcase();
        });
    }

    // Headless video hook: PCBVIEW_RECORD="out.mp4|720|30|h265|0" records
    // the playlist (from PCBVIEW_SHOWCASE or the saved one) and quits.
    if (qEnvironmentVariableIsSet("PCBVIEW_RECORD")) {
        const QStringList f = qEnvironmentVariable("PCBVIEW_RECORD").split('|');
        QTimer::singleShot(1500, this, [this, f] {
            // "1920x1080" = exact; a bare height keeps the window aspect.
            const QString res = f.value(1, "720");
            QSize target;
            if (res.contains('x')) {
                target = QSize(res.section('x', 0, 0).toInt() & ~1,
                               res.section('x', 1, 1).toInt() & ~1);
            } else {
                const int h = res.toInt() & ~1;
                const double windowAspect =
                    viewport_->height() > 0
                        ? static_cast<double>(viewport_->width()) /
                              viewport_->height()
                        : 16.0 / 9.0;
                target = QSize(static_cast<int>(h * windowAspect) & ~1, h);
            }
            startVideoRecording(f.value(0), target, f.value(2, "30").toInt(),
                                f.value(3, "h265") != "h264",
                                f.value(4, "0").toInt(),
                                /*quitWhenDone=*/true);
        });
    }

    // Headless measurement hook: pin a measurement between two world points
    // (x1,y1,z1,x2,y2,z2 in mm) -- mouse picks can't be synthesised. Deferred
    // like PCBVIEW_NET: the nets it resolves against need the board loaded,
    // and firing synchronously raced the load (mesh sometimes absent).
    if (qEnvironmentVariableIsSet("PCBVIEW_MEASURE")) {
        const QStringList p = qEnvironmentVariable("PCBVIEW_MEASURE").split(',');
        if (p.size() == 6) {
            QTimer::singleShot(700, this, [this, p] {
                viewport_->setMeasurement(p[0].toFloat(), p[1].toFloat(),
                                          p[2].toFloat(), p[3].toFloat(),
                                          p[4].toFloat(), p[5].toFloat());
            });
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
    // Exploded-view substrate opacity: persisted, dialog-adjustable, with an
    // env hook for headless captures (PCBVIEW_PEEL_ALPHA=0.12).
    peelAlpha_ = qEnvironmentVariableIsSet("PCBVIEW_PEEL_ALPHA")
                     ? qEnvironmentVariable("PCBVIEW_PEEL_ALPHA").toFloat()
                     : appSettings().value("peelAlpha", 0.02f).toFloat();
    // Silk clipping changes GEOMETRY, so the board must be re-assembled -- by
    // this point in the constructor it has already been built once with the
    // default. Same pattern as PCBVIEW_THICKNESS above.
    {
        const bool clipSilk =
            appSettings().value("subtractMaskFromSilk", false).toBool() ||
            qEnvironmentVariable("PCBVIEW_SILK_CLIP").toInt() != 0;
        if (clipSilk != subtractMaskFromSilk_) {
            subtractMaskFromSilk_ = clipSilk;
            reassemble();
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
        // Point the internal-resolution slider at the scale remembered for this
        // device + mode. Idempotent (setRenderScale early-returns if unchanged),
        // so ordinary board loads never disturb a scale already dialled in.
        applyInternalResForMode();
        // A rebuilt viewport (device switch) brings a renderer that has never
        // heard of the current net selection -- the highlight and its chase
        // just vanished from the board while the list still showed them
        // selected. Re-push, gated on an explicit rebuild flag: this signal
        // also fires on fresh board LOADS, where it arrives before
        // populateNets() clears the old board's selection -- re-applying
        // there would paint the previous board's net ids onto the new one.
        if (reapplyNetsOnUpload_) {
            reapplyNetsOnUpload_ = false;
            if (!highlightedNets_.empty()) applyNetHighlights();
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
    connect(viewport_, &VulkanWindow::moveRecordToggled, this,
            &MainWindow::toggleMoveRecording);
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
    // The internal-resolution scale is remembered per device + mode and
    // re-applied on the first board upload (applyInternalResForMode), so nothing
    // needs to be carried across the rebuild here. The net selection DOES need
    // a re-push -- it lives in this window but the renderer copy dies with the
    // viewport -- and the flag keeps that re-push out of ordinary board loads.
    reapplyNetsOnUpload_ = true;

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
    // A running showcase must not keep driving the camera across a board
    // swap.
    stopShowcase();
    QApplication::setOverrideCursor(Qt::WaitCursor);

    // A .kicad_pcb has full semantics; an ODB++ job resolves to LayerArt with
    // real nets from eda/data; anything else (a .zip, a folder, a .gbrjob) is
    // a Gerber package. ODB++ is probed before Gerber because both accept
    // folders and .zips -- matrix/matrix inside is what tells them apart.
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
        } else if (odb::isOdbJob(path.toStdString())) {
            gerber = true;  // "no BoardModel" path, same as a Gerber package
            // PCBVIEW_ODB_STEP opens a specific step -- e.g. a panel step,
            // whose step-repeats are expanded into the panel view.
            art = odb::importJob(
                path.toStdString(),
                qEnvironmentVariable("PCBVIEW_ODB_STEP").toStdString());
            for (const std::string& w : art.warnings)
                warnings << QString::fromStdString(w);
        } else if (ipc2581::isIpc2581(path.toStdString())) {
            gerber = true;
            art = ipc2581::importFile(path.toStdString());
            for (const std::string& w : art.warnings)
                warnings << QString::fromStdString(w);
        } else if (altium::isPcbDoc(path.toStdString())) {
            gerber = true;
            art = altium::importPcbDoc(path.toStdString());
            for (const std::string& w : art.warnings)
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
        d << "nets=" << baseArt_.nets.size()
          << " segments=" << baseArt_.netSegments.size()
          << " points=" << baseArt_.netPoints.size();
        double routed = 0.0;
        for (const auto& n : baseArt_.nets) routed += n.routedMm;
        d << " routedSum=" << routed << "mm\n";
        d << "barrels paths=" << baseArt_.barrels.size()
          << " area=" << Clipper2Lib::Area(baseArt_.barrels) / 1e12 << "mm2\n";
        // Report the UNIONED area alongside the raw sum. `art` is a raw pile of
        // overlapping tracks/pads/vias on the KiCad path but a single
        // composited image on the Gerber path, so the raw sum double-counts
        // overlaps on one side and not the other. Comparing the two importers
        // on it is apples-to-oranges and reads as a large geometry discrepancy
        // that does not exist; the union is the only number worth
        // cross-validating.
        for (const auto& al : baseArt_.layers) {
            const auto merged = Clipper2Lib::BooleanOp(
                Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero,
                al.art, Clipper2Lib::Paths64{});
            d << "layer " << al.name << " kind=" << int(al.kind)
              << " z=" << al.z << " paths=" << al.art.size()
              << " area=" << Clipper2Lib::Area(al.art) / 1e12 << "mm2"
              << " union=" << Clipper2Lib::Area(merged) / 1e12 << "mm2 "
              << bbox(al.art) << "\n";
            // Per-path detail on small layers. Aggregates cannot see a shape
            // that is mirrored or rotated wrongly -- area and bbox are both
            // invariant under mirroring -- so a fixture board needs the
            // individual shapes to compare against another importer.
            if (merged.size() <= 40) {
                for (size_t pi = 0; pi < merged.size(); ++pi) {
                    const auto& path = merged[pi];
                    long double cx = 0, cy = 0, a2 = 0;
                    for (size_t i = 0; i < path.size(); ++i) {
                        const auto& p0 = path[i];
                        const auto& p1 = path[(i + 1) % path.size()];
                        const long double cr =
                            static_cast<long double>(p0.x) * p1.y -
                            static_cast<long double>(p1.x) * p0.y;
                        a2 += cr;
                        cx += (p0.x + p1.x) * cr;
                        cy += (p0.y + p1.y) * cr;
                    }
                    if (a2 == 0) continue;
                    cx /= (3.0L * a2);
                    cy /= (3.0L * a2);
                    // Principal-axis angle. Area, centroid and bounding box are
                    // ALL invariant under mirroring and under swapping the sign
                    // of a rotation, so none of them can tell a pad rotated +45
                    // from one rotated -45. This can, and that distinction is
                    // exactly what a rotation convention gets wrong.
                    long double ixx = 0, iyy = 0, ixy = 0;
                    for (size_t i = 0; i < path.size(); ++i) {
                        const size_t j = (i + 1) % path.size();
                        const long double x0 = path[i].x - cx;
                        const long double y0 = path[i].y - cy;
                        const long double x1 = path[j].x - cx;
                        const long double y1 = path[j].y - cy;
                        const long double cr = x0 * y1 - x1 * y0;
                        ixx += (y0 * y0 + y0 * y1 + y1 * y1) * cr;
                        iyy += (x0 * x0 + x0 * x1 + x1 * x1) * cr;
                        ixy += (x0 * y1 + 2 * (x0 * y0 + x1 * y1) + x1 * y0) * cr;
                    }
                    const double angle =
                        0.5 *
                        std::atan2(2.0 * static_cast<double>(ixy),
                                   static_cast<double>(iyy) -
                                       static_cast<double>(ixx)) *
                        180.0 / 3.14159265358979;
                    d << "   path[" << pi << "] pts=" << path.size()
                      << " area=" << Clipper2Lib::Area(path) / 1e12
                      << "mm2 centroid=" << static_cast<double>(cx) / 1e6 << ","
                      << static_cast<double>(cy) / 1e6 << " axis=" << angle
                      << "deg\n";
                }
            }
        }
        // Soldermask openings with NO copper under them: bare laminate left
        // exposed. A thin ring around every pad is normal -- the mask relief
        // is deliberately larger than the pad -- so the total is what matters.
        for (const auto& mask : baseArt_.layers) {
            if (mask.kind != LayerKind::Soldermask) continue;
            const std::string side =
                mask.name.rfind("F.", 0) == 0 ? "F.Cu" : "B.Cu";
            for (const auto& cu : baseArt_.layers) {
                if (cu.name != side) continue;
                const auto bare = Clipper2Lib::BooleanOp(
                    Clipper2Lib::ClipType::Difference,
                    Clipper2Lib::FillRule::NonZero, mask.art, cu.art);
                d << "bare-laminate " << mask.name << " minus " << side
                  << " area=" << Clipper2Lib::Area(bare) / 1e12
                  << "mm2 regions=" << bare.size() << " " << bbox(bare) << "\n";
            }
        }
        for (const auto& w : baseArt_.warnings) d << "warn: " << w << "\n";
    }

    // A new board keeps any thickness override the user set, unless it is now
    // nonsensical; the override applies on top of baseArt_.
    mesh_ = thicknessOverride_ > 0.0
                ? [&] {
                      geom::LayerArt a = baseArt_;
                      geom::applyThickness(a, thicknessOverride_);
                      return geom::assemble(a, tessellateOptions());
                  }()
                : geom::assemble(baseArt_, tessellateOptions());

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

    updateImportReportBadge();

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
        "All supported (*.kicad_pcb *.zip *.gbrjob *.tgz *.tar.gz *.tar "
        "*.xml *.cvg *.PcbDoc);;"
        "KiCad PCB (*.kicad_pcb);;Gerber package (*.zip *.gbrjob);;"
        "ODB++ job (*.tgz *.tar.gz *.tar *.zip);;"
        "IPC-2581 (*.xml *.cvg);;Altium PCB (*.PcbDoc);;All files (*)");
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
    viewport_->renderer()->setPeelAlpha(peelAlpha_);
    viewport_->renderer()->setMaskColor(maskColor_[0], maskColor_[1],
                                        maskColor_[2], maskOpacity_);
    viewport_->renderer()->setComponentShine(fxComponentShine_ / 100.0f);
    viewport_->renderer()->setPadShine(fxPadShine_ / 100.0f);
    viewport_->renderer()->setShadowSoftness(fxShadowSoftness_ / 100.0f);
    viewport_->renderer()->setNetGlow(fxNetGlow_ * 0.16f);
    viewport_->requestUpdate();
}

// Tessellation options assembled from persisted view settings. Anything here
// changes GEOMETRY, so callers must re-assemble rather than just redraw.
geom::TessellateOptions MainWindow::tessellateOptions() const {
    geom::TessellateOptions o;
    o.subtractMaskFromSilk = subtractMaskFromSilk_;
    return o;
}

void MainWindow::reassemble() {
    if (!loaded_) return;
    geom::LayerArt a = baseArt_;
    if (thicknessOverride_ > 0.0) geom::applyThickness(a, thicknessOverride_);
    mesh_ = geom::assemble(a, tessellateOptions());
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
           localPath.endsWith(".tgz", Qt::CaseInsensitive) ||
           localPath.endsWith(".tar.gz", Qt::CaseInsensitive) ||
           localPath.endsWith(".tar", Qt::CaseInsensitive) ||
           localPath.endsWith(".xml", Qt::CaseInsensitive) ||
           localPath.endsWith(".cvg", Qt::CaseInsensitive) ||
           localPath.endsWith(".pcbdoc", Qt::CaseInsensitive) ||
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

    // Viewport size presets: pin the render area to an aspect for video
    // framing (the recorder keeps the viewport's aspect), or free it.
    QMenu* vpSize = view->addMenu("Viewport si&ze");
    const auto setViewportSize = [this](int w, int h) {
        if (!viewportContainer_) return;
        if (w <= 0) {
            viewportContainer_->setMinimumSize(0, 0);
            viewportContainer_->setMaximumSize(QWIDGETSIZE_MAX,
                                               QWIDGETSIZE_MAX);
        } else {
            viewportContainer_->setFixedSize(w, h);
        }
    };
    vpSize->addAction("Free (fill window)", this,
                      [setViewportSize] { setViewportSize(0, 0); });
    vpSize->addSeparator();
    vpSize->addAction("16:9 — 1280 × 720", this,
                      [setViewportSize] { setViewportSize(1280, 720); });
    vpSize->addAction("16:9 — 1600 × 900", this,
                      [setViewportSize] { setViewportSize(1600, 900); });
    vpSize->addAction("21:9 — 1290 × 552", this,
                      [setViewportSize] { setViewportSize(1290, 552); });
    vpSize->addAction("1:1 — 900 × 900", this,
                      [setViewportSize] { setViewportSize(900, 900); });
    vpSize->addAction("9:16 portrait — 540 × 960", this,
                      [setViewportSize] { setViewportSize(540, 960); });
    vpSize->addAction("Custom…", this, [this, setViewportSize] {
        bool ok = false;
        const int w = QInputDialog::getInt(this, "Viewport size", "Width:",
                                           1280, 200, 4000, 10, &ok);
        if (!ok) return;
        const int h = QInputDialog::getInt(this, "Viewport size", "Height:",
                                           720, 200, 3000, 10, &ok);
        if (!ok) return;
        setViewportSize(w, h);
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

    // Ray-traced shadows + AO used to be a toggle here. It is now always on:
    // the CPU device renders everything through Embree (where RT-on measured
    // FASTER-converging and better-looking than the flat preview), and on a
    // GPU the ray-query cost only applies at rest. PCBVIEW_RT=0 remains as a
    // headless hook for the flat preview.
    QAction* pt = render->addAction("&Path tracing (full-scene lighting)");
    pt->setCheckable(true);
    connect(pt, &QAction::toggled, this, [this](bool on) {
        viewport_->setPathTracing(on);
        applyInternalResForMode();
    });

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
            [this, pt, oidn, fastMove, gpuMenu] {
        const bool haveRenderer = viewport_->renderer() != nullptr;
        const bool ptAvail = haveRenderer && viewport_->ptAvailable();
        const bool ptOn = viewport_->pathTracing();
        pt->setEnabled(ptAvail);
        oidn->setEnabled(ptAvail && ptOn);
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
            const QSignalBlocker b2(pt), b3(oidn), b4(fastMove);
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

    // Camera readout. Persisted, and reachable headlessly, because its whole
    // point is that a capture carries the view that produced it.
    {
        auto* hud = view->addAction("Camera readout");
        hud->setCheckable(true);
        hud->setShortcut(QKeySequence("Ctrl+`"));
        hud->setStatusTip(
            "Draw yaw/pitch/roll, distance, target and mm-per-pixel into the "
            "frame — appears in screenshots, so a view can be reproduced");
        const bool on = appSettings().value("cameraHud", false).toBool() ||
                        qEnvironmentVariable("PCBVIEW_CAMERA_HUD").toInt() != 0;
        hud->setChecked(on);
        connect(hud, &QAction::toggled, this, [this](bool v) {
            appSettings().setValue("cameraHud", v);
            viewport_->setCameraHud(v);
        });
        viewport_->setCameraHud(on);
    }
    // Silkscreen clipping. KiCad's own (subtractmaskfromsilk) decides what is
    // PLOTTED and defaults to off, so by default we draw the ink the file
    // carries -- including over pads. Many fabs clip it anyway, because ink
    // will not stick to solder, so this shows the board as such a house builds
    // it. Changing it re-tessellates.
    {
        auto* clip = view->addAction("Clip silkscreen off pads");
        clip->setCheckable(true);
        clip->setStatusTip(
            "Remove silkscreen ink that lands on exposed copper, as a fab that "
            "clips silk would build it");
        clip->setChecked(subtractMaskFromSilk_);
        connect(clip, &QAction::toggled, this, [this](bool on) {
            subtractMaskFromSilk_ = on;
            appSettings().setValue("subtractMaskFromSilk", on);
            reassemble();
            viewport_->requestUpdate();
        });
    }
    view->addSeparator();

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

    // Filter plus a Clear button: the list drives highlighting through Qt
    // selection, and plain clicks can only ever REPLACE a selection -- so
    // without this there is no discoverable way back to "nothing selected"
    // (the other routes: Esc, or clicking bare board).
    auto* filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(4);
    netFilter_ = new QLineEdit;
    netFilter_->setPlaceholderText("Filter nets…");
    netFilter_->setClearButtonEnabled(true);
    filterRow->addWidget(netFilter_, 1);
    clearNetsBtn_ = new QPushButton("Clear");
    clearNetsBtn_->setToolTip(
        "Un-highlight every net (Esc, or click bare board, does the same)");
    clearNetsBtn_->setVisible(false);
    connect(clearNetsBtn_, &QPushButton::clicked, this,
            [this] { highlightNet(-1); });
    filterRow->addWidget(clearNetsBtn_);
    layout->addLayout(filterRow);

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
    // Esc while the panel has focus = the same clear.
    auto* escClear = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
    escClear->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escClear, &QShortcut::activated, this, [this] { highlightNet(-1); });

    dock->setContent(panel);
    dock->setMinimumWidth(230);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    netDock_ = dock;
}

// ---- showcase --------------------------------------------------------------
//
// A playlist of views played on a timer: each step animates to its view,
// waits for the glide to settle, holds, then advances; the list loops N
// times or forever. The step model is deliberately a (kind, param, hold)
// triple so richer kinds -- explode levels, 180/360 spins, layer hiding,
// trace highlights -- can be added without touching the engine.

namespace {
// The step kinds offered today. label is UI text; kind/param feed the engine.
struct ShowcaseKindDef {
    const char* label;
    const char* kind;
    const char* param;
};
constexpr ShowcaseKindDef kShowcaseKinds[] = {
    {"Top view", "view", "top"},
    {"Bottom view", "view", "bottom"},
    {"Isometric view", "view", "iso"},
    {"Explode 25%", "explode", "25"},
    {"Explode 50%", "explode", "50"},
    {"Explode 75%", "explode", "75"},
    {"Explode 100%", "explode", "100"},
    {"Collapse (explode 0%)", "explode", "0"},
    // For spins the seconds box is the SWEEP duration, not a hold. Negative
    // degrees run the same sweep the other way. Direction words are the
    // USER'S perception, set by report: CW/CCW as originally derived,
    // flip/tumble words swapped from the derivation.
    {"Spin 360° CW (turntable)", "spin", "yaw,360"},
    {"Spin 360° CCW (turntable)", "spin", "yaw,-360"},
    {"Spin 180° CW (turntable)", "spin", "yaw,180"},
    {"Spin 180° CCW (turntable)", "spin", "yaw,-180"},
    {"Flip 360° leftward", "spin", "flip,360"},
    {"Flip 360° rightward", "spin", "flip,-360"},
    {"Flip 180° leftward (show back)", "spin", "flip,180"},
    {"Flip 180° rightward (show back)", "spin", "flip,-180"},
    {"Tumble 360° backward (pitch)", "spin", "pitch,-360"},
    {"Tumble 360° forward (pitch)", "spin", "pitch,360"},
    {"Tumble 180° backward (pitch)", "spin", "pitch,-180"},
    {"Tumble 180° forward (pitch)", "spin", "pitch,180"},
    {"Twist 360° CW (roll)", "spin", "roll,360"},
    {"Twist 360° CCW (roll)", "spin", "roll,-360"},
    // Zoom to a percentage of the default framed size over the step's
    // seconds. Angles and target stay put -- place the board manually, then
    // let the step fly it in (or out).
    {"Zoom to 100% (framed size)", "zoom", "100"},
    {"Zoom to 150%", "zoom", "150"},
    {"Zoom to 200% (close-up)", "zoom", "200"},
    {"Zoom to 50% (pull back)", "zoom", "50"},
    {"Zoom to 25% (far)", "zoom", "25"},
    {"Zoom to custom %…", "zoom", "custom"},
};
}  // namespace

void MainWindow::buildShowcaseDock() {
    auto* dock = new CollapsibleDock("SHOWCASE", Qt::RightDockWidgetArea, this);

    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Add-step row: what + how long.
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(4);
    showcaseKind_ = new QComboBox;
    for (const auto& k : kShowcaseKinds) showcaseKind_->addItem(k.label);
    addRow->addWidget(showcaseKind_, 1);
    showcaseHold_ = new QDoubleSpinBox;
    showcaseHold_->setRange(0.0, 600.0);
    showcaseHold_->setValue(3.0);
    showcaseHold_->setDecimals(1);
    showcaseHold_->setSuffix(" s");
    showcaseHold_->setToolTip(
        "How long to hold this step once it settles.\n"
        "For spins this is the sweep duration instead.");
    addRow->addWidget(showcaseHold_);
    auto* addBtn = new QPushButton("Add");
    connect(addBtn, &QPushButton::clicked, this, [this] {
        const auto& k = kShowcaseKinds[showcaseKind_->currentIndex()];
        ShowcaseStep s;
        s.kind = k.kind;
        s.param = k.param;
        s.holdSec = showcaseHold_->value();
        if (s.kind == "zoom" && s.param == "custom") {
            bool ok = false;
            const int pct = QInputDialog::getInt(
                this, "Zoom step", "Zoom to (% of framed size):", 100, 1,
                1000, 5, &ok);
            if (!ok) return;
            s.param = QString::number(pct);
        }
        showcaseSteps_.push_back(s);
        refreshShowcaseList();
        saveShowcase();
    });
    addRow->addWidget(addBtn);
    layout->addLayout(addRow);

    // The playlist. Drag to reorder; the model follows the widget order.
    showcaseList_ = new QListWidget;
    showcaseList_->setDragDropMode(QAbstractItemView::InternalMove);
    showcaseList_->setToolTip(
        "Drag to reorder; Delete removes; double-click to change the "
        "seconds");
    connect(showcaseList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const int row = showcaseList_->row(item);
                if (row < 0 || row >= static_cast<int>(showcaseSteps_.size()))
                    return;
                ShowcaseStep& s = showcaseSteps_[row];
                bool ok = false;
                const double v = QInputDialog::getDouble(
                    this, "Step duration",
                    s.kind == "spin" ? "Sweep duration (seconds):"
                                     : "Hold time (seconds):",
                    s.holdSec, 0.0, 600.0, 1, &ok);
                if (!ok) return;
                s.holdSec = v;
                refreshShowcaseList();
                saveShowcase();
            });
    connect(showcaseList_->model(), &QAbstractItemModel::rowsMoved, this,
            [this] {
                std::vector<ShowcaseStep> reordered;
                for (int i = 0; i < showcaseList_->count(); ++i) {
                    const int from =
                        showcaseList_->item(i)->data(Qt::UserRole).toInt();
                    if (from >= 0 &&
                        from < static_cast<int>(showcaseSteps_.size()))
                        reordered.push_back(showcaseSteps_[from]);
                }
                showcaseSteps_ = std::move(reordered);
                refreshShowcaseList();
                saveShowcase();
            });
    showcaseList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(showcaseList_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QListWidgetItem* item = showcaseList_->itemAt(pos);
                if (!item) return;
                const int row = showcaseList_->row(item);
                if (row < 0 ||
                    row >= static_cast<int>(showcaseSteps_.size()))
                    return;
                QMenu menu(showcaseList_);
                QAction* retime = menu.addAction("Change seconds…");
                QAction* del = menu.addAction("Delete step");
                QAction* chosen =
                    menu.exec(showcaseList_->mapToGlobal(pos));
                if (chosen == del) {
                    const ShowcaseStep removed = showcaseSteps_[row];
                    showcaseSteps_.erase(showcaseSteps_.begin() + row);
                    // A custom move nobody references any more can go too.
                    if (removed.kind == "path") {
                        bool used = false;
                        for (const ShowcaseStep& s : showcaseSteps_)
                            if (s.kind == "path" && s.param == removed.param)
                                used = true;
                        if (!used) showcasePaths_.erase(removed.param);
                    }
                    refreshShowcaseList();
                    saveShowcase();
                } else if (chosen == retime) {
                    ShowcaseStep& s = showcaseSteps_[row];
                    bool ok = false;
                    const double v = QInputDialog::getDouble(
                        this, "Step duration",
                        s.kind == "spin" || s.kind == "path" ||
                                s.kind == "zoom"
                            ? "Motion duration (seconds):"
                            : "Hold time (seconds):",
                        s.holdSec, 0.0, 600.0, 1, &ok);
                    if (!ok) return;
                    s.holdSec = v;
                    refreshShowcaseList();
                    saveShowcase();
                }
            });
    auto* delSc = new QShortcut(QKeySequence::Delete, showcaseList_);
    delSc->setContext(Qt::WidgetShortcut);
    connect(delSc, &QShortcut::activated, this, [this] {
        const int row = showcaseList_->currentRow();
        if (row < 0 || row >= static_cast<int>(showcaseSteps_.size())) return;
        showcaseSteps_.erase(showcaseSteps_.begin() + row);
        refreshShowcaseList();
        saveShowcase();
    });
    layout->addWidget(showcaseList_);

    // Loop controls + transport.
    auto* playRow = new QHBoxLayout;
    playRow->setSpacing(4);
    playRow->addWidget(new QLabel("Loops:"));
    showcaseLoops_ = new QSpinBox;
    showcaseLoops_->setRange(1, 999);
    showcaseLoops_->setValue(1);
    playRow->addWidget(showcaseLoops_);
    showcaseForever_ = new QCheckBox("Repeat");
    showcaseForever_->setToolTip("Loop until stopped");
    connect(showcaseForever_, &QCheckBox::toggled, this,
            [this](bool on) { showcaseLoops_->setEnabled(!on); });
    playRow->addWidget(showcaseForever_);
    playRow->addStretch(1);
    moveRecBtn_ = new QPushButton("Rec move");
    moveRecBtn_->setToolTip(
        "Record your own camera movement (drag, zoom, explode) as a step.\n"
        "Start, drive the board, stop. The R key toggles this too, so the\n"
        "mouse stays free for the movement itself.");
    connect(moveRecBtn_, &QPushButton::clicked, this,
            &MainWindow::toggleMoveRecording);
    playRow->addWidget(moveRecBtn_);
    auto* recordBtn = new QPushButton("Record…");
    recordBtn->setToolTip(
        "Render the playlist to an MP4 (H.265 when the GPU can, else "
        "H.264).\nEvery frame is fully converged and denoised before it is "
        "encoded.");
    connect(recordBtn, &QPushButton::clicked, this,
            &MainWindow::recordShowcaseVideo);
    playRow->addWidget(recordBtn);
    showcasePlay_ = new QPushButton("Play");
    connect(showcasePlay_, &QPushButton::clicked, this, [this] {
        if (showcaseIndex_ >= 0) stopShowcase("stopped");
        else startShowcase();
    });
    playRow->addWidget(showcasePlay_);
    layout->addLayout(playRow);

    connect(showcaseLoops_, &QSpinBox::valueChanged, this,
            [this] { saveShowcase(); });
    connect(showcaseForever_, &QCheckBox::toggled, this,
            [this] { saveShowcase(); });

    showcaseTimer_ = new QTimer(this);
    showcaseTimer_->setSingleShot(true);

    dock->setContent(panel);
    dock->setMinimumWidth(230);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    showcaseDock_ = dock;

    loadShowcase();
}

void MainWindow::refreshShowcaseList() {
    if (!showcaseList_) return;
    const QSignalBlocker block(showcaseList_);
    showcaseList_->clear();
    for (size_t i = 0; i < showcaseSteps_.size(); ++i) {
        const ShowcaseStep& s = showcaseSteps_[i];
        QString label = s.param;
        if (s.kind == "path") label = "Custom move";
        else if (s.kind == "zoom") label = "Zoom to " + s.param + "%";
        for (const auto& k : kShowcaseKinds)
            if (s.kind == k.kind && s.param == k.param) label = k.label;
        auto* item = new QListWidgetItem(
            QString("%1  —  %2s").arg(label).arg(s.holdSec, 0, 'f', 1));
        item->setData(Qt::UserRole, static_cast<int>(i));
        showcaseList_->addItem(item);
    }
}

// Sample the live camera at 20Hz while the user drives; stopping turns the
// take into a "path" step whose seconds field is the recorded duration
// (retime it like any step -- playback stretches to fit).
void MainWindow::toggleMoveRecording() {
    if (!moveRecTimer_) {
        moveRecTimer_ = new QTimer(this);
        moveRecTimer_->setInterval(50);
        connect(moveRecTimer_, &QTimer::timeout, this, [this] {
            const Camera& c = viewport_->cameraPose();
            VulkanWindow::PathKey k;
            k.yaw = c.yaw;
            k.pitch = c.pitch;
            k.roll = c.roll;
            k.distance = c.distance;
            k.tx = c.targetX;
            k.ty = c.targetY;
            k.tz = c.targetZ;
            k.explode = viewport_->explodeProgress();
            // Unwrap angles against the previous key so interpolation never
            // takes a 2-pi shortcut through the wrap.
            if (!moveRecKeys_.empty()) {
                const auto unwrap = [](float prev, float v) {
                    constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;
                    while (v - prev > kPi) v -= kTwoPi;
                    while (v - prev < -kPi) v += kTwoPi;
                    return v;
                };
                const VulkanWindow::PathKey& p = moveRecKeys_.back();
                k.yaw = unwrap(p.yaw, k.yaw);
                k.pitch = unwrap(p.pitch, k.pitch);
                k.roll = unwrap(p.roll, k.roll);
            }
            moveRecKeys_.push_back(k);
            moveRecBtn_->setText(
                QString("Stop (%1s)").arg(moveRecKeys_.size() * 0.05, 0, 'f',
                                          1));
        });
    }
    if (!moveRecTimer_->isActive()) {
        if (!loaded_) return;
        stopShowcase();
        moveRecKeys_.clear();
        moveRecTimer_->start();
        statusBar()->showMessage(
            "Recording movement — drive the board, then click Stop.");
        return;
    }
    moveRecTimer_->stop();
    moveRecBtn_->setText("Rec move");
    if (moveRecKeys_.size() < 4) {
        statusBar()->showMessage("Movement too short — not added.", 4000);
        return;
    }
    const QString id = QString("p%1").arg(nextPathId_++);
    const double duration = moveRecKeys_.size() * 0.05;
    showcasePaths_[id] = std::move(moveRecKeys_);
    moveRecKeys_.clear();
    ShowcaseStep s;
    s.kind = "path";
    s.param = id;
    s.holdSec = duration;
    showcaseSteps_.push_back(s);
    refreshShowcaseList();
    saveShowcase();
    statusBar()->showMessage(
        QString("Custom movement added (%1s).").arg(duration, 0, 'f', 1),
        4000);
}

void MainWindow::applyShowcaseStep(const ShowcaseStep& step) {
    if (step.kind == "view") {
        if (step.param == "top") viewport_->setViewTop();
        else if (step.param == "bottom") viewport_->setViewBottom();
        else viewport_->setViewIso();
    } else if (step.kind == "explode") {
        // param = percent of the full peel; the renderer owns what "full"
        // means for this board.
        const float frac = step.param.toFloat() / 100.0f;
        const float full =
            viewport_->renderer() ? viewport_->renderer()->maxRank() : 0.0f;
        viewport_->setExplodeProgress(frac * full);
    } else if (step.kind == "spin") {
        // param = "axis,degrees"; the step's seconds are the sweep time.
        const QStringList f = step.param.split(',');
        const int axis = f.value(0) == "pitch" ? 1
                         : f.value(0) == "roll" ? 2
                         : f.value(0) == "flip" ? 3
                                                : 0;
        const float deg = f.size() > 1 ? f[1].toFloat() : 360.0f;
        viewport_->startSpin(axis, deg,
                             static_cast<float>(std::max(step.holdSec, 0.5)));
    } else if (step.kind == "path") {
        const auto it = showcasePaths_.find(step.param);
        if (it != showcasePaths_.end())
            viewport_->startPath(it->second, std::max(step.holdSec, 0.2));
    } else if (step.kind == "zoom") {
        viewport_->startTimedZoom(std::max(1.0f, step.param.toFloat()),
                                  static_cast<float>(
                                      std::max(step.holdSec, 0.2)));
    }
    // Future kinds ("layers", "net") dispatch here.
}

void MainWindow::startShowcase() {
    if (showcaseSteps_.empty() || !loaded_) return;
    showcaseIndex_ = 0;
    showcaseLoopsDone_ = 0;
    showcasePlay_->setText("Stop");
    showcaseAdvance();
}

void MainWindow::stopShowcase(const QString& reason) {
    if (showcaseIndex_ < 0) return;
    showcaseIndex_ = -1;
    showcaseTimer_->stop();
    showcaseTimer_->disconnect();
    if (showcasePlay_) showcasePlay_->setText("Play");
    if (!reason.isEmpty())
        statusBar()->showMessage("Showcase " + reason, 3000);
}

// Apply the current step, poll until its animation settles, hold, advance.
// One QTimer serves both phases; each connect replaces the last.
void MainWindow::showcaseAdvance() {
    if (showcaseIndex_ < 0 ||
        showcaseIndex_ >= static_cast<int>(showcaseSteps_.size())) {
        // End of the list: loop or finish.
        ++showcaseLoopsDone_;
        const bool repeatOn = showcaseForever_ && showcaseForever_->isChecked();
        if (!repeatOn && showcaseLoopsDone_ >= showcaseLoops_->value()) {
            stopShowcase("finished");
            return;
        }
        showcaseIndex_ = 0;
    }

    const ShowcaseStep step = showcaseSteps_[showcaseIndex_];
    const bool repeatOn = showcaseForever_ && showcaseForever_->isChecked();
    statusBar()->showMessage(
        QString("Showcase %1/%2%3")
            .arg(showcaseIndex_ + 1)
            .arg(showcaseSteps_.size())
            .arg(repeatOn ? QString("  ·  repeat")
                         : QString("  ·  loop %1/%2")
                               .arg(showcaseLoopsDone_ + 1)
                               .arg(showcaseLoops_->value())));
    applyShowcaseStep(step);

    // Phase 1: wait for the glide to settle, checking a few times a second.
    showcaseTimer_->disconnect();
    connect(showcaseTimer_, &QTimer::timeout, this, [this, step] {
        if (viewport_->viewAnimating()) {
            showcaseTimer_->start(100);
            return;
        }
        // Phase 2: the hold, then the next step. A spin's seconds WERE the
        // sweep just completed, so it advances immediately.
        showcaseTimer_->disconnect();
        connect(showcaseTimer_, &QTimer::timeout, this, [this] {
            ++showcaseIndex_;
            showcaseAdvance();
        });
        const double holdMs =
            step.kind == "spin" || step.kind == "path" ||
                    step.kind == "zoom"
                ? 0.0
                : step.holdSec * 1000.0;
        showcaseTimer_->start(static_cast<int>(holdMs));
    });
    showcaseTimer_->start(100);
}

// ---- showcase video recording ----------------------------------------------
//
// The same playlist the Play button runs, rendered offline: animations are
// paused and advanced by exactly 1/fps per video frame, and each frame is
// captured through the converge-then-grab path the screenshot export uses --
// so a path-traced frame is fully accumulated AND denoised before it lands
// in the file. Held (static) frames are encoded from the previous capture
// without re-rendering, which makes holds nearly free.

struct MainWindow::VideoJob {
    video::MfEncoder enc;
    bool encOpen = false;
    QString outPath;
    int fps = 30;
    bool preferHevc = true;
    int quality = 0;  // 0 standard, 1 high
    double dt = 1.0 / 30.0;
    std::vector<ShowcaseStep> steps;
    int stepIndex = 0;
    bool settling = true;
    double holdLeft = 0.0;
    int framesEncoded = 0;
    int estTotal = 1;
    bool quitWhenDone = false;  // the headless PCBVIEW_RECORD hook
    bool lastStatic = false;
    bool haveFrame = false;      // scratch holds the previous encoded frame
    // Pipelined capture state: the frame hook converges, arms the ring copy,
    // then the driver moves on while the GPU drains behind it.
    bool awaitingConverge = false, armedThisFrame = false;
    int pipeDepth = 1;
    int convergeBudget = 0;
    QMetaObject::Connection frameConn;
    std::vector<uint8_t> fetch;    // raw fetched pixels (full extent)
    std::vector<uint8_t> scratch;  // tight even-cropped copy for the encoder
    int encW = 0, encH = 0;
    QProgressDialog* progress = nullptr;
};

void MainWindow::recordShowcaseVideo() {
    if (!loaded_ || showcaseSteps_.empty()) {
        statusBar()->showMessage(
            "Add showcase steps first — the recorder renders the playlist.",
            5000);
        return;
    }
    if (videoJob_) return;
    stopShowcase();

    // ---- options dialog ----------------------------------------------------
    QDialog dlg(this);
    dlg.setWindowTitle("Record showcase video");
    auto* form = new QFormLayout(&dlg);
    auto* resBox = new QComboBox;
    resBox->addItem("Window size", QSize(0, 0));
    resBox->addItem("1920 × 1080", QSize(1920, 1080));
    resBox->addItem("2560 × 1440", QSize(2560, 1440));
    resBox->addItem("3840 × 2160 (4K)", QSize(3840, 2160));
    resBox->addItem("2560 × 1080 (21:9)", QSize(2560, 1080));
    resBox->addItem("1080 × 1920 (portrait)", QSize(1080, 1920));
    resBox->addItem("Custom…", QSize(-1, -1));
    resBox->setCurrentIndex(1);
    form->addRow("Resolution:", resBox);
    auto* fpsBox = new QComboBox;
    fpsBox->addItem("24", 24);
    fpsBox->addItem("30", 30);
    fpsBox->addItem("60", 60);
    fpsBox->setCurrentIndex(1);
    form->addRow("Frame rate:", fpsBox);
    auto* codecBox = new QComboBox;
    codecBox->addItem("H.265 (falls back to H.264)");
    codecBox->addItem("H.264");
    form->addRow("Codec:", codecBox);
    auto* qualityBox = new QComboBox;
    qualityBox->addItem("Standard");
    qualityBox->addItem("High");
    form->addRow("Quality:", qualityBox);

    double estSeconds = 0.0;
    for (const ShowcaseStep& s : showcaseSteps_)
        estSeconds += s.kind == "spin" || s.kind == "path" ||
                              s.kind == "zoom"
                          ? s.holdSec
                          : s.holdSec + 1.2;
    auto* info = new QLabel(
        QString("One pass of %1 steps ≈ %2 s of video.\nRendered offscreen "
                "at exactly the chosen size — the window preview may look "
                "stretched while recording.")
            .arg(showcaseSteps_.size())
            .arg(estSeconds, 0, 'f', 1));
    info->setWordWrap(true);
    form->addRow(info);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString suggested = QFileInfo(path_).absolutePath() + "/" +
                              QFileInfo(path_).completeBaseName() + ".mp4";
    const QString out = QFileDialog::getSaveFileName(
        this, "Save video", suggested, "MP4 video (*.mp4)");
    if (out.isEmpty()) return;

    QSize target = resBox->currentData().toSize();
    if (target.width() < 0) {  // Custom…
        bool ok = false;
        const int w = QInputDialog::getInt(this, "Video size", "Width:", 1920,
                                           320, 7680, 2, &ok);
        if (!ok) return;
        const int h = QInputDialog::getInt(this, "Video size", "Height:",
                                           1080, 240, 4320, 2, &ok);
        if (!ok) return;
        target = QSize(w & ~1, h & ~1);
    }
    startVideoRecording(out, target, fpsBox->currentData().toInt(),
                        codecBox->currentIndex() == 0,
                        qualityBox->currentIndex(), /*quitWhenDone=*/false);
}

void MainWindow::startVideoRecording(const QString& outPath, QSize target,
                                     int fps, bool preferHevc, int quality,
                                     bool quitWhenDone) {
    if (videoJob_ || showcaseSteps_.empty()) return;
    stopShowcase();

    auto* job = new VideoJob;
    job->outPath = outPath;
    job->fps = std::clamp(fps, 1, 120);
    job->dt = 1.0 / job->fps;
    job->preferHevc = preferHevc;
    job->quality = quality;
    job->quitWhenDone = quitWhenDone;
    job->steps = showcaseSteps_;
    double estSeconds = 0.0;
    for (const ShowcaseStep& s : job->steps)
        estSeconds += s.kind == "spin" || s.kind == "path" ||
                              s.kind == "zoom"
                          ? s.holdSec
                          : s.holdSec + 1.2;
    job->estTotal = std::max(
        1, static_cast<int>(std::ceil(estSeconds * job->fps)));

    vk::Renderer* r = viewport_->renderer();
    if (!r) {
        delete job;
        return;
    }
    // Render offscreen at EXACTLY the requested size, projection to match --
    // 4K video from any window, menus untouched. (0,0) keeps window x scale.
    if (target.width() > 0 && target.height() > 0) {
        r->setCaptureExtent(static_cast<uint32_t>(target.width()),
                            static_cast<uint32_t>(target.height()));
        viewport_->setAspectOverride(static_cast<float>(target.width()) /
                                     static_cast<float>(target.height()));
    }
    r->setUncappedPresent(true);  // render at GPU speed, not display speed
    viewport_->setAnimationsPaused(true);
    // Ring depth is re-budgeted for THIS recording's extent -- N+3 when the
    // host-visible heap allows it, less when it doesn't.
    job->pipeDepth = std::max(1, r->beginPipelinedCapture(3));

    // The frame hook: converge (accumulation + denoise), then arm the
    // in-frame ring copy. The frame AFTER the armed one confirms the copy is
    // queued; the driver then advances immediately -- the GPU finishes the
    // capture behind the next frame's work.
    job->frameConn = connect(viewport_, &VulkanWindow::frameRendered, this,
                             [this] {
        VideoJob* job = videoJob_;
        if (!job) return;
        vk::Renderer* r = viewport_->renderer();
        if (!r) return;
        if (job->armedThisFrame) {
            // The armed frame has been submitted; its copy is in the queue.
            job->armedThisFrame = false;
            videoEncodeCaptured();
            return;
        }
        if (!job->awaitingConverge) return;
        if (r->accumulating() && ++job->convergeBudget < 3000) {
            viewport_->requestUpdate();
            return;
        }
        job->awaitingConverge = false;
        r->armPipelinedCapture();
        job->armedThisFrame = true;
        viewport_->requestUpdate();
    });

    job->progress = new QProgressDialog("Rendering video…", "Cancel", 0,
                                        job->estTotal, this);
    job->progress->setWindowTitle("Showcase video");
    job->progress->setWindowModality(Qt::WindowModal);
    job->progress->setMinimumDuration(0);
    job->progress->setAutoClose(false);
    job->progress->setValue(0);

    videoJob_ = job;
    applyShowcaseStep(job->steps[0]);
    job->settling = true;
    QTimer::singleShot(0, this, &MainWindow::videoNextFrame);
}

void MainWindow::videoNextFrame() {
    VideoJob* job = videoJob_;
    if (!job) return;
    if (job->progress->wasCanceled()) {
        videoFinish("cancelled — partial video kept");
        return;
    }

    // Advance the virtual clock by one frame, then run the same
    // settle-then-hold logic the live player uses, in virtual time.
    viewport_->advanceAnimationsBy(job->dt);
    if (job->settling) {
        if (!viewport_->viewAnimating()) {
            job->settling = false;
            const ShowcaseStep& s = job->steps[job->stepIndex];
            job->holdLeft = s.kind == "spin" || s.kind == "path" ||
                                    s.kind == "zoom"
                                ? 0.0
                                : s.holdSec;
        }
    } else {
        job->holdLeft -= job->dt;
        if (job->holdLeft <= 0.0) {
            ++job->stepIndex;
            if (job->stepIndex >= static_cast<int>(job->steps.size())) {
                videoFinish({});
                return;
            }
            applyShowcaseStep(job->steps[job->stepIndex]);
            job->settling = true;
        }
    }

    job->progress->setLabelText(
        QString("Rendering video…  step %1/%2, frame %3")
            .arg(job->stepIndex + 1)
            .arg(job->steps.size())
            .arg(job->framesEncoded + 1));

    // A held frame is identical to the previous one: encode it again
    // without re-rendering. The ring must drain first, or duplicates would
    // jump the queue ahead of still-pending captures.
    const bool moving = viewport_->viewAnimating();
    if (!moving && job->lastStatic && job->haveFrame) {
        if (vk::Renderer* r = viewport_->renderer()) {
            while (r->pendingCaptures() > 0) {
                r->waitCapture(UINT64_MAX);
                if (!videoDrainOne()) return;  // encode error ended the job
            }
        }
        const std::string err = job->enc.writeFrame(job->scratch.data());
        if (!err.empty()) {
            videoFinish(QString::fromStdString(err));
            return;
        }
        ++job->framesEncoded;
        job->progress->setValue(
            std::min(job->framesEncoded, job->estTotal));
        QTimer::singleShot(0, this, &MainWindow::videoNextFrame);
        return;
    }

    // Kick the converge-then-arm sequence; the frameRendered hook takes it
    // from here.
    job->awaitingConverge = true;
    job->convergeBudget = 0;
    viewport_->requestUpdate();
}

// The armed frame's copy is queued on the GPU. Drain whatever fences have
// already signalled, block only when the ring is FULL, and advance to the
// next video frame -- the overlap is exactly the ring depth.
void MainWindow::videoEncodeCaptured() {
    VideoJob* job = videoJob_;
    if (!job) return;
    vk::Renderer* r = viewport_->renderer();
    if (!r) return;

    while (r->captureReady()) {
        if (!videoDrainOne()) return;
    }
    if (!r->captureSlotFree()) {
        r->waitCapture(UINT64_MAX);
        if (!videoDrainOne()) return;
    }
    job->lastStatic = !viewport_->viewAnimating();
    QTimer::singleShot(0, this, &MainWindow::videoNextFrame);
}

// Fetch and encode the oldest completed capture. False = the job ended
// (encoder error).
bool MainWindow::videoDrainOne() {
    VideoJob* job = videoJob_;
    if (!job) return false;
    vk::Renderer* r = viewport_->renderer();
    uint32_t cw = 0, ch = 0;
    if (!r || !r->fetchCapture(job->fetch, cw, ch)) return true;
    const int w = static_cast<int>(cw) & ~1;
    const int h = static_cast<int>(ch) & ~1;
    if (w <= 0 || h <= 0) {
        videoFinish("empty capture");
        return false;
    }

    if (!job->encOpen) {
        const double bpp = job->quality == 1 ? 0.20 : 0.10;
        const int bitrate = std::max(
            2'000'000,
            static_cast<int>(static_cast<double>(w) * h * job->fps * bpp));
        const std::string err =
            job->enc.open(job->outPath.toStdWString(), w, h, job->fps,
                          bitrate, job->preferHevc);
        if (!err.empty()) {
            videoFinish(QString::fromStdString(err));
            return false;
        }
        job->encOpen = true;
        job->encW = w;
        job->encH = h;
    } else if (w != job->encW || h != job->encH) {
        videoFinish("frame size changed mid-recording");
        return false;
    }

    job->scratch.resize(static_cast<size_t>(w) * h * 4);
    const int srcRow = static_cast<int>(cw) * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy(job->scratch.data() + static_cast<size_t>(y) * w * 4,
                    job->fetch.data() + static_cast<size_t>(y) * srcRow,
                    static_cast<size_t>(w) * 4);
    const std::string err = job->enc.writeFrame(job->scratch.data());
    if (!err.empty()) {
        videoFinish(QString::fromStdString(err));
        return false;
    }
    job->haveFrame = true;
    ++job->framesEncoded;
    job->progress->setValue(std::min(job->framesEncoded, job->estTotal));
    return true;
}

void MainWindow::videoFinish(const QString& message) {
    VideoJob* job = videoJob_;
    if (!job) return;
    videoJob_ = nullptr;

    QString codec;
    if (job->encOpen) {
        job->enc.finish();
        codec = job->enc.codecUsed();
    }
    QObject::disconnect(job->frameConn);
    if (vk::Renderer* r = viewport_->renderer()) {
        // The last frames of the show may still be in the ring: best-effort
        // tail drain, no recursion into this function on error.
        while (message.isEmpty() && job->encOpen &&
               r->pendingCaptures() > 0 && r->waitCapture(UINT64_MAX)) {
            uint32_t cw = 0, ch = 0;
            if (!r->fetchCapture(job->fetch, cw, ch)) break;
            const int w = static_cast<int>(cw) & ~1;
            const int h = static_cast<int>(ch) & ~1;
            if (w != job->encW || h != job->encH) break;
            job->scratch.resize(static_cast<size_t>(w) * h * 4);
            const int srcRow = static_cast<int>(cw) * 4;
            for (int y = 0; y < h; ++y)
                std::memcpy(
                    job->scratch.data() + static_cast<size_t>(y) * w * 4,
                    job->fetch.data() + static_cast<size_t>(y) * srcRow,
                    static_cast<size_t>(w) * 4);
            if (!job->enc.writeFrame(job->scratch.data()).empty()) break;
            ++job->framesEncoded;
        }
        r->endPipelinedCapture();
        r->setCaptureExtent(0, 0);
        r->setUncappedPresent(false);
    }
    viewport_->setAspectOverride(0.0f);
    viewport_->setAnimationsPaused(false);
    job->progress->close();
    job->progress->deleteLater();

    if (message.isEmpty()) {
        statusBar()->showMessage(
            QString("Video saved: %1  ·  %2 frames  ·  %3")
                .arg(job->outPath)
                .arg(job->framesEncoded)
                .arg(codec),
            10000);
    } else {
        statusBar()->showMessage("Video: " + message, 8000);
    }
    const bool quit = job->quitWhenDone;
    delete job;
    if (quit) QTimer::singleShot(0, qApp, &QApplication::quit);
}

void MainWindow::saveShowcase() {
    QStringList parts;
    for (const ShowcaseStep& s : showcaseSteps_)
        parts << QString("%1:%2:%3").arg(s.kind, s.param).arg(s.holdSec);
    QSettings s = appSettings();
    s.setValue("showcaseSteps", parts.join(";"));
    if (showcaseLoops_) s.setValue("showcaseLoops", showcaseLoops_->value());
    if (showcaseForever_)
        s.setValue("showcaseRepeat", showcaseForever_->isChecked());

    // Custom movement keys: id:val,val,...|id:... (8 floats per key).
    QStringList paths;
    for (const auto& [id, keys] : showcasePaths_) {
        QStringList vals;
        vals.reserve(static_cast<int>(keys.size()) * 8);
        for (const auto& k : keys)
            for (const float v : {k.yaw, k.pitch, k.roll, k.distance, k.tx,
                                  k.ty, k.tz, k.explode})
                vals << QString::number(v, 'f', 4);
        paths << id + ":" + vals.join(',');
    }
    s.setValue("showcasePathData", paths.join('|'));
    s.setValue("showcasePathNextId", nextPathId_);
}

void MainWindow::loadShowcase() {
    QSettings s = appSettings();
    showcaseSteps_.clear();
    const QString packed = s.value("showcaseSteps").toString();
    for (const QString& part :
         packed.split(';', Qt::SkipEmptyParts)) {
        const QStringList f = part.split(':');
        if (f.size() != 3) continue;
        ShowcaseStep st;
        st.kind = f[0];
        st.param = f[1];
        st.holdSec = f[2].toDouble();
        showcaseSteps_.push_back(st);
    }
    // Blockers, or the widgets' change-signals fire saveShowcase() MID-LOAD
    // and overwrite the not-yet-parsed path data with an empty map.
    if (showcaseLoops_) {
        const QSignalBlocker block(showcaseLoops_);
        showcaseLoops_->setValue(s.value("showcaseLoops", 1).toInt());
    }
    if (showcaseForever_) {
        const QSignalBlocker block(showcaseForever_);
        showcaseForever_->setChecked(s.value("showcaseRepeat", false).toBool());
        showcaseLoops_->setEnabled(!showcaseForever_->isChecked());
    }

    showcasePaths_.clear();
    for (const QString& entry : s.value("showcasePathData")
                                    .toString()
                                    .split('|', Qt::SkipEmptyParts)) {
        const int colon = entry.indexOf(':');
        if (colon <= 0) continue;
        const QString id = entry.left(colon);
        const QStringList vals = entry.mid(colon + 1).split(',');
        std::vector<VulkanWindow::PathKey> keys;
        for (int i = 0; i + 7 < vals.size(); i += 8) {
            VulkanWindow::PathKey k;
            k.yaw = vals[i].toFloat();
            k.pitch = vals[i + 1].toFloat();
            k.roll = vals[i + 2].toFloat();
            k.distance = vals[i + 3].toFloat();
            k.tx = vals[i + 4].toFloat();
            k.ty = vals[i + 5].toFloat();
            k.tz = vals[i + 6].toFloat();
            k.explode = vals[i + 7].toFloat();
            keys.push_back(k);
        }
        if (keys.size() >= 2) showcasePaths_[id] = std::move(keys);
    }
    nextPathId_ = s.value("showcasePathNextId", 1).toInt();
    refreshShowcaseList();
}

// Tick off any stackup row whose part was hidden from the environment, so the
// panel matches the render.
void MainWindow::syncStackupChecks(QTreeWidgetItem* item, const QStringList& hidden) {
    if (!item) return;
    const QString name = item->data(0, Qt::UserRole).toString();
    for (const QString& h : hidden)
        if (!name.isEmpty() && name == h.trimmed())
            item->setCheckState(0, Qt::Unchecked);
    for (int i = 0; i < item->childCount(); ++i)
        syncStackupChecks(item->child(i), hidden);
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
    // A measurement pinned BEFORE the inference now has nets to resolve
    // against; without this its routed readout stayed dark.
    viewport_->refreshMeasurementNets();
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
    // Clear the WHOLE selection, not just the primary: net ids belong to the
    // board that just went away, and a stale vector here painted the wrong
    // nets on the next board once boardUploaded started re-applying it.
    highlightedNets_.clear();
    if (viewport_->renderer()) viewport_->renderer()->setHighlightNet(-1);

    // Say WHY the list is empty rather than showing a blank panel that looks
    // like a failure -- but say it accurately. Gerbers absolutely can carry
    // nets (X2 %TO.N% object attributes, which pcbview reads); this particular
    // package simply has none, which for a KiCad export means it was written
    // without "Include advanced X2 features".
    // Offer inference when there is no netlist to overwrite -- or when the
    // nets are 356 test-point names that still need binding to the copper.
    // Say what the list is showing either way.
    if (inferNetsBtn_)
        inferNetsBtn_->setVisible(loaded_ &&
                                  (mesh_.nets.empty() ||
                                   (baseArt_.netsFromTestPoints &&
                                    !baseArt_.netsArePseudo)));
    if (pseudoNetNote_) {
        pseudoNetNote_->setText(
            baseArt_.netsFromTestPoints
                ? (baseArt_.netsArePseudo
                       ? "Net names from the IPC-D-356 netlist; connectivity "
                         "derived from copper geometry."
                       : "Net names from the IPC-D-356 netlist — run Infer "
                         "nets to bind them to the copper.")
                : "Derived from copper geometry — not a netlist. Names are "
                  "arbitrary.");
        pseudoNetNote_->setVisible(
            !mesh_.nets.empty() &&
            (baseArt_.netsArePseudo || baseArt_.netsFromTestPoints));
    }

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

    if (clearNetsBtn_) clearNetsBtn_->setVisible(!highlightedNets_.empty());

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

    // Import report indicator, far left so it reads first. A PERSISTENT
    // control rather than a dialog on load: a popup you dismiss is gone, and
    // on the common notes-only import a modal would be exactly the crying-wolf
    // problem the notes/warnings split exists to avoid. This waits to be
    // asked, and is still there an hour later.
    // ClickLabel is file-local, so the callback is set here and the member
    // holds it as a plain QLabel*.
    auto* report = new ClickLabel;
    report->onClick = [this] { showImportWarnings(); };
    report->setCursor(Qt::PointingHandCursor);
    report->setVisible(false);
    statusReport_ = report;

    statusBar()->addWidget(statusReport_);
    statusBar()->addWidget(statusFile_);
    statusBar()->addWidget(new QLabel(" · "));
    statusBar()->addWidget(statusBoard_);
    statusBar()->addWidget(new QLabel(" · "));
    statusBar()->addWidget(statusChecks_);
    statusBar()->addPermanentWidget(statusPerf_);
}

// Reflect the last import in the status bar. Colour carries the severity so
// the difference is visible without reading: amber only when something might
// actually make the render disagree with the board.
void MainWindow::updateImportReportBadge() {
    if (!statusReport_) return;
    const int w = importWarnings_.size();
    const int n = importNotes_.size();
    if (w == 0 && n == 0) {
        statusReport_->setVisible(false);
        return;
    }
    QString text;
    if (w > 0) {
        text = QString("⚠ %1 warning%2").arg(w).arg(w == 1 ? "" : "s");
        if (n > 0) text += QString(", %1 note%2").arg(n).arg(n == 1 ? "" : "s");
        statusReport_->setStyleSheet("color:#E0A030; padding:0 6px;");
    } else {
        text = QString("%1 note%2").arg(n).arg(n == 1 ? "" : "s");
        statusReport_->setStyleSheet(
            QString("color:%1; padding:0 6px;").arg(theme::kTextDim));
    }
    statusReport_->setText(text + "  ·");
    statusReport_->setToolTip("Click for the full import report");
    statusReport_->setVisible(true);
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
    // Remember it PER MODE (see applyInternalResForMode): a scale dialled in for
    // path tracing should not follow you back to plain raster, and vice versa.
    appSettings().setValue(internalResKey(), scale);
    viewport_->requestUpdate();
}

// Which persisted scale governs what is on screen right now. On the software
// (CPU) device the traced modes and plain raster keep SEPARATE scales; a GPU
// keeps one.
QString MainWindow::internalResKey() const {
    if (!viewport_ || !viewport_->cpuRender()) return "gpuRenderScale";
    const bool tracing = viewport_->pathTracing() || viewport_->rayTracing();
    return tracing ? "cpuTraceScale" : "cpuRasterScale";
}

// Point the internal-resolution slider (and the renderer) at the scale for the
// current device AND render mode. Called whenever any of those change, so the
// slider always reflects -- and controls -- what is actually on screen.
//
// The software renderer is ray-bound in the traced modes, so those default to
// HALF internal resolution (~4x fewer rays, upscaled to the window while the UI
// stays native) -- a big speedup. Plain raster defaults to NATIVE: a reduced
// scale there just buys an expensive linear upscale blit on llvmpipe for almost
// no rasterisation saved, so it would only make raster slower. Both remain
// fully adjustable with the slider and are remembered separately; a GPU keeps a
// single native default.
void MainWindow::applyInternalResForMode() {
    if (!viewport_->renderer()) return;
    // A headless PCBVIEW_RENDER_SCALE override is applied before the first frame
    // -- never fight it, just reflect it.
    if (qEnvironmentVariableIsSet("PCBVIEW_RENDER_SCALE")) {
        syncInternalResUi(viewport_->renderer()->renderScale());
        return;
    }
    const bool tracing = viewport_->cpuRender() &&
                         (viewport_->pathTracing() || viewport_->rayTracing());
    const float def = tracing ? 0.5f : 1.0f;  // raster and GPU default native
    const float s = appSettings().value(internalResKey(), def).toFloat();
    viewport_->renderer()->setRenderScale(s);  // early-returns if unchanged
    syncInternalResUi(s);
}

// Reflect a scale onto the slider + label WITHOUT re-entering the change slot
// (which would persist and request a redraw).
void MainWindow::syncInternalResUi(float scale) {
    if (scaleSlider_) {
        const QSignalBlocker block(scaleSlider_);
        scaleSlider_->setValue(static_cast<int>(std::lround(scale * 100.0f)));
    }
    if (scaleLabel_) scaleLabel_->setText(QString::number(scale, 'f', 2) + "×");
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

    // Opacity a fully peeled slab fades TO in the exploded view. Separate from
    // the at-rest opacity above: at rest the board should look like a board,
    // exploded it should read like glass so inner copper shows through.
    auto* peelOp = new QSlider(Qt::Horizontal);
    peelOp->setRange(2, 60);
    peelOp->setValue(static_cast<int>(peelAlpha_ * 100.0f + 0.5f));
    auto* peelOpVal = new QLabel(QString::number(peelOp->value()) + "%");
    auto* peelOpRow = new QHBoxLayout;
    peelOpRow->addWidget(peelOp);
    peelOpRow->addWidget(peelOpVal);
    form->addRow("Substrate opacity (exploded)", peelOpRow);

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
    connect(peelOp, &QSlider::valueChanged, this, [this, peelOpVal](int v) {
        peelOpVal->setText(QString::number(v) + "%");
        peelAlpha_ = v / 100.0f;
        appSettings().setValue("peelAlpha", peelAlpha_);
        if (viewport_->renderer()) viewport_->renderer()->setPeelAlpha(peelAlpha_);
        viewport_->requestUpdate();
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
        "<h3>pcbview 1.19.1</h3>"
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
