#pragma once

#include <cstdint>

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>

#include "AbstractPostSource.h"
#include "ManualUnreadVisibilityGate.h"
#include "PostListWidget.h"

class QFrame;
class QLabel;
class QPushButton;
class QResizeEvent;

namespace Mattermost {

class Backend;
class BackendPost;
class ChatArea;
class PostWidget;

/** Mattermost timeline behavior layered on the shared post-list presentation. */
class ChatLogWidget : public PostListWidget
{
    Q_OBJECT
public:
    explicit ChatLogWidget(QWidget* parent = nullptr);
    ~ChatLogWidget() override;

    void configure(Backend& backend, ChatArea& chatArea);
    void setSource(AbstractPostSource* source);
    AbstractPostSource* source() const { return postSource; }

    PostWidget* findPost(const QString& postId) const;

    /** Capture a semantic post near the viewport centre for inactive-page restore. */
    bool captureViewportBookmark(QString& postId) const;

    /** Restore an inactive-page bookmark without the navigation highlight animation. */
    bool restoreViewportBookmark(const QString& postId);

    bool ensurePostVisible(const QString& postId,
                           Alignment alignment = Alignment::EnsureVisible);
    void highlightPost(const QString& postId);
    void refreshPost(const QString& postId);

    /** Keep the locally confirmed outgoing post at the live edge of the log. */
    void followOwnPost(const QString& postId);

    /**
     * Re-evaluate read progress from the current viewport. This never treats the
     * caller/navigation action itself as a read; the lower edge of a concrete
     * post must actually be inside the active viewport.
     */
    void refreshReadState();

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

    bool isMessageSelectionMode() const { return messageSelectionMode_; }
    void beginMessageSelectionDrag(const QString& anchorPostId, const QString& currentPostId);
    void updateMessageSelectionDrag(const QString& currentPostId);
    void finishMessageSelectionDrag();
    void cancelMessageSelection();

signals:
    void postEditInitiated(BackendPost& post);

protected:
    QWidget* createItemWidget(int index) override;
    QString itemIdentity(const QWidget* widget) const override;
    int indexOfItemIdentity(const QString& identity) const override;
    bool isModelItemAvailable(int index) const override;
    void destroyItemWidget(int index, QWidget* widget) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    static AbstractPostSource::RequestReason toSourceReason(RequestReason reason);
    void reconnectSource();
    bool restoreNavigationTarget();
    bool finalizeNavigationLock();
    void scheduleNavigationFinalize();
    void scheduleReadCursorUpdate();
    void updateReadCursorFromViewport();
    void markPostUnread(const QString& postId);
    bool isPostLowerEdgeVisible(const QString& postId) const;
    void setMessageSelectionRange(const QString& currentPostId);
    void setMessagePostSelected(const QString& postId, bool selected);
    void applyMessageSelectionVisuals();
    void cacheSelectedPost(const QString& postId);
    void ensureSelectionToolbar();
    void updateSelectionToolbar();
    void positionSelectionToolbar();
    void copySelectedPosts();
    void deleteSelectedOwnPosts();

    Backend* backend = nullptr;
    ChatArea* chatArea = nullptr;
    QPointer<AbstractPostSource> postSource;
    QPointer<PostWidget> editedPostWidget;
    QVector<QMetaObject::Connection> sourceConnections;

    QString navigationPostId;
    QString pendingHighlightPostId;
    int navigationLogicalIndex = -1;
    Alignment navigationAlignment = Alignment::Center;
    int navigationQuietPeriodMs = 2000;
    bool navigationLockPending = false;
    bool navigationRecenterPending = false;
    bool readCursorUpdatePending_ = false;
    bool _initialScrollBarPulsePending = true;
    ManualUnreadVisibilityGate manualUnreadGate_;
    QString manualUnreadHighWaterPostId_;
    uint64_t manualUnreadHighWaterCreateAt_ = 0;
    bool manualUnreadExitedViewport_ = false;

    bool messageSelectionMode_ = false;
    bool messageSelectionDragActive_ = false;
    QString messageSelectionAnchorPostId_;
    QSet<QString> selectedPostIds_;
    QSet<QString> selectedOwnPostIds_;
    QHash<QString, QString> selectedFormattedPosts_;
    QFrame* selectionToolbar_ = nullptr;
    QLabel* selectionCountLabel_ = nullptr;
    QPushButton* selectionDeleteButton_ = nullptr;
    QPushButton* selectionCopyButton_ = nullptr;
};

} // namespace Mattermost
