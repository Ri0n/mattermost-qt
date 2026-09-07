#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ChannelIcons.h"
#include "SidebarItem.h"
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

bool isConversationRow(const QModelIndex& index)
{
    const int kind = index.data(SidebarItem::KindRole).toInt();
    return kind == SidebarItem::Channel || kind == SidebarItem::VirtualDestination;
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
    if (isConversationRow(index)) {
        hint.setHeight(ChannelRowHeight);
    }
    return hint;
}

void ChannelItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (!isConversationRow(index)) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem base(option);
    initStyleOption(&base, index);
    const QString text = base.text;
    QIcon icon = base.icon;
    const int type = channelType(index);

    if (type == BackendChannel::groupChannel) {
        const QString channelId = index.data(SidebarItem::IdRole).toString();
        if (!channelId.isEmpty() && !requestedGroupChannels.contains(channelId)) {
            requestedGroupChannels.insert(channelId);

            // Painting must stay independent from Backend/Storage services: the
            // delegate is also built in isolation by the rendering unit test.
            // Ask the owning ChannelTree to resolve this visible group-DM title
            // asynchronously through its meta-object instead of performing
            // network/storage work directly from paint().
            if (QObject* owner = parent()) {
                QMetaObject::invokeMethod(owner,
                                          "ensureGroupChannelDisplayName",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, channelId));
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
