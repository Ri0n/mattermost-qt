#include "PostCollectionView.h"

#include <algorithm>

#include <QAbstractButton>
#include <QCalendarWidget>
#include <QComboBox>
#include <QDate>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/PostRepository.h"
#include "backend/QByteArrayCreator.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "chat-area/PostListWidget.h"
#include "chat-area/post/PostWidget.h"
#include "navigation/AppNavigationService.h"
#include "ui/ThemeIconWidgets.h"
#include "widgets/InteractiveTextEdit.h"

namespace Mattermost {

class PostCollectionView::CollectionList final : public PostListWidget
{
public:
    explicit CollectionList(PostCollectionView& collectionOwner, QWidget* parent)
        : PostListWidget(parent)
        , _owner(collectionOwner)
    {
        setDefaultItemHeight(132);

        // Collection pagination is deliberately driven only by an actual user
        // viewport gesture. LongListWidget prefetch/materialization must never
        // turn a popular search into an automatic request chain.
        connect(this, &LongListWidget::userViewportChanged, this,
                [this](bool atEnd) {
            if (!_owner.hasMoreResults() || _owner.loading) {
                return;
            }
            const Range visible = visibleRange();
            const int threshold = std::max(
                0, static_cast<int>(_owner.posts.size()) - 2);
            if (!atEnd && (!visible.isValid() || visible.last < threshold)) {
                return;
            }
            QTimer::singleShot(0, this, [this] {
                if (_owner.hasMoreResults() && !_owner.loading) {
                    _owner.loadNextPage();
                }
            });
        });
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return _owner.createRow(index, viewport());
    }

private:
    PostCollectionView& _owner;
};

PostCollectionView::PostCollectionView(Backend& backendInstance, Mode viewMode, QWidget* parent)
    : QWidget(parent)
    , backend(backendInstance)
    , mode(viewMode)
{
    connect(&actionConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&actionConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
    buildUi();
}

PostCollectionView::~PostCollectionView()
{
    if (list) {
        // Destroy PostWidgets before either collection-owned snapshots or
        // borrowed pinned-post objects can disappear.
        list->setItemCount(0);
    }
}

void PostCollectionView::buildUi()
{
    auto* root = new QVBoxLayout(this);
    // Pinned is embedded inside ChatArea, which already owns the standard 2px
    // page gutter. Saved/Search are standalone stacked pages and provide that
    // same gutter themselves. Do not stack the old 8px collection inset on top
    // of the shared PostListWidget viewport policy.
    const int outerMargin = mode == Mode::Pinned ? 0 : 2;
    root->setContentsMargins(outerMargin, outerMargin, outerMargin, outerMargin);
    root->setSpacing(6);

    auto* header = new QHBoxLayout;
    QString titleText;
    switch (mode) {
    case Mode::Saved:
        titleText = tr("0 saved messages");
        break;
    case Mode::Search:
        titleText = tr("Search messages");
        break;
    case Mode::Pinned:
        titleText = tr("Pinned messages");
        break;
    }
    _titleLabel = new QLabel(titleText, this);
    QFont titleFont = _titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    _titleLabel->setFont(titleFont);
    header->addWidget(_titleLabel);
    header->addStretch();

    if (mode == Mode::Saved) {
        _refreshButton = new ThemeIconButton(this);
        _refreshButton->setText(QString());
        _refreshButton->setFixedSize(28, 28);
        _refreshButton->setIconSize(QSize(16, 16));
        _refreshButton->setProperty(ThemeIconResourceProperty,
                                    QStringLiteral(":/icons/refresh"));
        _refreshButton->setToolTip(tr("Refresh saved messages"));
        _refreshButton->setAccessibleName(tr("Refresh saved messages"));
        connect(_refreshButton, &QPushButton::clicked,
                this, &PostCollectionView::activateSaved);
        header->addWidget(_refreshButton);
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
        auto addDateModifier = [this, modifiers](const QString& label,
                                                 const QString& token) {
            auto* button = new QToolButton(this);
            button->setText(label);
            button->setAutoRaise(true);
            button->setPopupMode(QToolButton::InstantPopup);

            auto* menu = new QMenu(button);
            auto* calendar = new QCalendarWidget(menu);
            calendar->setGridVisible(true);
            auto* action = new QWidgetAction(menu);
            action->setDefaultWidget(calendar);
            menu->addAction(action);
            button->setMenu(menu);

            connect(calendar, &QCalendarWidget::clicked, this,
                    [this, token, menu](const QDate& date) {
                insertSearchToken(token + date.toString(Qt::ISODate));
                menu->close();
            });
            modifiers->addWidget(button);
        };
        addModifier(QStringLiteral("from:"), QStringLiteral("from:"));
        addModifier(QStringLiteral("in:"), QStringLiteral("in:"));
        addDateModifier(QStringLiteral("before:"), QStringLiteral("before:"));
        addDateModifier(QStringLiteral("after:"), QStringLiteral("after:"));
        addDateModifier(QStringLiteral("on:"), QStringLiteral("on:"));
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

void PostCollectionView::activatePinned(BackendChannel& channel)
{
    if (mode != Mode::Pinned) {
        return;
    }

    ++generation;
    pinnedChannel = &channel;
    resetCollection();

    for (BackendPost& post : channel.pinnedPosts) {
        if (post.id.isEmpty() || postIds.contains(post.id)) {
            continue;
        }
        postIds.insert(post.id);
        posts.push_back(&post);
    }

    if (list) {
        const int count = static_cast<int>(posts.size());
        list->setItemCount(count);
        for (int index = 0; index < count; ++index) {
            list->setRangeAvailable(index, index, true);
        }
        if (count > 0) {
            list->scrollToIndex(0, LongListWidget::Alignment::Top);
        }
    }
    updateStatus();
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
    ownedPosts.clear();
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
    return mode != Mode::Pinned
        && (serverHasMore
            || bufferedOffset < static_cast<int>(bufferedPosts.size()));
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
        auto owned = std::make_unique<BackendPost>(raw, backend.getStorage());
        posts.push_back(owned.get());
        ownedPosts.push_back(std::move(owned));
    }

    const int newCount = static_cast<int>(posts.size());
    if (!list || newCount == oldCount) {
        return;
    }

    list->setItemCount(newCount);
    for (int index = oldCount; index < newCount; ++index) {
        list->setRangeAvailable(index, index, true);
    }

    // Search is a result collection, not a live chat timeline. The first result
    // is the collection origin and should be shown at the top; later pages keep
    // the user's current viewport while extending the list downward.
    if (mode == Mode::Search && oldCount == 0) {
        list->scrollToIndex(0, LongListWidget::Alignment::Top);
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
    if (mode == Mode::Pinned || loading) {
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
            guard->updateStatus();
            if (guard->statusLabel) {
                const int count = static_cast<int>(guard->posts.size());
                guard->statusLabel->setVisible(true);
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
    if (mode != Mode::Pinned) {
        auto* origin = new QLabel(originLabel(post), row);
        QFont originFont = origin->font();
        originFont.setBold(true);
        origin->setFont(originFont);
        metadata->addWidget(origin);
    }
    metadata->addStretch();

    auto configureActionButton = [](ThemeIconButton* button, const QString& resource) {
        button->setText(QString());
        button->setFixedSize(26, 24);
        button->setIconSize(QSize(16, 16));
        button->setProperty(ThemeIconResourceProperty, resource);
    };

    if (mode == Mode::Saved || mode == Mode::Pinned) {
        auto* remove = new ThemeIconButton(row);
        configureActionButton(remove, QStringLiteral(":/icons/trash"));
        const QString removeLabel = mode == Mode::Pinned
            ? tr("Unpin message") : tr("Remove from saved");
        remove->setToolTip(removeLabel);
        remove->setAccessibleName(removeLabel);
        if (mode == Mode::Pinned) {
            connect(remove, &QPushButton::clicked, this,
                    [this, postId, remove] { unpinPost(postId, remove); });
        } else {
            connect(remove, &QPushButton::clicked, this,
                    [this, postId] { removeSavedPost(postId); });
        }
        metadata->addWidget(remove);
    }

    auto* jump = new ThemeIconButton(row);
    configureActionButton(jump, QStringLiteral(":/icons/jump"));
    jump->setToolTip(tr("Show this message in its conversation"));
    jump->setAccessibleName(tr("Jump to message"));
    connect(jump, &QPushButton::clicked, this, [this, postId] {
        if (mode == Mode::Pinned) {
            emit postActivated(postId);
            return;
        }
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
    if (mode != Mode::Saved || postId.isEmpty() || indexOfPost(postId) < 0) {
        return;
    }

    BackendUserPreferences preference {
        QStringLiteral("flagged_post"), postId, QStringLiteral("true")};
    backend.deleteUserPreferences(preference);
    removeSavedPostLocally(postId);
}

void PostCollectionView::removeSavedPostLocally(const QString& postId)
{
    if (mode != Mode::Saved || postId.isEmpty()) {
        return;
    }
    const int index = indexOfPost(postId);
    if (index < 0) {
        return;
    }

    // removeItems destroys the materialized PostWidget before we release the
    // collection-owned BackendPost it references.
    if (list) {
        list->removeItems(index, 1);
    }
    posts.erase(posts.begin() + index);
    ownedPosts.erase(ownedPosts.begin() + index);
    postIds.remove(postId);
    updateStatus();
}

void PostCollectionView::handleFlaggedPostChanged(const QString& postId, bool flagged)
{
    if (mode != Mode::Saved || postId.isEmpty()) {
        return;
    }
    if (!flagged) {
        removeSavedPostLocally(postId);
        return;
    }
    if (!postIds.contains(postId) && !loading) {
        QTimer::singleShot(0, this, &PostCollectionView::activateSaved);
    }
}

void PostCollectionView::unpinPost(const QString& postId, QAbstractButton* button)
{
    if (mode != Mode::Pinned || !pinnedChannel || postId.isEmpty()) {
        return;
    }
    if (button) {
        button->setEnabled(false);
    }

    const QString channelId = pinnedChannel->id;
    QPointer<PostCollectionView> guard(this);
    NetworkRequest request(QStringLiteral("posts/") + postId + QStringLiteral("/unpin"));
    actionConnector.post(request, QByteArrayCreator(QByteArray()), HttpResponseCallback(
        [guard, channelId](const QJsonDocument&) {
            if (!guard || !guard->pinnedChannel
                || guard->pinnedChannel->id != channelId) {
                return;
            }
            guard->backend.retrieveChannelPinnedPosts(*guard->pinnedChannel);
        }));
}

void PostCollectionView::updateStatus()
{
    const int count = static_cast<int>(posts.size());

    if (_titleLabel && mode == Mode::Saved) {
        _titleLabel->setText(tr("%n saved message(s)", nullptr, count));
    }
    if (_refreshButton) {
        _refreshButton->setProperty(ThemeIconBusyProperty, loading);
        _refreshButton->setEnabled(!loading);
    }
    if (!statusLabel) {
        return;
    }

    statusLabel->setVisible(false);

    // Saved uses its compact count as the title and the refresh icon itself as
    // the loading indicator, so a second status line would only duplicate it.
    if (mode == Mode::Saved) {
        return;
    }

    // A non-empty pinned list already has its count in the channel-header badge.
    if (mode == Mode::Pinned) {
        if (posts.empty()) {
            statusLabel->setText(tr("No pinned messages."));
            statusLabel->setVisible(true);
        }
        return;
    }

    statusLabel->setVisible(true);
    if (loading) {
        statusLabel->setText(tr("Loading…"));
        return;
    }
    if (activeTerms.isEmpty()) {
        statusLabel->setText(tr("Enter search terms or use a modifier above."));
        return;
    }
    if (posts.empty()) {
        statusLabel->setText(tr("No matching messages."));
        return;
    }
    statusLabel->setText(hasMoreResults()
        ? tr("%1 messages loaded — scroll for more").arg(count)
        : tr("%1 messages").arg(count));
}

} // namespace Mattermost
