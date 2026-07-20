#pragma once

#include <QLabel>
#include <QPushButton>
#include <QMainWindow>
#include <QStackedWidget>
#include <QStringList>
#include <QTreeWidget>

#include <array>
#include <functional>
#include <memory>

class QDockWidget;
class QImage;
class QLineEdit;
class QSlider;

#include "app/vulkan_window.h"
#include "geom/tessellate.h"
#include "model/board.h"

namespace pcbview::app {

// "Pro CAD" shell: menu bar, icon toolbar, stackup tree on the left, properties
// on the right, coordinate/validation status along the bottom. Dense on purpose
// -- every control visible without hunting.
//
// Owns the board and mesh by value. It used to hold const references to data
// owned by main(), which made File->Open impossible: there was nothing to load
// into.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& path = {});

    // Import + tessellate + upload. Returns false and shows a dialog on failure,
    // leaving any previously loaded board untouched.
    bool loadBoard(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private slots:
    void onLayerToggled(QTreeWidgetItem* item, int column);
    void onLayerSelected();
    void onRenderScaleChanged(int sliderValue);
    void onFrameRendered();
    void onOpen();
    void onOpenFolder();
    void onReload();

private:
    void buildMenus();

    // Grab the next rendered frame as a QImage (async: the capture lands one
    // frame later), then invoke `then`. Used by screenshot and print.
    // `exportScale` > 0 renders the scene at exactly that multiple of the
    // window and grabs the offscreen target instead of the swapchain -- the
    // export path, independent of where the internal-res slider sits.
    // Progressive modes converge first and the scale is restored afterwards.
    // 0 (the default, used by print) leaves the renderer alone and grabs the
    // window as presented.
    void grabFrame(std::function<void(const QImage&)> then,
                   float exportScale = 0.0f);

    // Net highlighting. `net` indexes mesh_.nets; -1 clears. Drives the
    // renderer, the net list selection and the status readout together, so
    // every entry point (list click, board click, headless hook) agrees.
    void highlightNet(int net);
    // Ctrl+click adds to (or removes from) the selection instead of replacing
    // it, so several nets can be followed at once in different colours.
    void toggleHighlightNet(int net);
    void applyNetHighlights();
    std::vector<int> highlightedNets_;
    void buildNetDock();
    void populateNets();
    void onSaveScreenshot();
    // mode: 0 = as shown on screen, 1 = flat overhead (top orthographic),
    // 2 = flat overhead printed at the board's true physical size (1:1).
    void printView(int mode);
    void sendToPrinter(const QImage& img, bool originalSize, double mmPerPixel);

    void buildToolbar();
    void buildStackupDock();
    void buildPropertiesDock();
    void buildStatusBar();
    void showAbout();
    void showAppearanceDialog();

    void populateStackup();
    void populateProperties();
    void updateStatus();
    void rememberRecent(const QString& path);
    void rebuildRecentMenu();

    geom::TessellateOptions tessellateOptions() const;
    // Clip silkscreen off exposed copper. Persisted; changes geometry.
    bool subtractMaskFromSilk_ = false;
    void reassemble();  // rebuild mesh_ from baseArt_ + current thickness override
    // Derive nets from copper connectivity for a package with no netlist.
    void inferNetsFromCopper();
    // Every warning from the last import. The status line shows only the
    // first, so this is the only way to see the rest.
    void showImportWarnings();
    void syncStackupChecks(QTreeWidgetItem* item, const QStringList& hidden);
    QStringList importWarnings_;
    QStringList importNotes_;
    // Persistent status-bar indicator for the last import; click opens the
    // report. Lives in the status bar rather than being a load-time dialog.
    QLabel* statusReport_ = nullptr;
    void updateImportReportBadge();

    // Append the cached component parts onto mesh_ (fresh copies, so repeated
    // reassembles don't accumulate), shifting top-mounted parts to sit on the
    // current surface when a thickness override moves it. Also grows the bounds.
    void appendComponents();

    // Owned, so a reload can replace them wholesale. `board_` is only populated
    // for a .kicad_pcb; `fromGerber_` gates the semantics-dependent UI
    // (validation, pad counts). `baseArt_` is the untouched stackup for BOTH
    // paths -- a thickness override re-derives from it.
    BoardModel board_;
    geom::LayerArt baseArt_;
    geom::BoardMesh mesh_;
    // Component bodies (Material::Component), in world space at the design
    // thickness. Sourced once per load via kicad-cli; re-appended to mesh_ on
    // every reassemble so a thickness change keeps them. Empty on the Gerber
    // path -- gerbers carry no component identity.
    std::vector<geom::Part> componentParts_;
    QString path_;
    bool loaded_ = false;
    bool fromGerber_ = false;

    double thicknessOverride_ = 0.0;  // 0 = use the design's own thickness

    // Substrate appearance lives here, not only in the renderer, because the
    // renderer does not exist until first expose. Pushed to it on boardUploaded.
    std::array<float, 3> subColor_ = {0.72f, 0.61f, 0.38f};
    float subOpacity_ = 1.0f;
    std::array<float, 3> maskColor_ = {0.010f, 0.246f, 0.025f};
    float maskOpacity_ = 0.72f;
    // Effects menu (stylised, persisted; 0-100 slider values).
    int fxComponentShine_ = 0;
    int fxPadShine_ = 94;
    int fxShadowSoftness_ = 15;  // sun radius = v% of 8 deg; 15 = 1.2 deg
    int fxNetGlow_ = 20;         // net highlight emission; v * 0.16 = strength
    void applyAppearance();  // push colours/opacity if the renderer exists

    // Create viewport_ + its container and wire every viewport signal. Called at
    // construction and again by rebuildViewport().
    void buildViewport();
    // Tear down and recreate the viewport window + container. Required when the
    // graphics device switches between a hardware GPU and the software CPU
    // driver: Windows cannot re-associate an existing native window with a new
    // presenting driver (the screen freezes on the last pre-switch frame), so
    // the native window itself must be replaced. Camera and peel carry over;
    // everything else re-loads from the persisted settings.
    void rebuildViewport();

    VulkanWindow* viewport_ = nullptr;
    QWidget* viewportContainer_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* placeholder_ = nullptr;
    QTreeWidget* stackup_ = nullptr;
    QTreeWidget* properties_ = nullptr;
    QDockWidget* stackupDock_ = nullptr;
    QDockWidget* propertiesDock_ = nullptr;
    QDockWidget* netDock_ = nullptr;
    QTreeWidget* netList_ = nullptr;
    QLineEdit* netFilter_ = nullptr;
    QPushButton* inferNetsBtn_ = nullptr;
    QLabel* pseudoNetNote_ = nullptr;
    int highlightedNet_ = -1;
    QMenu* recentMenu_ = nullptr;
    QAction* measureAction_ = nullptr;
    QAction* dimsAction_ = nullptr;
    // One action shared by the View menu and the toolbar; the O key syncs it
    // through VulkanWindow::orthoChanged. Two separate checkable actions for
    // one piece of camera state drift apart the moment either is used.
    QAction* orthoAction_ = nullptr;

    QLabel* statusFile_ = nullptr;
    QLabel* statusBoard_ = nullptr;
    QLabel* statusChecks_ = nullptr;
    QLabel* statusPerf_ = nullptr;
    QLabel* toolbarInfo_ = nullptr;
    QLabel* scaleLabel_ = nullptr;
    QSlider* scaleSlider_ = nullptr;
    // Set by rebuildViewport (device switch) to the scale the dying renderer
    // had; the new renderer picks it up once it exists. Negative = nothing
    // pending, which is what keeps a normal board load from clobbering the
    // PCBVIEW_RENDER_SCALE override.
    float pendingRenderScale_ = -1.0f;
    QLabel* explodeLabel_ = nullptr;

    int frameCounter_ = 0;
};

}  // namespace pcbview::app
