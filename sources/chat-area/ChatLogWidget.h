#pragma once

#include <QPointer>
#include <QString>

#include "AbstractPostSource.h"
#include "widgets/LongListWidget.h"

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
     * is replaced by authoritative source data. ChatLogWidget owns only post
     * identity; LongListWidget owns the actual viewport lock and all scroll math.
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

private:
    static AbstractPostSource::RequestReason toSourceReason(RequestReason reason);
    void reconnectSource();
    void rematerializeRange(int first, int last);
    bool restoreNavigationTarget();

    Backend* backend = nullptr;
    ChatArea* chatArea = nullptr;
    QPointer<AbstractPostSource> postSource;
    QPointer<PostWidget> editedPostWidget;
    QVector<QMetaObject::Connection> sourceConnections;

    QString navigationPostId;
    int navigationLogicalIndex = -1;
};

} // namespace Mattermost
