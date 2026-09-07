#pragma once

#include <memory>
#include <vector>

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include "backend/HTTPConnector.h"

class QAbstractButton;
class QComboBox;
class QLabel;
class QToolButton;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class InteractiveTextEdit;
class ThemeIconButton;

/**
 * Virtualized post collection used by Saved, Search, and an in-channel Pinned view.
 *
 * Saved/Search entries own endpoint snapshots and keep their original
 * channel/thread identity without becoming a fake BackendChannel timeline.
 * Pinned mode borrows the current channel's authoritative pinned-post objects;
 * LongListWidget only virtualizes collection presentation in all modes.
 */
class PostCollectionView final : public QWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Saved,
        Search,
        Pinned,
    };

    explicit PostCollectionView(Backend& backend, Mode mode, QWidget* parent = nullptr);
    ~PostCollectionView() override;

    void activateSaved();
    void activateSearch(const QString& preferredTeamId = QString());
    void activatePinned(BackendChannel& channel);

    // Feed the logged-in user's realtime flagged_post preference changes into
    // an already open Saved collection. Other collection modes ignore them.
    void syncFlaggedPost(const QString& postId, bool flagged)
    {
        handleFlaggedPostChanged(postId, flagged);
    }

signals:
    void postActivated(const QString& postId);

private:
    class CollectionList;

    void buildUi();
    void rebuildSearchScopes(const QString& preferredTeamId);
    void configureSearchCompletions();
    void insertSearchToken(const QString& token);
    void startSearch();
    void resetCollection();
    void loadNextPage();
    void appendPosts(const QVector<QJsonObject>& rawPosts);
    bool appendBufferedPage();
    bool hasMoreResults() const;
    QWidget* createRow(int index, QWidget* parent);
    int indexOfPost(const QString& postId) const;
    QString originLabel(const BackendPost& post) const;
    void removeSavedPost(const QString& postId);
    void removeSavedPostLocally(const QString& postId);
    void handleFlaggedPostChanged(const QString& postId, bool flagged);
    void unpinPost(const QString& postId, QAbstractButton* button);
    void updateStatus();

    Backend& backend;
    Mode mode;
    CollectionList* list = nullptr;
    QLabel* statusLabel = nullptr;
    InteractiveTextEdit* searchEdit = nullptr;
    QComboBox* scopeCombo = nullptr;
    QToolButton* searchAction = nullptr;
    BackendChannel* pinnedChannel = nullptr;
    HTTPConnector actionConnector;

    QLabel* _titleLabel = nullptr;
    ThemeIconButton* _refreshButton = nullptr;

    std::vector<std::unique_ptr<BackendPost>> ownedPosts;
    std::vector<BackendPost*> posts;
    QSet<QString> postIds;

    // A database-backed Mattermost search can ignore page/per_page and return
    // a large bounded snapshot. Keep that snapshot off-screen and reveal only
    // one normal collection page per explicit user scroll.
    QVector<QJsonObject> bufferedPosts;
    int bufferedOffset = 0;

    QString activeTerms;
    QString activeTeamId;
    quint64 generation = 0;
    int nextPage = 0;
    bool loading = false;
    bool serverHasMore = false;

    static constexpr int PageSize = 10;
};

} // namespace Mattermost
