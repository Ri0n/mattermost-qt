/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "ChatArea.h"

#include <QIcon>
#include <QPalette>
#include <QPointer>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include "AbstractPostSource.h"
#include "ChannelPostSource.h"
#include "ChatLogWidget.h"
#include "ThreadPostSource.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/ThreadFollowService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemWidget.h"
#include "channel-tree-dialogs/ViewChannelMembersListDialog.h"
#include "log.h"
#include "navigation/AppNavigationService.h"
#include "post-collection/PostCollectionView.h"
#include "ui/IconUtils.h"
#include "ui_ChatArea.h"

namespace Mattermost {

namespace {

constexpr int HeaderActionIconExtent = 16;

} // namespace

ChatArea::ChatArea(Backend& backend,
                   BackendChannel& channel,
                   ChannelItem* treeItem,
                   QWidget* parent,
                   bool initialize)
    : QWidget(parent)
    , parentArea(nullptr)
    , ui(new Ui::ChatArea)
    , backend(backend)
    , channel(channel)
    , treeItem(treeItem)
    , unreadMessagesCount(0)
    , isThread(false)
    , initialized(false)
{
    setAcceptDrops(true);
    ui->setupUi(this);
    ui->listWidget->configure(backend, *this);
    setupHeaderUi();
    setupPinnedPostsView();
    setupComposerUi();

    ui->outgoingPostCreator->init(backend, channel, *ui->listWidget,
                                  ui->footerLayout, *ui->composerStatusLabel,
                                  *ui->attachButton, *ui->addEmojiButton,
                                  *ui->sendButton);

    ui->titleLabel->setText(channel.display_name);
    ui->statusLabel->setText(channel.getChannelDescription());

    const BackendUser* user = backend.getStorage().getUserById(channel.name);
    if (user) {
        connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
            setUserAvatar(*user);
        });
        connect(user, &BackendUser::onStatusChanged, this, [this, user] {
            ui->statusLabel->setText(user->status);
        });

        if (!user->avatar.isNull()) {
            setUserAvatar(*user);
        } else {
            backend.retrieveUserAvatar(user->id);
        }
        if (ui->statusLabel->text().isEmpty()) {
            ui->statusLabel->setText(user->status);
        }
    } else {
        ui->userAvatar->clear();
        ui->userAvatar->hide();

        backend.retrieveChannelMembers(this->channel, [this] {
            if (!ui) {
                return;
            }
            updateUsersButton();
            ui->usersButton->show();
        });
    }

    // Lazy ChatAreas may be created after the application's eager startup work.
    // Pinned posts are independent from the virtualized timeline and can be
    // requested immediately.
    backend.retrieveChannelPinnedPosts(channel);

    if (initialize) {
        init();
    }
}

