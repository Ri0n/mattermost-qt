#pragma once

#include <memory>
#include <vector>

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QToolButton;

namespace Mattermost {

class Backend;
class BackendPost;
class InteractiveTextEdit;

/**
 * Virtualized cross-conversation post collection used by Saved and Search.
 *
 * Collection ordering and pagination are endpoint authority only. Entries keep
 * their original channel/thread identity and never become a fake BackendChannel
 * timeline; LongListWidget only virtualizes the resulting collection order.
 */
class PostCollectionView final : public QWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Saved,
        Search,
    };

    explicit PostCollectionView(Backend& backend, Mode mode, QWidget* parent = nullptr);
    ~PostCollectionView() override;

    void activateSaved();
    void activateSearch(const QString& preferredTeamId = QString());

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
    void updateStatus();

    Backend& backend;
    Mode mode;
    CollectionList* list = nullptr;
    QLabel* statusLabel = nullptr;
    InteractiveTextEdit* searchEdit = nullptr;
    QComboBox* scopeCombo = nullptr;
    QToolButton* searchAction = nullptr;

    std::vector<std::unique_ptr<BackendPost>> posts;
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
