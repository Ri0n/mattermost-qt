#include "PostCollectionView.h"

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
    explicit CollectionList(PostCollectionView& collectionOwner, QWidget* parent)
        : LongListWidget(parent)
        , owner(collectionOwner)
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
            if (guard->list) {
                guard->list->setItemCount(static_cast<int>(guard->posts.size()));
            }
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