ChatArea::ChatArea(Backend& backend,
                   BackendChannel& channel,
                   QString rootId,
                   ChatArea* parentArea)
    : QWidget(nullptr)
    , parentArea(parentArea)
    , parentPostId(rootId)
    , ui(new Ui::ChatArea)
    , backend(backend)
    , channel(channel)
    , treeItem(nullptr)
    , unreadMessagesCount(0)
    , isThread(true)
    , initialized(false)
    , root_id(std::move(rootId))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    ui->setupUi(this);
    ui->listWidget->configure(backend, *this);
    setupHeaderUi();
    setupComposerUi();

    ui->outgoingPostCreator->init(backend, channel, *ui->listWidget,
                                  ui->footerLayout, *ui->composerStatusLabel,
                                  *ui->attachButton, *ui->addEmojiButton,
                                  *ui->sendButton);
    ui->outgoingPostCreator->setRootId(root_id);

    ui->titleLabel->setText(channel.display_name);
    ui->statusLabel->setText(channel.getChannelDescription());
    ui->userAvatar->hide();

    init();

    // Thread follow state is a per-user Mattermost resource. Query it lazily
    // when the thread window opens and use the official PUT/DELETE endpoint.
    if (channel.team && !root_id.isEmpty()) {
        const QString teamId = channel.team->id;
        const QString threadId = root_id;
        threadFollowButton = new QToolButton(this);
        threadFollowButton->setObjectName(QStringLiteral("threadFollowButton"));
        threadFollowButton->setAutoRaise(true);
        threadFollowButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        threadFollowButton->setIconSize(QSize(HeaderActionIconExtent,
                                              HeaderActionIconExtent));
        threadFollowButton->setCursor(Qt::PointingHandCursor);
        threadFollowButton->setEnabled(false);
        threadFollowButton->setProperty("following", false);
        threadFollowButton->setToolTip(tr("Follow thread"));
        threadFollowButton->setAccessibleName(tr("Follow thread"));
        ui->propertieslLayout->addWidget(threadFollowButton, 0, Qt::AlignVCenter);
        refreshHeaderActionIcons();

        QPointer<ChatArea> areaGuard(this);
        QPointer<QToolButton> buttonGuard(threadFollowButton);
        auto setButtonState = [areaGuard, buttonGuard](bool following) {
            if (!areaGuard || !buttonGuard) {
                return;
            }
            buttonGuard->setProperty("following", following);
            const QString label = following ? areaGuard->tr("Unfollow thread")
                                            : areaGuard->tr("Follow thread");
            buttonGuard->setToolTip(label);
            buttonGuard->setAccessibleName(label);
            buttonGuard->setEnabled(true);
            areaGuard->refreshHeaderActionIcons();
        };

        auto& followService = ThreadFollowService::instance(backend);
        connect(&followService, &ThreadFollowService::followingChanged,
                threadFollowButton,
                [teamId, threadId, setButtonState](const QString& changedTeamId,
                                                   const QString& changedThreadId,
                                                   bool following) {
            if (changedTeamId == teamId && changedThreadId == threadId) {
                setButtonState(following);
            }
        });

        followService.queryFollowing(teamId, threadId, setButtonState);
        connect(threadFollowButton, &QToolButton::clicked, this,
                [this, teamId, threadId, buttonGuard, setButtonState] {
            if (!buttonGuard) {
                return;
            }
            const bool following = buttonGuard->property("following").toBool();
            const bool desired = !following;
            buttonGuard->setEnabled(false);
            ThreadFollowService::instance(this->backend).setFollowing(
                teamId, threadId, desired,
                [buttonGuard, following, desired, setButtonState](bool success) {
                    if (!buttonGuard) {
                        return;
                    }
                    setButtonState(success ? desired : following);
                });
        });
    }

    ui->pinnedPostsButton->hide();
    ui->usersButton->hide();
    ui->loadOldPosts->hide();
}

ChatArea::~ChatArea()
{
    deinit();
    if (isThread && parentArea) {
        parentArea->threadsAreas.remove(this);
    }
    delete ui;
}

void ChatArea::setupHeaderUi()
{
    auto configureCountButton = [](QToolButton& button) {
        button.setAutoRaise(true);
        button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button.setIconSize(QSize(HeaderActionIconExtent,
                                 HeaderActionIconExtent));
        button.setCursor(Qt::PointingHandCursor);
    };

    configureCountButton(*ui->usersButton);
    configureCountButton(*ui->pinnedPostsButton);
    ui->pinnedPostsButton->setCheckable(true);

    ui->loadOldPosts->setAutoRaise(true);
    ui->propertieslLayout->setSpacing(2);
    refreshHeaderActionIcons();
}

void ChatArea::setupPinnedPostsView()
{
    if (isThread || contentStack || !ui || !ui->listWidget) {
        return;
    }

    const int listIndex = ui->verticalLayout->indexOf(ui->listWidget);
    if (listIndex < 0) {
        return;
    }

    contentStack = new QStackedWidget(this);
    ui->verticalLayout->removeWidget(ui->listWidget);
    contentStack->addWidget(ui->listWidget);

    pinnedPostsView = new PostCollectionView(
        backend, PostCollectionView::Mode::Pinned, contentStack);
    contentStack->addWidget(pinnedPostsView);
    contentStack->setCurrentWidget(ui->listWidget);
    ui->verticalLayout->insertWidget(listIndex, contentStack, 1);

    connect(pinnedPostsView, &PostCollectionView::postActivated,
            this, [this](const QString& postId) {
        showPinnedPosts(false);
        AppNavigationService::instance(backend).openPost(postId);
    });
    pinnedPostsView->activatePinned(channel);
}

void ChatArea::showPinnedPosts(bool show)
{
    if (!contentStack || !pinnedPostsView || !ui || !ui->listWidget) {
        return;
    }

    if (show && channel.pinnedPosts.empty()) {
        show = false;
    }
    ui->pinnedPostsButton->setChecked(show);
    contentStack->setCurrentWidget(show
        ? static_cast<QWidget*>(pinnedPostsView)
        : static_cast<QWidget*>(ui->listWidget));
}

