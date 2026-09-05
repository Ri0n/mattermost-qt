#pragma once

#include <QWidget>

class QEvent;
class QHBoxLayout;
class QLabel;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void rebuildParticipantAvatars();
    void watchUser(const BackendUser* user);
    void refreshTheme();

    Backend& backend;
    BackendChannel& channel;
    BackendPost& rootPost;
    QHBoxLayout* layout = nullptr;
    QWidget* chip = nullptr;
    QLabel* chipIcon = nullptr;
    QLabel* chipCount = nullptr;
};

} // namespace Mattermost
