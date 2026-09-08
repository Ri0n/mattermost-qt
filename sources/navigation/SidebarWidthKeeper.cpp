#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTimer>

#include "mainwindow.h"

namespace Mattermost {
namespace {

class SidebarWidthKeeper final : public QObject
{
public:
    explicit SidebarWidthKeeper(QSplitter* sourceSplitter)
        : QObject(sourceSplitter)
        , splitter(sourceSplitter)
    {
        if (!splitter) {
            return;
        }

        splitter->installEventFilter(this);
        if (QSplitterHandle* handle = splitter->handle(1)) {
            splitterHandle = handle;
            handle->installEventFilter(this);
        }

        const QList<int> currentSizes = splitter->sizes();
        if (!currentSizes.isEmpty()) {
            preferredWidth = currentSizes.front();
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event || !splitter) {
            return QObject::eventFilter(watched, event);
        }

        if (watched == splitterHandle) {
            if (event->type() == QEvent::MouseButtonPress) {
                dragging = true;
            } else if (event->type() == QEvent::MouseButtonRelease) {
                dragging = false;
                const QList<int> currentSizes = splitter->sizes();
                if (!currentSizes.isEmpty()) {
                    preferredWidth = currentSizes.front();
                }
            }
            return QObject::eventFilter(watched, event);
        }

        if (watched == splitter && event->type() == QEvent::Resize && !dragging) {
            scheduleRestore();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void scheduleRestore()
    {
        if (restorePending || preferredWidth < 0 || !splitter) {
            return;
        }
        restorePending = true;
        QPointer<SidebarWidthKeeper> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (guard) {
                guard->restorePreferredWidth();
            }
        });
    }

    void restorePreferredWidth()
    {
        restorePending = false;
        if (!splitter || preferredWidth < 0 || splitter->count() != 2) {
            return;
        }

        QList<int> currentSizes = splitter->sizes();
        if (currentSizes.size() != 2) {
            return;
        }

        const int available = std::max(0, currentSizes.at(0) + currentSizes.at(1));
        const int leftWidth = std::min(preferredWidth, available);
        if (currentSizes.at(0) == leftWidth) {
            return;
        }

        currentSizes[0] = leftWidth;
        currentSizes[1] = std::max(0, available - leftWidth);
        splitter->setSizes(currentSizes);
    }

    QPointer<QSplitter> splitter;
    QPointer<QSplitterHandle> splitterHandle;
    int preferredWidth = -1;
    bool dragging = false;
    bool restorePending = false;
};

QSplitter* findSidebarSplitter(MainWindow& window)
{
    QWidget* header = window.findChild<QWidget*>(QStringLiteral("lefttop_frame"));
    if (!header) {
        return nullptr;
    }

    const auto splitters = window.findChildren<QSplitter*>();
    for (QSplitter* candidate : splitters) {
        if (!candidate || candidate->count() < 2) {
            continue;
        }
        QWidget* firstPane = candidate->widget(0);
        if (firstPane && (firstPane == header || firstPane->isAncestorOf(header))) {
            return candidate;
        }
    }
    return nullptr;
}

void installSidebarWidthKeeper(MainWindow& window)
{
    QSplitter* splitter = findSidebarSplitter(window);
    if (!splitter || splitter->property("sidebarWidthKeeperInstalled").toBool()) {
        return;
    }

    splitter->setProperty("sidebarWidthKeeperInstalled", true);
    new SidebarWidthKeeper(splitter);
}

class SidebarWidthBootstrap final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event && event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<MainWindow*>(watched)) {
                QPointer<MainWindow> guard(window);
                // NavigationUiController also finalizes the main-window splitter
                // hierarchy after Show. Install one event-loop turn later than
                // that bootstrap so the preferred width starts from the settled,
                // restored sidebar geometry rather than a transient layout size.
                QTimer::singleShot(0, window, [guard] {
                    if (!guard) {
                        return;
                    }
                    QTimer::singleShot(0, guard, [guard] {
                        if (guard) {
                            installSidebarWidthKeeper(*guard);
                        }
                    });
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void installSidebarWidthBootstrap()
{
    if (!qApp) {
        return;
    }
    auto* bootstrap = new SidebarWidthBootstrap(qApp);
    qApp->installEventFilter(bootstrap);
}

} // namespace
} // namespace Mattermost

Q_COREAPP_STARTUP_FUNCTION(Mattermost::installSidebarWidthBootstrap)