void ChatArea::refreshHeaderActionIcons()
{
    if (!ui) {
        return;
    }

    if (ui->usersButton) {
        ui->usersButton->setIcon(IconUtils::tintedSymbolicIcon(
            QStringLiteral(":/icons/members"),
            ui->usersButton->palette().color(QPalette::ButtonText)));
    }
    if (ui->pinnedPostsButton) {
        ui->pinnedPostsButton->setIcon(IconUtils::tintedSymbolicIcon(
            QStringLiteral(":/icons/pin"),
            ui->pinnedPostsButton->palette().color(QPalette::ButtonText)));
    }
    if (threadFollowButton) {
        const bool following = threadFollowButton->property("following").toBool();
        threadFollowButton->setIcon(IconUtils::tintedSymbolicIcon(
            following ? QStringLiteral(":/icons/bell-filled")
                      : QStringLiteral(":/icons/bell"),
            threadFollowButton->palette().color(QPalette::ButtonText)));
    }
}

void ChatArea::updateUsersButton()
{
    if (!ui || !ui->usersButton) {
        return;
    }

    const qulonglong memberCount = static_cast<qulonglong>(channel.members.size());
    ui->usersButton->setText(QString::number(memberCount));
    const QString tooltip = memberCount == 1
        ? tr("1 member")
        : tr("%1 members").arg(memberCount);
    ui->usersButton->setToolTip(tooltip);
    ui->usersButton->setAccessibleName(tooltip);
}

void ChatArea::setupPostSource()
{
    if (!postSource) {
        if (isThread) {
            postSource = new ThreadPostSource(backend, channel, root_id, this);
        } else {
            postSource = new ChannelPostSource(backend, channel, this);
        }
    }
    ui->listWidget->setSource(postSource);
}

void ChatArea::scheduleNewestPosition()
{
    const std::uint64_t generation = viewportNavigationGeneration;
    QPointer<ChatArea> guard(this);
    QTimer::singleShot(0, this, [guard, generation] {
        if (!guard || !guard->ui || !guard->ui->listWidget
            || generation != guard->viewportNavigationGeneration
            || !guard->pendingPostId.isEmpty()) {
            return;
        }
        guard->ui->listWidget->scrollToEnd();
    });
}

void ChatArea::scheduleStoredPosition()
{
    if (storedViewportPostId.isEmpty()) {
        scheduleNewestPosition();
        return;
    }

    const std::uint64_t generation = viewportNavigationGeneration;
    const QString postId = storedViewportPostId;
    QPointer<ChatArea> guard(this);
    QTimer::singleShot(0, this, [guard, generation, postId] {
        if (!guard || !guard->ui || !guard->ui->listWidget
            || generation != guard->viewportNavigationGeneration
            || !guard->pendingPostId.isEmpty()) {
            return;
        }

        if (!guard->ui->listWidget->restoreViewportBookmark(postId)) {
            guard->scheduleNewestPosition();
        }
    });
}

void ChatArea::finishPendingNavigation()
{
    if (pendingPostId.isEmpty() || !ui || !ui->listWidget) {
        return;
    }
    if (ui->listWidget->findPost(pendingPostId)) {
        const QString target = pendingPostId;
        pendingPostId.clear();
        ui->listWidget->highlightPost(target);
    }
}

