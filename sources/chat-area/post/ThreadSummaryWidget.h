#pragma once

#include <QWidget>

class QHBoxLayout;
class QPushButton;

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

    QPushButton* buttonWidget() const { return button; }

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
    QHBoxLayout* layout = nullptr;
    QPushButton* button = nullptr;
};

} // namespace Mattermost
