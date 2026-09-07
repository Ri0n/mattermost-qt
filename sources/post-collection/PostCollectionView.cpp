#include "PostCollectionView.h"

#include <algorithm>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "chat-area/post/PostWidget.h"
#include "navigation/AppNavigationService.h"
#include "widgets/InteractiveTextEdit.h"
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
        setRequestBlockSize(10);
        setPrefetchScreens(1);
        setSeekDebounceMs(100);

        // Collection pagination is deliberately driven only by an actual user
        // viewport gesture. LongListWidget prefetch/materialization must never
        // turn a popular search into an automatic request chain.
        connect(this, &LongListWidget::userViewportChanged, this,
                [this](bool atEnd) {
            if (!owner.hasMoreResults() || owner.loading) {
                return;
            }
            const Range visible = visibleRange();
            const int threshold = std::max(
                0, static_cast<int>(owner.posts.size()) - 2);
            if (!atEnd && (!visible.isValid() || visible.last < threshold)) {
                return;
            }
            QTimer::singleShot(0, this, [this] {
                if (owner.hasMoreResults() && !owner.loading) {
                    owner.loadNextPage();
                }
            });
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

        searchEdit = new InteractiveTextEdit(this);
        searchEdit->setAcceptRichText(false);
        searchEdit->setLineWrapMode(QTextEdit::NoWrap);
        searchEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        searchEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        searchEdit->setTabChangesFocus(true);
        searchEdit->setSubmitOnEnter(true);
        searchEdit->setSubmitHandler([this] { startSearch(); });
        searchEdit->setPlaceholderText(tr("Search messages…"));
        searchEdit->setFixedHeight(
            std::max(30, searchEdit->fontMetrics().lineSpacing() + 12));
        configureSearchCompletions();
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

void PostCollectionView::configureSearchCompletions()
{
    if (!searchEdit) {
        return;
    }

    QVector<InteractiveTextEdit::CompletionRule> rules;

    InteractiveTextEdit::CompletionRule channelRule;
    channelRule.prefix = QStringLiteral("in:");
    channelRule.provider = [this] {
        QVector<InteractiveTextEdit::CompletionCandidate> candidates;
        const auto& channels = backend.getStorage().channels;
        candidates.reserve(channels.size());

        for (auto it = channels.cbegin(); it != channels.cend(); ++it) {
            BackendChannel* channel = it.value();
            if (!channel || channel->id.isEmpty()) {
                continue;
            }

            const bool namedChannel = channel->type == BackendChannel::publicChannel
                || channel->type == BackendChannel::privateChannel;
            const QString canonical = namedChannel && !channel->name.isEmpty()
                ? channel->name : channel->id;
            QString display = channel->display_name.trimmed();
            if (display.isEmpty()) {
                display = !channel->name.isEmpty() ? channel->name : canonical;
            }

            InteractiveTextEdit::CompletionCandidate candidate;
            candidate.displayText = display;
            candidate.insertText = canonical;
            if (namedChannel && !channel->name.isEmpty()
                && channel->name != display) {
                candidate.detailText = channel->name;
                candidate.filterKeys.push_back(channel->name);
            }
            candidates.push_back(std::move(candidate));
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
            return QString::localeAwareCompare(lhs.displayText, rhs.displayText) < 0;
        });
        return candidates;
    };
    rules.push_back(std::move(channelRule));

    InteractiveTextEdit::CompletionRule userRule;
    userRule.prefix = QStringLiteral("from:");
    userRule.provider = [this] {
        QVector<InteractiveTextEdit::CompletionCandidate> candidates;
        const auto& users = backend.getStorage().getAllUsers();
        candidates.reserve(static_cast<int>(users.size()));

        for (const auto& entry : users) {
            const BackendUser& user = entry.second;
            if (user.username.isEmpty()) {
                continue;
            }

            InteractiveTextEdit::CompletionCandidate candidate;
            candidate.displayText = user.getDisplayName();
            if (candidate.displayText.isEmpty()) {
                candidate.displayText = user.username;
            }
            candidate.insertText = user.username;
            candidate.detailText = QStringLiteral("@") + user.username;
            if (!user.nickname.isEmpty()) {
                candidate.filterKeys.push_back(user.nickname);
            }
            if (!user.first_name.isEmpty()) {
                candidate.filterKeys.push_back(user.first_name);
            }
            if (!user.last_name.isEmpty()) {
                candidate.filterKeys.push_back(user.last_name);
            }
            candidates.push_back(std::move(candidate));
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
            return QString::localeAwareCompare(lhs.displayText, rhs.displayText) < 0;
        });
        return candidates;
    };
    rules.push_back(std::move(userRule));

    searchEdit->setCompletionRules(std::move(rules));
}