void ChatArea::init()
{
    if (initialized) {
        return;
    }

    setupPostSource();

    // The semantic source object remains alive while a channel page is inactive,
    // but ChatLogWidget detaches from it so all concrete PostWidgets and their
    // repository residency leases can be released. Rebuild only orchestration
    // connections and the view attachment on activation.
    if (!isThread) {
        signalConnections.push_back(connect(&channel, &BackendChannel::onViewed,
                                            this, [this] {
            LOG_DEBUG("Channel viewed: " << channel.display_name);
            setUnreadMessagesCount(0);
        }));

        signalConnections.push_back(connect(&channel, &BackendChannel::onUpdated,
                                            this, [this] {
            ui->titleLabel->setText(channel.display_name);
            ui->statusLabel->setText(channel.getChannelDescription());
            if (treeItem) {
                treeItem->setLabel(channel.display_name);
            }
        }));

        signalConnections.push_back(connect(&channel,
                                            &BackendChannel::onPinnedPostsReceived,
                                            this, [this] {
            updatePinnedPostsButton();
        }));

        signalConnections.push_back(connect(&channel, &BackendChannel::onUserAdded,
                                            this, [this](const BackendUser&) {
            updateUsersButton();
        }));
        signalConnections.push_back(connect(&channel, &BackendChannel::onUserRemoved,
                                            this, [this](const BackendUser&) {
            updateUsersButton();
        }));

        signalConnections.push_back(connect(ui->listWidget,
                                            &LongListWidget::userViewportChanged,
                                            this, [this](bool atEnd) {
            if (atEnd) {
                markChannelViewedIfAtBottom();
            }
        }));

        signalConnections.push_back(connect(ui->usersButton, &QToolButton::clicked,
                                            this, [this] {
            auto* dialog = new ViewChannelMembersListDialog(backend, channel, this);
            dialog->show();
        }));

        signalConnections.push_back(connect(ui->pinnedPostsButton,
                                            &QToolButton::clicked,
                                            this, [this](bool checked) {
            showPinnedPosts(checked);
        }));
    }

    auto& composer = *ui->outgoingPostCreator;
    signalConnections.push_back(connect(&channel, &BackendChannel::onNewPost,
                                        &composer,
                                        &OutgoingPostCreator::onPostReceived));
    signalConnections.push_back(connect(&channel, &BackendChannel::onPostEdited,
                                        &composer,
                                        &OutgoingPostCreator::onPostReceived));
    signalConnections.push_back(connect(&channel, &BackendChannel::onUserTyping,
                                        this, &ChatArea::handleUserTyping));

    signalConnections.push_back(connect(ui->listWidget,
                                        &ChatLogWidget::postEditInitiated,
                                        &composer,
                                        &OutgoingPostCreator::postEditInitiated));
    signalConnections.push_back(connect(&composer,
                                        &OutgoingPostCreator::postEditFinished,
                                        ui->listWidget,
                                        &ChatLogWidget::postEditFinished));

    signalConnections.push_back(connect(ui->listWidget,
                                        &LongListWidget::materializedRangeChanged,
                                        this, [this](int, int) {
        finishPendingNavigation();
    }));

    ui->loadOldPosts->hide();
    if (!isThread) {
        updatePinnedPostsButton();
        ui->usersButton->hide();
    }

    initialized = true;
    if (!isThread && !storedViewportPostId.isEmpty()) {
        scheduleStoredPosition();
    } else {
        scheduleNewestPosition();
    }

    if (!pendingPostId.isEmpty()) {
        goToPost(pendingPostId);
    }
}

void ChatArea::deinit()
{
    if (!initialized) {
        return;
    }

    for (const QMetaObject::Connection& connection : signalConnections) {
        disconnect(connection);
    }
    signalConnections.clear();

    QObject::disconnect(explicitReadPostsConnection);
    explicitReadPostsConnection = QMetaObject::Connection();
    explicitReadPending = false;
    initialized = false;
}

void ChatArea::setUserAvatar(const BackendUser& user)
{
    ui->userAvatar->setPixmap(user.avatar);
    if (channel.type == BackendChannel::directChannel && !isThread && treeItem) {
        treeItem->setIcon(QIcon(user.avatar));
    }
}

Ui::ChatArea* ChatArea::getUi()
{
    return ui;
}

Backend& ChatArea::getBackend()
{
    return backend;
}

BackendChannel& ChatArea::getChannel()
{
    return channel;
}

void ChatArea::updatePinnedPostsButton()
{
    if (isThread) {
        ui->pinnedPostsButton->hide();
        return;
    }

    if (pinnedPostsView) {
        pinnedPostsView->activatePinned(channel);
    }

    if (channel.pinnedPosts.empty()) {
        showPinnedPosts(false);
        ui->pinnedPostsButton->hide();
        return;
    }

    const qulonglong pinnedPostCount =
        static_cast<qulonglong>(channel.pinnedPosts.size());
    ui->pinnedPostsButton->setText(QString::number(pinnedPostCount));
    const QString tooltip = pinnedPostCount == 1
        ? tr("1 pinned post")
        : tr("%1 pinned posts").arg(pinnedPostCount);
    ui->pinnedPostsButton->setToolTip(tooltip);
    ui->pinnedPostsButton->setAccessibleName(tooltip);
    ui->pinnedPostsButton->show();
}

void ChatArea::markChannelViewedIfAtBottom()
{
    if (isThread || !initialized || !ui->listWidget->isAtEnd()) {
        return;
    }
    setUnreadMessagesCount(0);
    SidebarService::instance(backend).markChannelViewedLocally(channel);
    backend.markChannelAsViewed(channel);
}

