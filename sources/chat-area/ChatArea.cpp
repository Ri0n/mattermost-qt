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

#include <QDockWidget>
#include <QIcon>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QTreeWidget>

#include "AbstractPostSource.h"
#include "ChannelPostSource.h"
#include "ChatLogWidget.h"
#include "PinnedPostsList.h"
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
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

namespace {

const QIcon& getUserButtonIcon()
{
    static QIcon icon(QStringLiteral(":/img/user-icon.png"));
    return icon;
}

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
    , pinnedPostsDockWidget(nullptr)
    , unreadMessagesCount(0)
    , isThread(false)
    , initialized(false)
{
    setAcceptDrops(true);
    ui->setupUi(this);
    ui->listWidget->configure(backend, *this);

    ui->usersButton->setIcon(getUserButtonIcon());
    ui->outgoingPostPanel->composer().init(backend, channel, *ui->outgoingPostPanel,
                                           *ui->listWidget, ui->footerLayout);

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
            ui->usersButton->show();
            ui->usersButton->setText(QString::number(this->channel.members.size())
                                     + QStringLiteral(" members"));
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
    , pinnedPostsDockWidget(nullptr)
    , unreadMessagesCount(0)
    , isThread(true)
    , initialized(false)
    , root_id(std::move(rootId))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    ui->setupUi(this);
    ui->listWidget->configure(backend, *this);

    ui->outgoingPostPanel->composer().init(backend, channel, *ui->outgoingPostPanel,
                                           *ui->listWidget, ui->footerLayout);
    ui->outgoingPostPanel->composer().setRootId(root_id);

    ui->titleLabel->setText(channel.display_name);
    ui->statusLabel->setText(channel.getChannelDescription());
    ui->userAvatar->hide();

    init();

    // Thread follow state is a per-user Mattermost resource. Query it lazily
    // when the thread window opens and use the official PUT/DELETE endpoint.
    if (channel.team && !root_id.isEmpty()) {
        const QString teamId = channel.team->id;
        const QString threadId = root_id;
        auto* followButton = new QPushButton(tr("Follow"), this);
        followButton->setEnabled(false);
        followButton->setProperty("following", false);
        ui->propertieslLayout->insertWidget(0, followButton);

        QPointer<ChatArea> areaGuard(this);
        QPointer<QPushButton> buttonGuard(followButton);
        auto setButtonState = [areaGuard, buttonGuard](bool following) {
            if (!areaGuard || !buttonGuard) {
                return;
            }
            buttonGuard->setProperty("following", following);
            buttonGuard->setText(following ? areaGuard->tr("Unfollow")
                                           : areaGuard->tr("Follow"));
            buttonGuard->setEnabled(true);
        };

        auto& followService = ThreadFollowService::instance(backend);
        connect(&followService, &ThreadFollowService::followingChanged,
                followButton,
                [teamId, threadId, setButtonState](const QString& changedTeamId,
                                                   const QString& changedThreadId,
                                                   bool following) {
            if (changedTeamId == teamId && changedThreadId == threadId) {
                setButtonState(following);
            }
        });

        followService.queryFollowing(teamId, threadId, setButtonState);
        connect(followButton, &QPushButton::clicked, this,
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
    QPointer<ChatArea> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (!guard || !guard->ui || !guard->ui->listWidget
            || !guard->pendingPostId.isEmpty()) {
            return;
        }
        guard->ui->listWidget->scrollToEnd();
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

    // The generic source remains connected while the page is inactive; these
    // connections are only view/orchestration concerns and are rebuilt on
    // activation.
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
            ui->usersButton->setText(QString::number(channel.members.size())
                                     + QStringLiteral(" members"));
        }));
        signalConnections.push_back(connect(&channel, &BackendChannel::onUserRemoved,
                                            this, [this](const BackendUser&) {
            ui->usersButton->setText(QString::number(channel.members.size())
                                     + QStringLiteral(" members"));
        }));

        signalConnections.push_back(connect(ui->listWidget,
                                            &LongListWidget::userViewportChanged,
                                            this, [this](bool atEnd) {
            if (atEnd) {
                markChannelViewedIfAtBottom();
            }
        }));

        signalConnections.push_back(connect(ui->usersButton, &QPushButton::clicked,
                                            this, [this] {
            auto* dialog = new ViewChannelMembersListDialog(backend, channel, this);
            dialog->show();
        }));

        signalConnections.push_back(connect(ui->pinnedPostsButton,
                                            &QPushButton::clicked,
                                            this, [this] {
            if (pinnedPostsDockWidget) {
                delete pinnedPostsDockWidget;
                pinnedPostsDockWidget = nullptr;
                return;
            }

            pinnedPostsDockWidget = new QDockWidget(this);
            pinnedPostsDockWidget->setFloating(true);
            pinnedPostsDockWidget->setFeatures(QDockWidget::DockWidgetMovable);
            auto* pinnedPostsList = new PinnedPostsList(this);
            pinnedPostsDockWidget->setWidget(pinnedPostsList);

            for (BackendPost& post : channel.pinnedPosts) {
                pinnedPostsList->addPost(
                    new PostWidget(backend, post, pinnedPostsList, this, nullptr));
            }

            pinnedPostsDockWidget->setTitleBarWidget(new QWidget());
            pinnedPostsDockWidget->move(
                mapToGlobal(ui->pinnedPostsButton->pos()) + QPoint(0, 40));
            pinnedPostsDockWidget->setFixedWidth(
                geometry().size().width() - ui->pinnedPostsButton->pos().x() - 30);
            pinnedPostsDockWidget->setFixedHeight(300);
            ui->headerLayout->addWidget(pinnedPostsDockWidget);
        }));
    }

    auto& composer = ui->outgoingPostPanel->composer();
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
    scheduleNewestPosition();

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
    if (isThread || channel.pinnedPosts.empty()) {
        ui->pinnedPostsButton->hide();
        return;
    }

    const auto pinnedPostCount = channel.pinnedPosts.size();
    ui->pinnedPostsButton->setText(
        QString::number(static_cast<qulonglong>(pinnedPostCount))
        + (pinnedPostCount == 1 ? QStringLiteral(" pinned post")
                                : QStringLiteral(" pinned posts")));
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
    const bool wasInitialized = initialized;
    init();
    if (wasInitialized) {
        scheduleNewestPosition();
    }
}

void ChatArea::onDeactivate()
{
    if (pinnedPostsDockWidget) {
        delete pinnedPostsDockWidget;
        pinnedPostsDockWidget = nullptr;
    }
    deinit();
}

void ChatArea::onMainWindowActivate()
{
    // Merely restoring/focusing the application is not a reading gesture.
}

void ChatArea::onMove(QPoint)
{
    if (!pinnedPostsDockWidget) {
        return;
    }
    pinnedPostsDockWidget->move(
        mapToGlobal(ui->pinnedPostsButton->pos()) + QPoint(0, 40));
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
    ui->outgoingPostPanel->composer().onDragEnterEvent(event);
}

void ChatArea::dragMoveEvent(QDragMoveEvent* event)
{
    ui->outgoingPostPanel->composer().onDragMoveEvent(event);
}

void ChatArea::dropEvent(QDropEvent* event)
{
    ui->outgoingPostPanel->composer().onDropEvent(event);
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

    if (!ui->listWidget->ensurePostVisible(postId,
                                           LongListWidget::Alignment::Center)) {
        pendingPostId = postId;
        return;
    }

    pendingPostId = postId;
    finishPendingNavigation();
}

} /* namespace Mattermost */
