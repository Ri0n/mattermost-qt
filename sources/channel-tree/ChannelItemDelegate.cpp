#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ChannelIcons.h"
#include "ChannelTree.h"
#include "SidebarItem.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "ui/AvatarUtils.h"

namespace Mattermost {

namespace {

constexpr int ChannelRowHeight = 32;
constexpr int AvatarSize = 24;
constexpr int StatusSize = 12;
constexpr int MuteIconSize = 16;
constexpr int HorizontalMargin = 4;
constexpr int ItemSpacing = 4;

int channelType(const QModelIndex& index)
{
    return index.data(SidebarItem::ChannelTypeRole).toInt();
}

} // namespace

ChannelItemDelegate::ChannelItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize ChannelItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
    QSize hint = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(SidebarItem::KindRole).toInt() == SidebarItem::Channel) {
        hint.setHeight(ChannelRowHeight);
    }
    return hint;
}

void ChannelItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (index.data(SidebarItem::KindRole).toInt() != SidebarItem::Channel) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem base(option);
    initStyleOption(&base, index);
    QString text = base.text;
    QIcon icon = base.icon;
    const int type = channelType(index);

    if (type == BackendChannel::groupChannel) {
        auto* tree = qobject_cast<ChannelTree*>(parent());
        Backend* backend = tree ? tree->backendInstance() : nullptr;
        const QString channelId = index.data(SidebarItem::IdRole).toString();
        BackendChannel* channel = backend
            ? backend->getStorage().getChannelById(channelId) : nullptr;

        if (channel) {
            if (!channel->display_name.isEmpty()) {
                text = channel->display_name;
            }

            if (!resolvedGroupChannels.contains(channelId)) {
                resolvedGroupChannels.insert(channelId);

                // The row delegate is already the point where we know a group
                // DM is actually visible. Resolve its tiny member set here rather
                // than reintroducing an eager global user-directory preload.
                // Keep rendering tied to BackendChannel::display_name so the same
                // value is also consumed by ChatArea's existing onUpdated path.
                QPointer<ChannelTree> treeGuard(tree);
                QObject::connect(channel, &BackendChannel::onUpdated,
                                 const_cast<ChannelItemDelegate*>(this),
                                 [treeGuard] {
                    if (treeGuard && treeGuard->viewport()) {
                        treeGuard->viewport()->update();
                    }
                });
                UserProfileService::instance(*backend).ensureGroupChannelMembers(*channel);
            }
        }
    }

    if (icon.isNull()) {
        if (type == BackendChannel::groupChannel) {
            icon = ChannelIcons::groupConversation();
        } else if (type == BackendChannel::publicChannel
                   || type == BackendChannel::privateChannel) {
            icon = ChannelIcons::channel();
        }
    }

    base.text.clear();
    base.icon = QIcon();

    const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &base, painter, option.widget);

    QRect contentRect = option.rect.adjusted(HorizontalMargin, 0, -HorizontalMargin, 0);
    int textLeft = contentRect.left();

    if (!icon.isNull()) {
        const QRect iconRect(textLeft,
                             contentRect.center().y() - AvatarSize / 2,
                             AvatarSize,
                             AvatarSize);
        const QPixmap pixmap = icon.pixmap(AvatarSize, AvatarSize);

        const QString status = type == BackendChannel::directChannel
            ? index.data(SidebarItem::PresenceRole).toString()
            : QString();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (!status.isEmpty()) {
            QPainterPath clip;
            clip.addEllipse(iconRect);
            painter->setClipPath(clip);
        }
        painter->drawPixmap(iconRect, pixmap);
        painter->restore();

        if (!status.isEmpty()) {
            const QRect statusRect(iconRect.right() - StatusSize + 3,
                                   iconRect.bottom() - StatusSize + 3,
                                   StatusSize,
                                   StatusSize);
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const QColor badgeBackground = selected
                ? option.palette.color(QPalette::Highlight)
                : option.palette.color(QPalette::Base);
            AvatarUtils::drawStatusBadge(painter, statusRect, status, badgeBackground);
        }

        textLeft = iconRect.right() + 1 + ItemSpacing;
    }

    int textRight = contentRect.right();
    if (index.data(SidebarItem::MutedRole).toBool()) {
        const QRect muteRect(textRight - MuteIconSize + 1,
                             contentRect.center().y() - MuteIconSize / 2,
                             MuteIconSize,
                             MuteIconSize);
        style->standardIcon(QStyle::SP_MediaVolumeMuted).paint(painter, muteRect);
        textRight = muteRect.left() - ItemSpacing;
    }

    QRect textRect(textLeft, contentRect.top(), qMax(0, textRight - textLeft + 1), contentRect.height());
    QFont font = option.font;
    const bool unread = index.data(SidebarItem::UnreadRole).toBool();
    const bool mentioned = index.data(SidebarItem::MentionedRole).toBool();
    font.setBold(unread || mentioned);
    painter->setFont(font);

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool muted = index.data(SidebarItem::MutedRole).toBool();
    const QColor textColor = selected
        ? option.palette.color(QPalette::HighlightedText)
        : (muted ? option.palette.color(QPalette::Disabled, QPalette::Text)
                 : option.palette.color(QPalette::Text));
    painter->setPen(textColor);

    const QString elided = option.fontMetrics.elidedText(text, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

} // namespace Mattermost