void ChatArea::handleUserTyping(const BackendUser& user)
{
    LOG_DEBUG("Channel " << channel.display_name << ": "
                          << user.getDisplayName() << " is typing");
}

void ChatArea::onActivate()
{
    backend.setCurrentChannel(channel);

    // Pinned is an inspection mode, not durable channel state. Entering a chat
    // always returns to the canonical conversation surface.
    if (!isThread) {
        showPinnedPosts(false);
    }

    init();
}

void ChatArea::onDeactivate()
{
    if (!isThread && ui && ui->listWidget) {
        showPinnedPosts(false);

        QString postId;
        if (ui->listWidget->captureViewportBookmark(postId)) {
            storedViewportPostId = postId;
        }
    }

    deinit();

    if (!isThread && ui && ui->listWidget) {
        // Keep ChannelPostSource alive as the semantic timeline authority while
        // dropping the concrete view. setItemCount(0) inside setSource(nullptr)
        // destroys every materialized PostWidget and releases its residency lease.
        ui->listWidget->setSource(nullptr);
    }
}

void ChatArea::onMainWindowActivate()
{
    // Merely restoring/focusing the application is not a reading gesture.
}

void ChatArea::onMove(QPoint)
{
    // Pinned messages are embedded in the ChatArea; no floating child follows
    // the main-window position anymore.
}

void ChatArea::moveOnListTop()
{
    if (!treeItem || !treeItem->parent() || !treeItem->treeWidget()) {
        return;
    }

    QTreeWidgetItem* parent = treeItem->parent();
    QTreeWidget* tree = treeItem->treeWidget();
    if (parent->indexOfChild(treeItem) == 0) {
        return;
    }

    const bool isCurrent = tree->currentItem() == treeItem;
    auto* thisItemWidget = static_cast<ChannelItemWidget*>(
        tree->itemWidget(treeItem, 0));
    if (!thisItemWidget) {
        return;
    }

    auto* newItemWidget = new ChannelItemWidget(thisItemWidget->parentWidget());
    newItemWidget->setLabel(channel.display_name);
    if (!thisItemWidget->getPixmap().isNull()) {
        newItemWidget->setIcon(QIcon(thisItemWidget->getPixmap()));
    }

    tree->blockSignals(true);
    QTreeWidgetItem* child = parent->takeChild(parent->indexOfChild(treeItem));
    parent->insertChild(0, child);
    tree->blockSignals(false);

    if (child != treeItem) {
        return;
    }

    tree->setItemWidget(child, 0, newItemWidget);
    treeItem->setWidget(newItemWidget);
    if (isCurrent) {
        tree->setCurrentItem(child);
    }
}

void ChatArea::setUnreadMessagesCount(uint32_t count)
{
    unreadMessagesCount = count;
    if (!treeItem) {
        return;
    }
    treeItem->setText(1, count == 0 ? QString() : QString::number(count));
}

void ChatArea::resizeEvent(QResizeEvent* event)
{
    // ChatLogWidget/LongListWidget owns its own durable viewport anchor and
    // remeasures visible rows from its resizeEvent(). ChatArea must not mutate
    // the scrollbar as a side effect of outer layout changes.
    QWidget::resizeEvent(event);
}

void ChatArea::dragEnterEvent(QDragEnterEvent* event)
{
    ui->outgoingPostCreator->onDragEnterEvent(event);
}

void ChatArea::dragMoveEvent(QDragMoveEvent* event)
{
    ui->outgoingPostCreator->onDragMoveEvent(event);
}

void ChatArea::dropEvent(QDropEvent* event)
{
    ui->outgoingPostCreator->onDropEvent(event);
}

void ChatArea::goToPost(const BackendPost& post)
{
    goToPost(post.id);
}

void ChatArea::goToPost(const QString& postId)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return;
    }

    // Any semantic post navigation returns the channel content surface from the
    // optional pinned collection to the canonical timeline first. The hidden
    // ChatLogWidget has stayed alive, so this does not disturb its viewport.
    showPinnedPosts(false);

    // Opening/reactivating a channel schedules a weak activation position on
    // the next event-loop turn. Explicit post navigation supersedes that intent,
    // even when the destination materializes immediately and pendingPostId is
    // cleared before the queued callback gets a chance to run.
    ++viewportNavigationGeneration;

    if (!ui->listWidget->ensurePostVisible(postId,
                                           LongListWidget::Alignment::Center)) {
        pendingPostId = postId;
        return;
    }

    pendingPostId = postId;
    finishPendingNavigation();
}

} /* namespace Mattermost */
