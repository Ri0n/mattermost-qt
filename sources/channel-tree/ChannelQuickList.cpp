/**
 * @file ChannelQuickList.cpp
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#include "ChannelQuickList.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QShowEvent>
#include <QTabWidget>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/FollowingNavigation.h"
#include "channel-tree/SidebarItem.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int FollowingKeyRole = Qt::UserRole + 100;
constexpr int FollowingSortTimeRole = Qt::UserRole + 101;
constexpr int ThreadSnippetLength = 120;

QString entryKey(const FollowingModel::Entry& entry)
{
    return entry.isThread()
        ? QStringLiteral("t:") + entry.threadId
        : QStringLiteral("c:") + entry.channelId;
}

QString compactMessage(QString message)
{
    message = message.simplified();
    if (message.size() > ThreadSnippetLength) {
        message.truncate(ThreadSnippetLength - 1);
        message += QChar(0x2026);
    }
    return message;
}

} // namespace

ChannelQuickList::ChannelQuickList(QWidget* parent)
    : QTreeWidget(parent)
{
    setColumnCount(1);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing_ || retainedKey_.isEmpty()) {
            return;
        }
        const QString currentKey = current
            ? current->data(0, FollowingKeyRole).toString() : QString();
        if (currentKey != retainedKey_) {
            releaseSelectionRetention();
        }
    });

    connect(this, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item) {
            return;
        }
        const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
        if (!channelId.isEmpty()) {
            emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos));
        }
    });
}

void ChannelQuickList::initialize(Backend& backend, Mode)
{
    backend_ = &backend;
    model_ = &FollowingModel::instance(backend);

    if (auto* tabs = qobject_cast<QTabWidget*>(parentWidget())) {
        const int index = tabs->indexOf(this);
        if (index >= 0) {
            tabs->setTabText(index, tr("Following"));
            tabs->setTabToolTip(index,
                                tr("Unread direct messages and followed threads"));
        }
        connect(tabs, &QTabWidget::currentChanged, this,
                [this, tabs](int currentIndex) {
            if (tabs->widget(currentIndex) != this) {
                releaseSelectionRetention();
            }
        });
    }

    connect(model_, &FollowingModel::changed,
            this, &ChannelQuickList::refresh);
    refresh();
    model_->ensureThreadsFresh();
}

void ChannelQuickList::keyPressEvent(QKeyEvent* event)
{
    if (event && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        activateItem(currentItem());
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void ChannelQuickList::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* pressedItem = nullptr;
    if (event && event->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        pressedItem = itemAt(event->position().toPoint());
#else
        pressedItem = itemAt(event->pos());
#endif
    }

    QTreeWidget::mousePressEvent(event);
    if (pressedItem) {
        activateItem(pressedItem);
    }
}

void ChannelQuickList::showEvent(QShowEvent* event)
{
    QTreeWidget::showEvent(event);
    if (model_) {
        model_->ensureThreadsFresh();
    }
}

void ChannelQuickList::refreshThreads()
{
    if (model_) {
        model_->ensureThreadsFresh();
    }
}

void ChannelQuickList::activateItem(QTreeWidgetItem* current)
{
    if (refreshing_ || !current || !backend_ || !model_) {
        return;
    }

    const QString key = current->data(0, FollowingKeyRole).toString();
    retainedKey_ = key;
    retainedSortTime_ = current->data(0, FollowingSortTimeRole).toULongLong();
    retainedUnreadPosition_ = current->data(0, SidebarItem::UnreadRole).toBool();

    const QString channelId = current->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = current->data(0, SidebarItem::ThreadIdRole).toString();
    const FollowingModel::Entry* modelEntry = model_->findEntry(channelId, threadId);
    if (modelEntry) {
        retainedEntry_ = *modelEntry;
    }

    const FollowingModel::Entry* entry = modelEntry;
    if (!entry && retainedEntry_
        && retainedEntry_->channelId == channelId
        && retainedEntry_->threadId == threadId) {
        entry = &*retainedEntry_;
    }
    if (!entry) {
        return;
    }

    if (entry->isThread()) {
        openThread(*entry);
        return;
    }

    BackendChannel* channel = backend_->getStorage().getChannelById(channelId);
    if (!channel) {
        emit channelSelected(channelId);
        return;
    }

    if (entry->resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry->firstUnreadPostId.isEmpty()) {
        AppNavigationService::instance(*backend_).openPost(entry->firstUnreadPostId);
        return;
    }
    if (entry->resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    if (backend_->getCurrentChannel() == channel && !entry->requiresAttention()) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    QPointer<ChannelQuickList> guard(this);
    const QString requestedKey = key;
    backend_->retrieveChannelUnreadPost(
        *channel, [guard, channelId, requestedKey](const QString& postId) {
            if (!guard || !guard->backend_ || !guard->model_
                || guard->retainedKey_ != requestedKey) {
                return;
            }

            const FollowingModel::Entry* current = guard->model_->findEntry(channelId);
            if (!current || current->resumeState == FollowingModel::ResumeState::AtEnd) {
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
                return;
            }
            if (current->resumeState == FollowingModel::ResumeState::FirstUnread
                && !current->firstUnreadPostId.isEmpty()) {
                AppNavigationService::instance(*guard->backend_).openPost(
                    current->firstUnreadPostId);
                return;
            }

            BackendChannel* currentChannel =
                guard->backend_->getStorage().getChannelById(channelId);
            if (!postId.isEmpty() && currentChannel
                && !isStaleConversationResumeTarget(*current, *currentChannel, postId)) {
                AppNavigationService::instance(*guard->backend_).openPost(postId);
            } else {
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
            }
        });
}

void ChannelQuickList::releaseSelectionRetention()
{
    if (retainedKey_.isEmpty() && !retainedEntry_) {
        return;
    }

    retainedKey_.clear();
    retainedEntry_.reset();
    retainedSortTime_ = 0;
    retainedUnreadPosition_ = false;
    QTimer::singleShot(0, this, [this] {
        if (!refreshing_) {
            refresh();
        }
    });
}

QString ChannelQuickList::threadLabel(const FollowingModel::Entry& entry) const
{
    if (!backend_) {
        return compactMessage(entry.message);
    }

    const BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
    const QString channelName = channel ? channel->display_name : entry.channelId;
    const QString snippet = compactMessage(entry.message);
    const QString prefix = entry.synthetic || entry.unreadMentions > 0
        ? QStringLiteral("@ ") : QStringLiteral("\u21aa ");

    return snippet.isEmpty()
        ? prefix + channelName
        : prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void ChannelQuickList::openThread(const FollowingModel::Entry& entry)
{
    if (!backend_ || !model_ || entry.threadId.isEmpty() || entry.channelId.isEmpty()) {
        return;
    }

    if (entry.synthetic) {
        model_->ensureThreadsFresh();
        AppNavigationService::instance(*backend_).openPost(entry.threadId);
        return;
    }

    if (entry.resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openThread(entry.channelId, entry.threadId);
        return;
    }

    uint64_t resumeAfter = entry.lastViewedAt;
    if (entry.hasLocalProgress()) {
        resumeAfter = entry.readThroughCreateAt;
    }
    const QString fallbackPostId =
        entry.resumeState == FollowingModel::ResumeState::FirstUnread
        ? entry.firstUnreadPostId : QString();

    // Navigation itself is not a read acknowledgement. The thread ChatArea
    // marks it read only when the user has actually reached the newest edge;
    // oversized targets therefore remain unread until their lower edge is seen.
    AppNavigationService::instance(*backend_).openThreadAtLastViewed(
        entry.channelId, entry.threadId, resumeAfter, fallbackPostId, {}, false);
}

void ChannelQuickList::refresh()
{
    if (!backend_ || !model_) {
        return;
    }

    struct Candidate {
        FollowingModel::Entry entry;
        QString key;
        uint64_t sortTime = 0;
        bool sortAsUnread = false;
    };

    QVector<Candidate> candidates;
    const uint64_t fallbackNow = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    for (const FollowingModel::Entry& entry : model_->entries()) {
        if (!entry.isThread() && entry.muted) {
            continue;
        }
        if (entry.isThread() && !backend_->getStorage().getChannelById(entry.channelId)) {
            continue;
        }

        Candidate candidate;
        candidate.entry = entry;
        candidate.key = entryKey(entry);
        const bool retainedUnread = candidate.key == retainedKey_
            && retainedUnreadPosition_;
        candidate.sortAsUnread = entry.requiresAttention() || retainedUnread;
        if (candidate.sortAsUnread) {
            candidate.sortTime = retainedUnread && retainedSortTime_ != 0
                ? retainedSortTime_
                : (entry.attentionSince != 0 ? entry.attentionSince
                                             : (entry.lastReplyAt != 0
                                                    ? entry.lastReplyAt : fallbackNow));
        } else {
            candidate.sortTime = entry.lastReplyAt;
        }
        candidates.push_back(std::move(candidate));
    }

    // A read DM/GM disappears from the shared model immediately. Keep only the
    // selected row in this view so an action never removes the item under the
    // pointer; this retention must not prolong its cursor/model lifetime.
    if (retainedEntry_ && !retainedEntry_->isThread()) {
        const QString key = entryKey(*retainedEntry_);
        bool present = false;
        for (const Candidate& candidate : std::as_const(candidates)) {
            if (candidate.key == key) {
                present = true;
                break;
            }
        }

        BackendChannel* channel = backend_->getStorage().getChannelById(
            retainedEntry_->channelId);
        const bool muted = channel
            ? SidebarService::instance(*backend_).isChannelMuted(*channel)
            : true;
        if (!present && channel && !muted) {
            Candidate candidate;
            candidate.entry = *retainedEntry_;
            candidate.entry.unread = false;
            candidate.entry.mentioned = false;
            candidate.entry.unreadReplies = 0;
            candidate.entry.unreadMentions = 0;
            candidate.entry.attentionSince = 0;
            candidate.key = key;
            candidate.sortAsUnread = retainedUnreadPosition_;
            candidate.sortTime = retainedSortTime_ != 0
                ? retainedSortTime_ : candidate.entry.lastReplyAt;
            candidates.push_back(std::move(candidate));
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                        const Candidate& rhs) {
        if (lhs.sortAsUnread != rhs.sortAsUnread) {
            return lhs.sortAsUnread;
        }
        if (lhs.sortTime != rhs.sortTime) {
            return lhs.sortTime > rhs.sortTime;
        }
        return lhs.key < rhs.key;
    });

    const QString selectedKey = currentItem()
        ? currentItem()->data(0, FollowingKeyRole).toString() : QString();

    refreshing_ = true;
    clear();
    channelItems_.clear();
    QTreeWidgetItem* itemToRestore = nullptr;

    for (const Candidate& candidate : std::as_const(candidates)) {
        const FollowingModel::Entry& entry = candidate.entry;
        auto* item = new QTreeWidgetItem(this);
        item->setData(0, FollowingKeyRole, candidate.key);
        item->setData(0, FollowingSortTimeRole,
                      QVariant::fromValue<qulonglong>(candidate.sortTime));
        item->setData(0, SidebarItem::UnreadRole, entry.requiresAttention());
        item->setData(0, SidebarItem::MentionedRole,
                      entry.mentioned || entry.unreadMentions > 0);
        item->setData(0, SidebarItem::ChannelIdRole, entry.channelId);
        item->setData(0, SidebarItem::ThreadIdRole, entry.threadId);
        item->setData(0, SidebarItem::TeamIdRole, entry.teamId);
        item->setData(0, SidebarItem::MutedRole, entry.muted);

        if (!entry.isThread()) {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
            if (!channel) {
                delete item;
                continue;
            }

            item->setText(0, channel->display_name);
            item->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            item->setData(0, SidebarItem::IdRole, channel->id);
            item->setData(0, SidebarItem::ChannelTypeRole, channel->type);
            item->setToolTip(0, channel->getTeamAndChannelName());

            if (channel->type == BackendChannel::directChannel) {
                BackendUser* user = backend_->getStorage().getUserById(channel->name);
                if (user) {
                    if (!user->avatar.isNull()) {
                        item->setIcon(0, QIcon(user->avatar));
                    }
                    item->setData(0, SidebarItem::PresenceRole, user->status);
                    ensureDirectUserConnections(*channel);
                }
            } else {
                item->setIcon(0, ChannelIcons::groupConversation());
            }
            channelItems_.insert(channel->id, item);
        } else {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
            item->setText(0, threadLabel(entry));
            item->setData(0, SidebarItem::KindRole, SidebarItem::Thread);
            item->setData(0, SidebarItem::IdRole, entry.threadId);
            item->setData(0, SidebarItem::ChannelTypeRole,
                          channel ? channel->type : BackendChannel::publicChannel);
            item->setToolTip(0, entry.message);
            if (channel && channel->type == BackendChannel::privateChannel) {
                item->setIcon(0, ChannelIcons::privateChannel());
            } else {
                item->setIcon(0, ChannelIcons::channel());
            }
        }

        if (candidate.key == selectedKey) {
            itemToRestore = item;
        }
    }

    if (itemToRestore) {
        setCurrentItem(itemToRestore);
    } else {
        setCurrentItem(nullptr);
        clearSelection();
    }
    refreshing_ = false;
}

void ChannelQuickList::ensureDirectUserConnections(BackendChannel& channel)
{
    if (!backend_ || channel.type != BackendChannel::directChannel) {
        return;
    }

    BackendUser* user = backend_->getStorage().getUserById(channel.name);
    if (!user || connectedUsers_.contains(user->id)) {
        return;
    }

    connectedUsers_.insert(user->id);
    connect(user, &BackendUser::onStatusChanged, this, [this, user] {
        updateDirectUser(*user);
    });
    connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
        updateDirectUser(*user);
    });
}

void ChannelQuickList::updateDirectUser(const BackendUser& user)
{
    if (!backend_) {
        return;
    }

    BackendChannel* channel = backend_->getStorage().getDirectChannelByUserId(user.id);
    if (!channel) {
        return;
    }

    QTreeWidgetItem* item = channelItems_.value(channel->id, nullptr);
    if (!item) {
        return;
    }

    item->setData(0, SidebarItem::PresenceRole, user.status);
    if (!user.avatar.isNull()) {
        item->setIcon(0, QIcon(user.avatar));
    }
    viewport()->update();
}

} // namespace Mattermost
