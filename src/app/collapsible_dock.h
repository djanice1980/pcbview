#pragma once

// A dock that can collapse to a thin spine on its edge, VS-style.
//
// Qt has no native auto-hide dock, so this builds it: a custom title bar with a
// pin and a hide button; hide collapses the panel to a narrow spine; hovering or
// clicking the spine reveals it; and when a panel is unpinned it auto-collapses
// once the pointer leaves it. Leave-detection is a cursor-position poll rather
// than leaveEvent(), because child widgets (the tree) eat enter/leave events and
// make leaveEvent() fire while the pointer is still inside the panel.
//
// No Q_OBJECT: it only overrides virtuals and connects signals to lambdas, so it
// needs no moc and can live header-only in the one TU that includes it.

#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

namespace pcbview::app {

class CollapsibleDock : public QDockWidget {
public:
    CollapsibleDock(const QString& title, Qt::DockWidgetArea side, QWidget* parent)
        : QDockWidget(title, parent), side_(side) {
        setFeatures(QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable);

        const bool left = side == Qt::LeftDockWidgetArea;

        // --- custom title bar: [title] .... [pin][hide] ---
        titleBar_ = new QWidget;
        auto* lay = new QHBoxLayout(titleBar_);
        lay->setContentsMargins(8, 2, 4, 2);
        lay->setSpacing(2);
        auto* label = new QLabel(title);
        label->setStyleSheet("font-weight:600; letter-spacing:1px;");
        pin_ = new QToolButton;
        pin_->setCheckable(true);
        pin_->setChecked(true);
        pin_->setAutoRaise(true);
        pin_->setText("PIN");
        pin_->setToolTip("Pinned open — uncheck to auto-hide when the mouse leaves");
        auto* hide = new QToolButton;
        hide->setAutoRaise(true);
        hide->setText(left ? "‹" : "›");  // ‹ / ›
        hide->setToolTip("Hide panel");
        lay->addWidget(label);
        lay->addStretch(1);
        lay->addWidget(pin_);
        lay->addWidget(hide);

        // A near-empty title bar for the collapsed state, so only the spine shows.
        emptyTitle_ = new QWidget;
        emptyTitle_->setFixedHeight(1);

        // --- the spine shown when collapsed ---
        spine_ = new QToolButton;
        spine_->setAutoRaise(true);
        spine_->setText(left ? "›" : "‹");  // › / ‹  (points inward)
        spine_->setToolTip("Show " + title);
        spine_->setFixedWidth(kSpine);
        spine_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        spine_->installEventFilter(this);  // hover reveals

        // --- stacked content: page 0 real content, page 1 spine ---
        stack_ = new QStackedWidget;
        stack_->addWidget(new QWidget);  // placeholder until setContent()
        stack_->addWidget(spine_);
        setWidget(stack_);
        setTitleBarWidget(titleBar_);

        hoverTimer_.setInterval(200);

        // Hiding is what ENTERS auto-hide mode: collapse to the spine AND unpin,
        // so a subsequent hover-reveal is temporary (it re-hides when the mouse
        // leaves) until the user explicitly pins it open again.
        connect(hide, &QToolButton::clicked, this, [this] {
            collapse();               // sets collapsed_ = true, stops the timer
            pin_->setChecked(false);  // no-op timer restart while collapsed
        });
        connect(spine_, &QToolButton::clicked, this, [this] { expand(); });
        connect(pin_, &QToolButton::toggled, this, [this](bool on) {
            if (on) {
                hoverTimer_.stop();
                expand();  // pinning re-reveals
            } else if (!collapsed_) {
                hoverTimer_.start();  // begin watching for the pointer to leave
            }
        });
        connect(&hoverTimer_, &QTimer::timeout, this, [this] {
            if (pin_->isChecked() || collapsed_) {
                hoverTimer_.stop();
                return;
            }
            // Collapse once the cursor is genuinely outside the whole panel --
            // title bar and content both -- not merely off one child widget. The
            // dock is a child window, so build its rect in GLOBAL coords (where
            // QCursor::pos() lives) rather than using the parent-relative geometry.
            const QRect global(mapToGlobal(QPoint(0, 0)), size());
            if (!global.contains(QCursor::pos())) collapse();
        });
    }

    // Install the real panel content (the tree) as page 0.
    void setContent(QWidget* w) {
        QWidget* old = stack_->widget(0);
        stack_->insertWidget(0, w);
        stack_->removeWidget(old);
        old->deleteLater();
        content_ = w;
        if (!collapsed_) stack_->setCurrentWidget(content_);
    }

    void collapse() {
        if (collapsed_) return;
        collapsed_ = true;
        hoverTimer_.stop();
        expandedWidth_ = width();
        setTitleBarWidget(emptyTitle_);
        stack_->setCurrentWidget(spine_);
        setFixedWidth(kSpine);
    }

    void expand() {
        if (!collapsed_) return;
        collapsed_ = false;
        setMinimumWidth(kMinWidth);
        setMaximumWidth(QWIDGETSIZE_MAX);
        resize(expandedWidth_ > kMinWidth ? expandedWidth_ : kExpanded, height());
        setTitleBarWidget(titleBar_);
        stack_->setCurrentWidget(content_ ? content_ : stack_->widget(0));
        if (!pin_->isChecked()) hoverTimer_.start();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == spine_ && e->type() == QEvent::Enter) {
            expand();
            return false;
        }
        return QDockWidget::eventFilter(o, e);
    }

private:
    static constexpr int kSpine = 26;
    static constexpr int kMinWidth = 200;
    static constexpr int kExpanded = 230;

    Qt::DockWidgetArea side_;
    QStackedWidget* stack_ = nullptr;
    QWidget* content_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QWidget* emptyTitle_ = nullptr;
    QToolButton* spine_ = nullptr;
    QToolButton* pin_ = nullptr;
    bool collapsed_ = false;
    int expandedWidth_ = kExpanded;
    QTimer hoverTimer_;
};

}  // namespace pcbview::app