void PostCollectionView::insertSearchToken(const QString& token)
{
    if (!searchEdit) {
        return;
    }
    const QString current = searchEdit->toPlainText();
    if (!current.isEmpty() && !current.endsWith(QLatin1Char(' '))) {
        searchEdit->insertPlainText(QStringLiteral(" "));
    }
    searchEdit->insertPlainText(token);
    searchEdit->setFocus(Qt::ShortcutFocusReason);
}

void PostCollectionView::startSearch()
{
    if (mode != Mode::Search || !searchEdit) {
        return;
    }

    const QString terms = searchEdit->toPlainText().trimmed();
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
    bufferedPosts.clear();
    bufferedOffset = 0;
    nextPage = 0;
    loading = false;
    serverHasMore = false;
    updateStatus();
}

bool PostCollectionView::hasMoreResults() const
{
    return serverHasMore
        || bufferedOffset < static_cast<int>(bufferedPosts.size());
}

void PostCollectionView::appendPosts(const QVector<QJsonObject>& rawPosts)
{
    const int oldCount = static_cast<int>(posts.size());
    for (const QJsonObject& raw : rawPosts) {
        const QString postId = raw.value(QStringLiteral("id")).toString();
        if (postId.isEmpty() || postIds.contains(postId)) {
            continue;
        }
        postIds.insert(postId);
        posts.push_back(std::make_unique<BackendPost>(raw, backend.getStorage()));
    }

    const int newCount = static_cast<int>(posts.size());
    if (!list || newCount == oldCount) {
        return;
    }

    list->setItemCount(newCount);
    for (int index = oldCount; index < newCount; ++index) {
        list->setRangeAvailable(index, index, true);
    }
}

bool PostCollectionView::appendBufferedPage()
{
    const int bufferedCount = static_cast<int>(bufferedPosts.size());
    if (bufferedOffset < 0 || bufferedOffset >= bufferedCount) {
        return false;
    }

    const int end = std::min(bufferedCount, bufferedOffset + PageSize);
    QVector<QJsonObject> page;
    page.reserve(end - bufferedOffset);
    for (int index = bufferedOffset; index < end; ++index) {
        page.push_back(bufferedPosts.at(index));
    }
    bufferedOffset = end;
    if (bufferedOffset >= bufferedCount) {
        bufferedPosts.clear();
        bufferedOffset = 0;
    }

    appendPosts(page);
    updateStatus();
    return true;
}

void PostCollectionView::loadNextPage()
{
    if (loading) {
        return;
    }
    if (bufferedOffset < static_cast<int>(bufferedPosts.size())) {
        appendBufferedPage();
        return;
    }
    if (nextPage > 0 && !serverHasMore) {
        return;
    }
    if (mode == Mode::Search && activeTerms.isEmpty()) {
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
            guard->serverHasMore = false;
            guard->bufferedPosts.clear();
            guard->bufferedOffset = 0;
            if (guard->statusLabel) {
                const int count = static_cast<int>(guard->posts.size());
                if (count > 0) {
                    guard->statusLabel->setText(
                        guard->tr("%1 messages loaded — loading more failed").arg(count));
                } else {
                    guard->statusLabel->setText(guard->mode == Mode::Saved
                        ? guard->tr("Could not load saved messages.")
                        : guard->tr("Search failed."));
                }
            }
            return;
        }

        const int oldCount = static_cast<int>(guard->posts.size());
        guard->nextPage = page + 1;

        if (result.completeResultSet) {
            // The backend ignored per_page (database search is the common
            // example). Keep the full response in memory but expose only one
            // ten-row page now; subsequent pages are revealed on user scroll
            // without issuing the same expensive search again.
            guard->serverHasMore = false;
            guard->bufferedPosts = result.posts;
            guard->bufferedOffset = 0;
            guard->appendBufferedPage();
            return;
        }

        guard->appendPosts(result.posts);
        const int newCount = static_cast<int>(guard->posts.size());
        guard->serverHasMore = result.hasMore && newCount > oldCount;
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
    statusLabel->setText(hasMoreResults()
        ? tr("%1 messages loaded — scroll for more").arg(count)
        : tr("%1 messages").arg(count));
}

} // namespace Mattermost
