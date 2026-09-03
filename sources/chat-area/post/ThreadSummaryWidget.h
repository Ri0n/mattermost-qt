#pragma once

#include <QPointer>
#include <QWidget>

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;

class ThreadSummaryWidget final : public QWidget
{
    Q_OBJECT
public:
    ThreadSummaryWidget(Backend& backend,
                        BackendChannel& channel,
                        BackendPost& rootPost,
                        QWidget* parent = nullptr);

signals:
    void clicked();

public slots:
    void refresh();

private:
    void rebuildParticipantAvatars();
    void watchUser(const BackendUser* user);

    Backend& backend;
    BackendChannel& channel;
    BackendPost& rootPost;
    class QHBoxLayout* layout = nullptr;
    class QPushButton* button = nullptr;
};

} // namespace Mattermost
