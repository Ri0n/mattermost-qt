#pragma once

#include <QPointer>
#include <QString>
#include <QTimer>

#include "AbstractPostSource.h"
#include "widgets/LongListWidget.h"

class QWheelEvent;

namespace Mattermost {

class Backend;
class BackendPost;
class ChatArea;
class PostWidget;

/** Mattermost post presentation layered on the generic LongListWidget. */
class ChatLogWidget : public LongListWidget
{
    Q_OBJECT
public:
    explicit ChatLogWidget(QWidget* parent = nullptr);
    ~ChatLogWidget() override;

    void configure(Backend& backend, ChatArea& chatArea);
    void setSource(AbstractPostSource* source);
    AbstractPostSource* source() const { return postSource; }

    PostWidget* findPost(const QString& postId) const;
    bool ensurePostVisible(const QString& postId,
                           Alignment alignment = Alignment::EnsureVisible);
    void highlightPost(const QString& postId);
    void refreshPost(const QString& postId);

    /**
     * Keep a semantic post target anchored while its provisional logical index
     * is replaced by authoritative source data. A zero quiet period keeps the
     * lock until a real user viewport gesture cancels it.
     */
    bool lockNavigationToPost(const QString& postId,
                              Alignment alignment = Alignment::Center,
                              int quietPeriodMs = 2000);
    void clearNavigationLock();

    /** Composer up-arrow action; no QListWidgetItem leaks through this API. */
    bool editLastOwnPost();
    void postEditFinished();

signals:
    void postEditInitiated(BackendPost& post);

protected:
    QWidget* createItemWidget(int index) override;
    void destroyItemWidget(int index, QWidget* widget) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    static AbstractPostSource::RequestReason toSourceReason(RequestReason reason);
    void reconnectSource();
    void rematerializeRange(int first, int last);
    bool restoreNavigationTarget(bool force = false);
    void touchNavigationLock();

    Backend* backend = nullptr;
    ChatArea* chatArea = nullptr;
    QPointer<AbstractPostSource> postSource;
    QPointer<PostWidget> editedPostWidget;
    QVector<QMetaObject::Connection> sourceConnections;

    QString navigationPostId;
    Alignment navigationAlignment = Alignment::Center;
    int navigationLogicalIndex = -1;
    int navigationQuietPeriodMs = 0;
    QTimer navigationLockTimer;
};

} // namespace Mattermost
