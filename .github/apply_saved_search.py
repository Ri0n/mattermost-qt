from pathlib import Path
import re


def write(path, content):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:120]!r}")
    p.write_text(text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# Shared collection view.
# ---------------------------------------------------------------------------
write("sources/post-collection/PostCollectionView.h", r'''#pragma once

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
''')

write("sources/post-collection/PostCollectionView.cpp", r'''#include "PostCollectionView.h"

#include <algorithm>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/post/PostWidget.h"
#include "navigation/AppNavigationService.h"
#include "widgets/LongListWidget.h"

namespace Mattermost {

class PostCollectionView::CollectionList final : public LongListWidget
{
public:
    explicit CollectionList(PostCollectionView& owner, QWidget* parent)
        : LongListWidget(parent)
        , owner(owner)
    {
        setDefaultItemHeight(132);
        setMaterializationLimit(200);
        setRequestBlockSize(20);
        setPrefetchScreens(1);
        setSeekDebounceMs(100);

        connect(this, &LongListWidget::rangeRequested, this,
                [this](int first, int last, RequestReason, quint64) {
            if (last < static_cast<int>(owner.posts.size()) || !owner.hasMore) {
                finishRangeRequest(first, last);
                return;
            }
            owner.pendingRangeRequests.push_back(qMakePair(first, last));
            owner.loadNextPage();
        });

        connect(this, &LongListWidget::visibleRangeChanged, this,
                [this](int, int last) {
            if (owner.hasMore && !owner.loading
                && last >= std::max(0, static_cast<int>(owner.posts.size()) - 5)) {
                owner.loadNextPage();
            }
        });
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return owner.createRow(index, viewport());
    }

private:
    PostCollectionView& owner;
};

PostCollectionView::PostCollectionView(Backend& backendInstance, Mode viewMode, QWidget* parent)
    : QWidget(parent)
    , backend(backendInstance)
    , mode(viewMode)
{
    buildUi();
}

PostCollectionView::~PostCollectionView()
{
    if (list) {
        // Destroy PostWidgets (and their BackendPost references) before the
        // collection-owned BackendPost instances themselves are released.
        list->setItemCount(0);
    }
}

void PostCollectionView::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(mode == Mode::Saved ? tr("Saved messages")
                                                 : tr("Search messages"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch();

    if (mode == Mode::Saved) {
        auto* refresh = new QToolButton(this);
        refresh->setText(tr("Refresh"));
        refresh->setAutoRaise(true);
        connect(refresh, &QToolButton::clicked, this, &PostCollectionView::activateSaved);
        header->addWidget(refresh);
    }
    root->addLayout(header);

    if (mode == Mode::Search) {
        auto* searchRow = new QHBoxLayout;
        scopeCombo = new QComboBox(this);
        scopeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        searchRow->addWidget(scopeCombo);

        searchEdit = new QLineEdit(this);
        searchEdit->setClearButtonEnabled(true);
        searchEdit->setPlaceholderText(tr("Search messages…"));
        searchRow->addWidget(searchEdit, 1);

        searchAction = new QToolButton(this);
        searchAction->setText(tr("Search"));
        searchAction->setToolButtonStyle(Qt::ToolButtonTextOnly);
        searchRow->addWidget(searchAction);
        root->addLayout(searchRow);

        auto* modifiers = new QHBoxLayout;
        auto addModifier = [this, modifiers](const QString& label, const QString& token) {
            auto* button = new QToolButton(this);
            button->setText(label);
            button->setAutoRaise(true);
            connect(button, &QToolButton::clicked, this,
                    [this, token] { insertSearchToken(token); });
            modifiers->addWidget(button);
        };
        addModifier(QStringLiteral("from:"), QStringLiteral("from:"));
        addModifier(QStringLiteral("in:"), QStringLiteral("in:"));
        addModifier(QStringLiteral("before:"), QStringLiteral("before:"));
        addModifier(QStringLiteral("after:"), QStringLiteral("after:"));
        addModifier(QStringLiteral("on:"), QStringLiteral("on:"));
        modifiers->addStretch();
        root->addLayout(modifiers);

        auto* syntax = new QLabel(
            tr("Syntax: from:user  in:channel  before:YYYY-MM-DD  after:YYYY-MM-DD  "
               "on:YYYY-MM-DD  \"exact phrase\"  -exclude  word*  #hashtag"), this);
        syntax->setWordWrap(true);
        syntax->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QPalette mutedPalette = syntax->palette();
        mutedPalette.setColor(QPalette::WindowText,
                              mutedPalette.color(QPalette::Disabled, QPalette::Text));
        syntax->setPalette(mutedPalette);
        root->addWidget(syntax);

        connect(searchEdit, &QLineEdit::returnPressed,
                this, &PostCollectionView::startSearch);
        connect(searchAction, &QToolButton::clicked,
                this, &PostCollectionView::startSearch);
    }

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    root->addWidget(statusLabel);

    list = new CollectionList(*this, this);
    root->addWidget(list, 1);

    updateStatus();
}

void PostCollectionView::activateSaved()
{
    if (mode != Mode::Saved) {
        return;
    }
    ++generation;
    activeTerms.clear();
    activeTeamId.clear();
    resetCollection();
    loadNextPage();
}

void PostCollectionView::activateSearch(const QString& preferredTeamId)
{
    if (mode != Mode::Search) {
        return;
    }
    rebuildSearchScopes(preferredTeamId);
    if (searchEdit) {
        searchEdit->setFocus(Qt::ShortcutFocusReason);
        searchEdit->selectAll();
    }
}

void PostCollectionView::rebuildSearchScopes(const QString& preferredTeamId)
{
    if (!scopeCombo) {
        return;
    }

    const QString previous = scopeCombo->currentData().toString();
    scopeCombo->clear();
    scopeCombo->addItem(tr("All teams"), QString());

    QVector<const BackendTeam*> teams;
    teams.reserve(static_cast<int>(backend.getStorage().teams.size()));
    for (const auto& entry : backend.getStorage().teams) {
        teams.push_back(&entry.second);
    }
    std::sort(teams.begin(), teams.end(), [](const BackendTeam* lhs, const BackendTeam* rhs) {
        return QString::localeAwareCompare(lhs->display_name, rhs->display_name) < 0;
    });
    for (const BackendTeam* team : teams) {
        scopeCombo->addItem(team->display_name, team->id);
    }

    QString desired = previous;
    if (desired.isEmpty() && !preferredTeamId.isEmpty()) {
        desired = preferredTeamId;
    }
    if (!desired.isEmpty()) {
        const int index = scopeCombo->findData(desired);
        if (index >= 0) {
            scopeCombo->setCurrentIndex(index);
        }
    }
}

void PostCollectionView::insertSearchToken(const QString& token)
{
    if (!searchEdit) {
        return;
    }
    if (!searchEdit->text().isEmpty() && !searchEdit->text().endsWith(QLatin1Char(' '))) {
        searchEdit->insert(QStringLiteral(" "));
    }
    searchEdit->insert(token);
    searchEdit->setFocus(Qt::ShortcutFocusReason);
}

void PostCollectionView::startSearch()
{
    if (mode != Mode::Search || !searchEdit) {
        return;
    }

    const QString terms = searchEdit->text().trimmed();
    if (terms.isEmpty()) {
        activeTerms.clear();
        ++generation;
        resetCollection();
        updateStatus();
        return;
    }

    activeTerms = terms;
    activeTeamId = scopeCombo ? scopeCombo->currentData().toString() : QString();
    ++generation;
    resetCollection();
    loadNextPage();
}

void PostCollectionView::resetCollection()
{
    if (list) {
        list->setItemCount(0);
    }
    posts.clear();
    postIds.clear();
    pendingRangeRequests.clear();
    nextPage = 0;
    loading = false;
    hasMore = false;
    updateStatus();
}

void PostCollectionView::loadNextPage()
{
    if (loading || (nextPage > 0 && !hasMore)) {
        return;
    }
    if (mode == Mode::Search && activeTerms.isEmpty()) {
        finishPendingRangeRequests();
        return;
    }

    loading = true;
    updateStatus();
    const quint64 requestGeneration = generation;
    const int page = nextPage;
    QPointer<PostCollectionView> guard(this);

    auto callback = [guard, requestGeneration, page](const PostRepository::CollectionPage& result) {
        if (!guard || requestGeneration != guard->generation) {
            return;
        }

        guard->loading = false;
        if (!result.success) {
            guard->hasMore = false;
            guard->finishPendingRangeRequests();
            guard->updateStatus();
            return;
        }

        const int oldCount = static_cast<int>(guard->posts.size());
        for (const QJsonObject& raw : result.posts) {
            const QString postId = raw.value(QStringLiteral("id")).toString();
            if (postId.isEmpty() || guard->postIds.contains(postId)) {
                continue;
            }
            guard->postIds.insert(postId);
            guard->posts.push_back(
                std::make_unique<BackendPost>(raw, guard->backend.getStorage()));
        }

        const int newCount = static_cast<int>(guard->posts.size());
        guard->hasMore = result.hasMore && newCount > oldCount;
        guard->nextPage = page + 1;

        // A single unavailable logical sentinel advertises that more collection
        // rows exist. LongListWidget requests it before the viewport reaches the
        // end, but no fake message widget is ever created for the sentinel.
        guard->list->setItemCount(newCount + (guard->hasMore ? 1 : 0));
        for (int index = oldCount; index < newCount; ++index) {
            guard->list->setRangeAvailable(index, index, true);
        }

        guard->finishPendingRangeRequests();
        guard->updateStatus();
    };

    auto& repository = PostRepository::instance(backend);
    if (mode == Mode::Saved) {
        repository.loadFlaggedPosts(page, PageSize, std::move(callback));
    } else {
        repository.searchPosts(activeTeamId, activeTerms, page, PageSize,
                               std::move(callback));
    }
}

void PostCollectionView::finishPendingRangeRequests()
{
    if (!list) {
        pendingRangeRequests.clear();
        return;
    }
    const QVector<QPair<int, int>> pending = pendingRangeRequests;
    pendingRangeRequests.clear();
    for (const auto& range : pending) {
        list->finishRangeRequest(range.first, range.second);
    }
}

QWidget* PostCollectionView::createRow(int index, QWidget* parent)
{
    if (index < 0 || index >= static_cast<int>(posts.size()) || !posts[index]) {
        return nullptr;
    }

    BackendPost& post = *posts[index];
    const QString postId = post.id;

    auto* row = new QFrame(parent);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(4, 4, 4, 8);
    layout->setSpacing(2);

    auto* metadata = new QHBoxLayout;
    metadata->setContentsMargins(0, 0, 0, 0);
    auto* origin = new QLabel(originLabel(post), row);
    QFont originFont = origin->font();
    originFont.setBold(true);
    origin->setFont(originFont);
    metadata->addWidget(origin);
    metadata->addStretch();

    if (mode == Mode::Saved) {
        auto* remove = new QToolButton(row);
        remove->setText(tr("Remove from saved"));
        remove->setAutoRaise(true);
        connect(remove, &QToolButton::clicked, this,
                [this, postId] { removeSavedPost(postId); });
        metadata->addWidget(remove);
    }

    auto* jump = new QToolButton(row);
    jump->setText(tr("Jump"));
    jump->setToolTip(tr("Show this message in its conversation"));
    jump->setAutoRaise(true);
    connect(jump, &QToolButton::clicked, this, [this, postId] {
        AppNavigationService::instance(backend).openPost(postId);
    });
    metadata->addWidget(jump);
    layout->addLayout(metadata);

    auto* postWidget = new PostWidget(backend, post, row, nullptr, nullptr);
    layout->addWidget(postWidget);
    connect(postWidget, &PostWidget::dimensionsChanged, this, [this, postId] {
        const int currentIndex = indexOfPost(postId);
        if (currentIndex >= 0 && list) {
            list->itemsChanged(currentIndex, currentIndex);
        }
    });
    return row;
}

int PostCollectionView::indexOfPost(const QString& postId) const
{
    for (int index = 0; index < static_cast<int>(posts.size()); ++index) {
        if (posts[index] && posts[index]->id == postId) {
            return index;
        }
    }
    return -1;
}

QString PostCollectionView::originLabel(const BackendPost& post) const
{
    QString label;
    if (BackendChannel* channel = backend.getStorage().getChannelById(post.channel_id)) {
        label = channel->display_name;
        if (label.isEmpty()) {
            label = channel->name;
        }
    }
    if (label.isEmpty()) {
        label = tr("Channel %1").arg(post.channel_id.left(8));
    }
    if (!post.root_id.isEmpty()) {
        label += tr(" • thread");
    }
    return label;
}

void PostCollectionView::removeSavedPost(const QString& postId)
{
    if (mode != Mode::Saved || postId.isEmpty()) {
        return;
    }
    const int index = indexOfPost(postId);
    if (index < 0) {
        return;
    }

    BackendUserPreferences preference {
        QStringLiteral("flagged_post"), postId, QStringLiteral("true")};
    backend.deleteUserPreferences(preference);

    // removeItems destroys the materialized PostWidget before we release the
    // collection-owned BackendPost it references.
    if (list) {
        list->removeItems(index, 1);
    }
    posts.erase(posts.begin() + index);
    postIds.remove(postId);
    updateStatus();
}

void PostCollectionView::updateStatus()
{
    if (!statusLabel) {
        return;
    }
    if (loading) {
        statusLabel->setText(tr("Loading…"));
        return;
    }
    if (mode == Mode::Search && activeTerms.isEmpty()) {
        statusLabel->setText(tr("Enter search terms or use a modifier above."));
        return;
    }
    if (posts.empty()) {
        statusLabel->setText(mode == Mode::Saved
            ? tr("No saved messages.") : tr("No matching messages."));
        return;
    }
    const int count = static_cast<int>(posts.size());
    statusLabel->setText(hasMore
        ? tr("%1 messages loaded — scroll for more").arg(count)
        : tr("%1 messages").arg(count));
}

} // namespace Mattermost
''')

# ---------------------------------------------------------------------------
# PostRepository collection endpoints.
# ---------------------------------------------------------------------------
replace_once("sources/backend/PostRepository.h",
'''    struct PostResult {
        QString postId;
        QString channelId;
        QString rootId;
        bool success = false;
    };

    using PageCallback = std::function<void(const Page&)>;
    using ContextCallback = std::function<void(const Context&)>;
    using PostCallback = std::function<void(const PostResult&)>;
''',
'''    struct PostResult {
        QString postId;
        QString channelId;
        QString rootId;
        bool success = false;
    };

    /** One page of a cross-conversation endpoint, preserving server order. */
    struct CollectionPage {
        QVector<QJsonObject> posts;
        bool hasMore = false;
        bool success = false;
    };

    using PageCallback = std::function<void(const Page&)>;
    using ContextCallback = std::function<void(const Context&)>;
    using PostCallback = std::function<void(const PostResult&)>;
    using CollectionCallback = std::function<void(const CollectionPage&)>;
''')

replace_once("sources/backend/PostRepository.h",
'''    /** Fetch one post by id and quietly merge it into its known channel cache. */
    void loadPost(const QString& postId, PostCallback callback);

    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */
''',
'''    /** Fetch one post by id and quietly merge it into its known channel cache. */
    void loadPost(const QString& postId, PostCallback callback);

    /** Fetch the logged-in user's saved/flagged posts, preserving collection order. */
    void loadFlaggedPosts(int page, int perPage, CollectionCallback callback);

    /** Search message posts in one team, or all teams when teamId is empty. */
    void searchPosts(const QString& teamId,
                     const QString& terms,
                     int page,
                     int perPage,
                     CollectionCallback callback);

    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */
''')

replace_once("sources/backend/PostRepository.cpp",
'''QStringList combineContext(const QStringList& before,
                           const QString& targetId,
                           const QStringList& after)
{
''',
'''PostRepository::CollectionPage collectionPageFromDocument(
    QVariant status, const QJsonDocument& doc, int perPage)
{
    PostRepository::CollectionPage result;
    if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
        return result;
    }

    const QJsonObject root = doc.object();
    const QJsonObject objects = root.value(QStringLiteral("posts")).toObject();
    const QJsonArray order = root.value(QStringLiteral("order")).toArray();
    QSet<QString> seen;

    for (const QJsonValue& value : order) {
        const QString id = value.toString();
        const QJsonValue postValue = objects.value(id);
        if (id.isEmpty() || seen.contains(id) || !postValue.isObject()) {
            continue;
        }
        seen.insert(id);
        result.posts.push_back(postValue.toObject());
    }

    // Older/custom servers may omit order for collection-like responses. Keep
    // the payload usable, but never reinterpret this fallback as timeline order.
    if (order.isEmpty()) {
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            if (!it->isObject() || seen.contains(it.key())) {
                continue;
            }
            seen.insert(it.key());
            result.posts.push_back(it->toObject());
        }
    }

    result.hasMore = perPage > 0 && order.size() >= perPage;
    result.success = true;
    return result;
}

QStringList combineContext(const QStringList& before,
                           const QString& targetId,
                           const QStringList& after)
{
''')

replace_once("sources/backend/PostRepository.cpp",
'''void PostRepository::loadPost(const QString& postId, PostCallback callback)
{
''',
'''void PostRepository::loadFlaggedPosts(int page, int perPage, CollectionCallback callback)
{
    const int safePage = std::max(0, page);
    const int safePerPage = std::max(1, perPage);
    const QString userId = backend.getLoginUser().id;
    if (userId.isEmpty()) {
        if (callback) {
            callback(CollectionPage {});
        }
        return;
    }

    const QString path = QStringLiteral("users/") + userId
        + QStringLiteral("/posts/flagged?page=") + QString::number(safePage)
        + QStringLiteral("&per_page=") + QString::number(safePerPage);
    coalescedGet(path,
        [safePerPage, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc, const RequestContext&) mutable {
            if (callback) {
                callback(collectionPageFromDocument(status, doc, safePerPage));
            }
        });
}

void PostRepository::searchPosts(const QString& teamId,
                                 const QString& terms,
                                 int page,
                                 int perPage,
                                 CollectionCallback callback)
{
    const QString query = terms.trimmed();
    if (query.isEmpty()) {
        if (callback) {
            CollectionPage empty;
            empty.success = true;
            callback(empty);
        }
        return;
    }

    const int safePage = std::max(0, page);
    const int safePerPage = std::max(1, perPage);
    const QString path = teamId.isEmpty()
        ? QStringLiteral("posts/search")
        : QStringLiteral("teams/") + teamId + QStringLiteral("/posts/search");

    QJsonObject body {
        {QStringLiteral("terms"), query},
        {QStringLiteral("is_or_search"), false},
        {QStringLiteral("time_zone_offset"), QDateTime::currentDateTime().offsetFromUtc()},
        {QStringLiteral("page"), safePage},
        {QStringLiteral("per_page"), safePerPage},
    };

    NetworkRequest request(path);
    httpConnector.post(request, body, HttpResponseCallback(
        [safePerPage, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            if (callback) {
                callback(collectionPageFromDocument(status, doc, safePerPage));
            }
        }));
}

void PostRepository::loadPost(const QString& postId, PostCallback callback)
{
''')

# ---------------------------------------------------------------------------
# Saved preference mutation.
# ---------------------------------------------------------------------------
replace_once("sources/backend/Backend.h",
'''\tvoid updateUserPreferences (const BackendUserPreferences& preferences);
''',
'''\tvoid updateUserPreferences (const BackendUserPreferences& preferences);
\tvoid deleteUserPreferences (const BackendUserPreferences& preferences);
''')

replace_once("sources/backend/Backend.cpp",
'''void Backend::retrieveMultipleUsersStatus (const QVector<QString> & userIDs, std::function<void()> callback)
{
''',
'''void Backend::deleteUserPreferences (const BackendUserPreferences& preferences)
{
    NetworkRequest request("users/" + getLoginUser().id + "/preferences/delete");

    QJsonArray jsonArr;
    jsonArr.push_back(QJsonObject {
        {"user_id", getLoginUser().id},
        {"category", preferences.category},
        {"name", preferences.name},
        {"value", preferences.value},
    });

    httpConnector.post(request, jsonArr, HttpResponseCallback([](const QJsonDocument&) {}));
}

void Backend::retrieveMultipleUsersStatus (const QVector<QString> & userIDs, std::function<void()> callback)
{
''')

# Save action in ordinary message menu.
replace_once("sources/chat-area/post/PostWidget.cpp",
'''    QAction* reactionAction = menu.addAction(tr("Add emoji reaction"));
    connect(reactionAction, &QAction::triggered, this, [this] {
        showEmojiDialog([this](Emoji emoji) {
            backend.addPostReaction(post.id, emoji.name);
        });
    });

    if (post.author) {
''',
'''    QAction* reactionAction = menu.addAction(tr("Add emoji reaction"));
    connect(reactionAction, &QAction::triggered, this, [this] {
        showEmojiDialog([this](Emoji emoji) {
            backend.addPostReaction(post.id, emoji.name);
        });
    });

    QAction* saveAction = menu.addAction(tr("Save message"));
    connect(saveAction, &QAction::triggered, this, [this] {
        backend.updateUserPreferences(BackendUserPreferences {
            QStringLiteral("flagged_post"), post.id, QStringLiteral("true")});
    });

    if (post.author) {
''')

# ---------------------------------------------------------------------------
# Sidebar Saved destination.
# ---------------------------------------------------------------------------
replace_once("sources/channel-tree/ChannelTree.h",
'''\tbool canRemoveChannelFromCategory(const ChannelItem* item) const;
\tvoid removeChannelFromCategory(ChannelItem* item);

protected:
''',
'''\tbool canRemoveChannelFromCategory(const ChannelItem* item) const;
\tvoid removeChannelFromCategory(ChannelItem* item);

signals:
    void virtualDestinationRequested(int destination, const QString& teamId);

protected:
''')

replace_once("sources/channel-tree/ChannelTree.h",
'''    ChannelItem* createPersonalItem(Backend& backend, TeamItem& teamItem,
                                    QTreeWidgetItem& categoryItem);
    QTreeWidgetItem* personalItemForTeam(const QString& teamId) const;
''',
'''    ChannelItem* createPersonalItem(Backend& backend, TeamItem& teamItem,
                                    QTreeWidgetItem& categoryItem);
    ChannelItem* createSavedItem(Backend& backend, TeamItem& teamItem,
                                 QTreeWidgetItem& categoryItem);
    QTreeWidgetItem* personalItemForTeam(const QString& teamId) const;
''')

replace_once("sources/channel-tree/ChannelTree.cpp",
'''        if (favorites) {
            createPersonalItem(backend, teamItem, *categoryItem);
        }
''',
'''        if (favorites) {
            createPersonalItem(backend, teamItem, *categoryItem);
            createSavedItem(backend, teamItem, *categoryItem);
        }
''')

# Insert createSavedItem immediately before personalItemForTeam.
replace_once("sources/channel-tree/ChannelTree.cpp",
'''QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId) const
{
''',
'''ChannelItem* ChannelTree::createSavedItem(Backend& backend, TeamItem& teamItem,
                                          QTreeWidgetItem& categoryItem)
{
    auto* item = new ChannelItem(backend, nullptr);
    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:saved"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::SavedDestination);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Saved"));
    item->setIcon(QIcon(QStringLiteral(":/icons/bookmark")));
    return item;
}

QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId) const
{
''')

# Relax virtual activation guard and route Saved to MainWindow.
replace_once("sources/channel-tree/ChannelTree.cpp",
'''void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar
        || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind
        || item->data(0, ItemDestinationRole).toInt() != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(
''',
'''void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar
        || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind) {
        return;
    }

    const int destination = item->data(0, ItemDestinationRole).toInt();
    if (destination == SidebarItem::SavedDestination) {
        emit virtualDestinationRequested(destination,
                                         item->data(0, ItemTeamIdRole).toString());
        return;
    }
    if (destination != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(
''')

# ---------------------------------------------------------------------------
# MainWindow search entry and collection pages.
# ---------------------------------------------------------------------------
replace_once("sources/mainwindow.h",
'''class NotificationManager;
class SettingsWindow;
struct NotificationTarget;
''',
'''class NotificationManager;
class PostCollectionView;
class SettingsWindow;
struct NotificationTarget;
''')

replace_once("sources/mainwindow.h",
'''\tvoid openDirectMessageSearch ();
\tvoid openAttentionThread (const QString& channelId, const QString& rootPostId);
\tvoid refreshMenuButtonIcon ();
\tvoid refreshUnreadFilterIcon ();
''',
'''\tvoid openDirectMessageSearch ();
    void openMessageSearch();
    void openSavedMessages(const QString& teamId = QString());
    void showCollectionPage(PostCollectionView* page);
\tvoid openAttentionThread (const QString& channelId, const QString& rootPostId);
\tvoid refreshMenuButtonIcon ();
    void refreshSearchButtonIcon();
\tvoid refreshUnreadFilterIcon ();
''')

replace_once("sources/mainwindow.h",
'''\tAttentionList*\t\t\t\t\t\tattentionList = nullptr;
\tQString\t\t\t\t\t\t\t\tretainedUnreadFilterChannelId;
''',
'''\tAttentionList*\t\t\t\t\t\tattentionList = nullptr;
    PostCollectionView*                 savedMessagesPage = nullptr;
    PostCollectionView*                 searchMessagesPage = nullptr;
\tQString\t\t\t\t\t\t\t\tretainedUnreadFilterChannelId;
''')

replace_once("sources/mainwindow.cpp",
'''#include "notifications/NotificationManager.h"
#include "ui/IconUtils.h"
''',
'''#include "notifications/NotificationManager.h"
#include "post-collection/PostCollectionView.h"
#include "ui/IconUtils.h"
''')

replace_once("sources/mainwindow.cpp",
'''\tui->setupUi(this);
\tui->toolButton->installEventFilter(this);
\tsetupChannelTabs();
\trefreshMenuButtonIcon();
''',
'''\tui->setupUi(this);
\tui->toolButton->installEventFilter(this);
    ui->searchButton->installEventFilter(this);
\tsetupChannelTabs();
\trefreshMenuButtonIcon();
    refreshSearchButtonIcon();
    connect(ui->searchButton, &QToolButton::clicked,
            this, &MainWindow::openMessageSearch);
    connect(ui->channelList, &ChannelTree::virtualDestinationRequested,
            this, [this](int destination, const QString& teamId) {
        if (destination == SidebarItem::SavedDestination) {
            openSavedMessages(teamId);
        }
    });
''')

# Add collection navigation methods before openDirectMessageSearch.
replace_once("sources/mainwindow.cpp",
'''void MainWindow::openDirectMessageSearch()
{
''',
'''void MainWindow::showCollectionPage(PostCollectionView* page)
{
    if (!page || !ui || !ui->chatAreaStackedWidget) {
        return;
    }
    if (ChatArea* current = ui->channelList->getCurrentPage()) {
        current->onDeactivate();
    }
    if (ui->chatAreaStackedWidget->indexOf(page) < 0) {
        ui->chatAreaStackedWidget->addWidget(page);
    }
    ui->chatAreaStackedWidget->setCurrentWidget(page);
}

void MainWindow::openSavedMessages(const QString& teamId)
{
    Q_UNUSED(teamId)
    if (!savedMessagesPage) {
        savedMessagesPage = new PostCollectionView(
            backend, PostCollectionView::Mode::Saved, ui->chatAreaStackedWidget);
    }
    showCollectionPage(savedMessagesPage);
    savedMessagesPage->activateSaved();
}

void MainWindow::openMessageSearch()
{
    if (!searchMessagesPage) {
        searchMessagesPage = new PostCollectionView(
            backend, PostCollectionView::Mode::Search, ui->chatAreaStackedWidget);
    }

    QString preferredTeamId;
    if (BackendChannel* channel = backend.getCurrentChannel()) {
        if (channel->team) {
            preferredTeamId = channel->team->id;
        }
    }

    // Search is a transient destination, not the selected sidebar channel. Clear
    // currentItem so clicking the previously active channel will always navigate
    // back even though its tree row did not otherwise change.
    ui->channelList->setCurrentItem(nullptr);
    showCollectionPage(searchMessagesPage);
    searchMessagesPage->activateSearch(preferredTeamId);
}

void MainWindow::openDirectMessageSearch()
{
''')

replace_once("sources/mainwindow.cpp",
'''\tif (event && event->type() == QEvent::PaletteChange) {
\t\tif (watched == ui->toolButton) {
\t\t\trefreshMenuButtonIcon();
\t\t} else if (watched == unreadFilterButton) {
''',
'''\tif (event && event->type() == QEvent::PaletteChange) {
\t\tif (watched == ui->toolButton) {
\t\t\trefreshMenuButtonIcon();
        } else if (watched == ui->searchButton) {
            refreshSearchButtonIcon();
\t\t} else if (watched == unreadFilterButton) {
''')

replace_once("sources/mainwindow.cpp",
'''void MainWindow::refreshUnreadFilterIcon()
{
''',
'''void MainWindow::refreshSearchButtonIcon()
{
    if (!ui || !ui->searchButton) {
        return;
    }
    ui->searchButton->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/search"),
        ui->searchButton->palette().color(QPalette::ButtonText)));
}

void MainWindow::refreshUnreadFilterIcon()
{
''')

# UI: give username room to shrink and insert magnifier next to hamburger.
replace_once("sources/mainwindow.ui",
'''            <width>200</width>
            <height>20</height>
''',
'''            <width>0</width>
            <height>20</height>
''')

replace_once("sources/mainwindow.ui",
'''       <item>
        <widget class="QToolButton" name="toolButton">
''',
'''       <item>
        <widget class="QToolButton" name="searchButton">
         <property name="toolTip">
          <string>Search messages</string>
         </property>
         <property name="accessibleName">
          <string>Search messages</string>
         </property>
         <property name="text">
          <string/>
         </property>
         <property name="icon">
          <iconset resource="../resource.qrc">
           <normaloff>:/icons/search</normaloff>:/icons/search</iconset>
         </property>
         <property name="autoRaise">
          <bool>true</bool>
         </property>
        </widget>
       </item>
       <item>
        <widget class="QToolButton" name="toolButton">
''')

replace_once("sources/mainwindow.ui",
'''  <tabstop>channelList</tabstop>
  <tabstop>toolButton</tabstop>
''',
'''  <tabstop>channelList</tabstop>
  <tabstop>searchButton</tabstop>
  <tabstop>toolButton</tabstop>
''')

# Icons.
write("img/search.svg", r'''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
  <path fill="#000000" d="M6.5 1a5.5 5.5 0 1 0 3.43 9.8l3.64 3.63 1.06-1.06-3.63-3.64A5.5 5.5 0 0 0 6.5 1Zm0 1.5a4 4 0 1 1 0 8 4 4 0 0 1 0-8Z"/>
</svg>
''')
write("img/bookmark.svg", r'''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
  <path fill="#000000" d="M3 1.5h10v13l-5-3-5 3v-13Zm1.5 1.5v8.85L8 9.75l3.5 2.1V3h-7Z"/>
</svg>
''')

replace_once("resource.qrc",
'''        <file alias="burger">img/burger_icon.svg</file>
''',
'''        <file alias="burger">img/burger_icon.svg</file>
        <file alias="search">img/search.svg</file>
        <file alias="bookmark">img/bookmark.svg</file>
''')

# ---------------------------------------------------------------------------
# Documentation: mark Saved/Search as implemented and record syntax/UI.
# ---------------------------------------------------------------------------
replace_once("docs/sidebar-virtual-destinations.md",
'''## Saved

**Saved** (`Сохранённое`) is fundamentally different. Saved posts can originate from multiple channels
and threads, so it must not pretend to be a `BackendChannel`.

It should be modeled as a virtual destination backed by a cross-conversation post collection:
''',
'''## Saved

**Saved** (`Сохранённое`) is fundamentally different. Saved posts can originate from multiple channels
and threads, so it does not pretend to be a `BackendChannel`.

It is implemented as the second fixed local row in Favorites and opens the shared virtualized
`PostCollectionView`. The producer is the paged `/users/{user_id}/posts/flagged` endpoint. Ordinary
message context menus can add `flagged_post` preferences and Saved rows can remove them again.

The destination is backed by a cross-conversation post collection:
''')

replace_once("docs/sidebar-virtual-destinations.md",
'''## Message search

Future message search should reuse the same collection/navigation model as Saved. The difference is
lifetime and producer, not row semantics:
''',
'''## Message search

Message search reuses the same collection/navigation model as Saved. The difference is lifetime and
producer, not row semantics. A magnifier beside the sidebar menu opens the transient Search page;
queries are sent unchanged to Mattermost's search endpoint so server-side syntax remains authoritative.
The UI exposes the standard modifiers `from:`, `in:`, `before:`, `after:` and `on:`, plus reminders for
quoted phrases, exclusions, suffix wildcards and hashtags. Search can target the current/specific team
or the server's all-team search endpoint when supported.
''')

replace_once("docs/sidebar-virtual-destinations.md",
'''The eventual shared collection layer can therefore own:
''',
'''The shared collection layer therefore owns:
''')

print("saved/search patch applied")
