#pragma once

#include <memory>
#include <vector>

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

namespace Mattermost {

class Backend;
class BackendPost;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

/**
 * Virtualized cross-conversation post collection used by Saved and Search.
 *
 * Collection ordering is endpoint authority only. Entries keep their original
 * channel/thread identity and never become a fake BackendChannel timeline.
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
    void insertSearchToken(const QString& token);
    void startSearch();
    void resetCollection();
    void loadNextPage();
    void finishPendingRangeRequests();
    QWidget* createRow(int index, QWidget* parent);
    int indexOfPost(const QString& postId) const;
    QString originLabel(const BackendPost& post) const;
    void removeSavedPost(const QString& postId);
    void updateStatus();

    Backend& backend;
    Mode mode;
    CollectionList* list = nullptr;
    QLabel* statusLabel = nullptr;
    QLineEdit* searchEdit = nullptr;
    QComboBox* scopeCombo = nullptr;
    QToolButton* searchAction = nullptr;

    std::vector<std::unique_ptr<BackendPost>> posts;
    QSet<QString> postIds;
    QVector<QPair<int, int>> pendingRangeRequests;

    QString activeTerms;
    QString activeTeamId;
    quint64 generation = 0;
    int nextPage = 0;
    bool loading = false;
    bool hasMore = false;

    static constexpr int PageSize = 50;
};

} // namespace Mattermost
