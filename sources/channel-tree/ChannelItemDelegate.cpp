#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ChannelIcons.h"
#include "ChannelTree.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

namespace {

constexpr int ChannelRowHeight = 32;
constexpr int AvatarSize = 24;
constexpr int StatusSize = 8;
constexpr int MuteIconSize = 16;
constexpr int HorizontalMargin = 4;
constexpr int ItemSpacing = 4;

const QColor OnlineColor(QStringLiteral("#3DB887"));
const QColor AwayColor(QStringLiteral("#FFBC1F"));
const QColor DndColor(QStringLiteral("#D24B4E"));

void drawStatusBadge(QPainter* painter, const QRect& rect, const QString& status,
                     const QColor& backgroundColor)
{
    if (status.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // A one-pixel halo keeps the badge readable where it overlaps the avatar.
    painter->setPen(Qt::NoPen);
    painter->setBrush(backgroundColor);
    painter->drawEllipse(QRectF(rect).adjusted(-1.0, -1.0, 1.0, 1.0));

    const QRectF badgeRect(rect);

    if (status == QStringLiteral("online")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(OnlineColor);
        painter->drawEllipse(badgeRect);

        QPen pen(Qt::white);
        pen.setWidthF(1.15);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        QPainterPath check;
        check.moveTo(badgeRect.left() + 1.8, badgeRect.top() + 4.1);
        check.lineTo(badgeRect.left() + 3.3, badgeRect.top() + 5.5);
        check.lineTo(badgeRect.left() + 6.3, badgeRect.top() + 2.4);
        painter->drawPath(check);
    } else if (status == QStringLiteral("away")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(AwayColor);
        painter->drawEllipse(badgeRect);

        QPen pen(Qt::white);
        pen.setWidthF(1.0);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        const QPointF center = badgeRect.center();
        painter->drawLine(center, QPointF(center.x(), badgeRect.top() + 2.0));
        painter->drawLine(center, QPointF(badgeRect.right() - 1.7, center.y() + 1.0));
    } else if (status == QStringLiteral("dnd")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(DndColor);
        painter->drawEllipse(badgeRect);

        QPen pen(Qt::white);
        pen.setWidthF(1.25);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        painter->drawLine(QPointF(badgeRect.left() + 2.0, badgeRect.center().y()),
                          QPointF(badgeRect.right() - 2.0, badgeRect.center().y()));
    } else {
        // Mattermost's offline marker is a dark center with a green outline.
        // Treat unknown server states as offline rather than silently dropping
        // the status indicator.
        QPen pen(OnlineColor);
        pen.setWidthF(1.25);
        painter->setPen(pen);
        painter->setBrush(Qt::black);
        painter->drawEllipse(badgeRect.adjusted(0.65, 0.65, -0.65, -0.65));
    }

    painter->restore();
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
    if (index.data(ChannelTree::ItemKindRole).toInt() == ChannelTree::ChannelItemKind) {
        hint.setHeight(ChannelRowHeight);
    }
    return hint;
}

void ChannelItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (index.data(ChannelTree::ItemKindRole).toInt() != ChannelTree::ChannelItemKind) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem base(option);
    initStyleOption(&base, index);
    const QString text = base.text;
    QIcon icon = base.icon;

    const auto* tree = qobject_cast<const ChannelTree*>(parent());
    Backend* backend = tree ? tree->backendInstance() : nullptr;
    const QString channelId = index.data(ChannelTree::ItemIdRole).toString();
    BackendChannel* channel = backend && !channelId.isEmpty()
        ? backend->getStorage().getChannelById(channelId) : nullptr;

    // Direct rows get an avatar asynchronously. Ordinary channels and group
    // conversations have no server image, so give them stable fallback glyphs
    // without storing generated pixmaps in every duplicated category row.
    if (icon.isNull() && channel) {
        if (channel->type == BackendChannel::groupChannel) {
            icon = ChannelIcons::groupConversation();
        } else if (channel->type == BackendChannel::publicChannel
                   || channel->type == BackendChannel::privateChannel) {
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

        // ItemStatusRole is populated only for direct conversations backed by a
        // real BackendUser. Public/private channels and group conversations
        // never receive this role, so they can never acquire a presence badge.
        const QString status = index.data(ChannelTree::ItemStatusRole).toString();

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
            const QRect statusRect(iconRect.right() - StatusSize + 2,
                                   iconRect.bottom() - StatusSize + 2,
                                   StatusSize,
                                   StatusSize);
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const QColor badgeBackground = selected
                ? option.palette.color(QPalette::Highlight)
                : option.palette.color(QPalette::Base);
            drawStatusBadge(painter, statusRect, status, badgeBackground);
        }

        textLeft = iconRect.right() + 1 + ItemSpacing;
    }

    int textRight = contentRect.right();
    if (index.data(ChannelTree::ItemMutedRole).toBool()) {
        const QRect muteRect(textRight - MuteIconSize + 1,
                             contentRect.center().y() - MuteIconSize / 2,
                             MuteIconSize,
                             MuteIconSize);
        style->standardIcon(QStyle::SP_MediaVolumeMuted).paint(painter, muteRect);
        textRight = muteRect.left() - ItemSpacing;
    }

    QRect textRect(textLeft, contentRect.top(), qMax(0, textRight - textLeft + 1), contentRect.height());
    QFont font = option.font;
    const bool unread = index.data(ChannelTree::ItemUnreadRole).toBool();
    const bool mentioned = index.data(ChannelTree::ItemMentionedRole).toBool();
    font.setBold(unread || mentioned);
    painter->setFont(font);

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool muted = index.data(ChannelTree::ItemMutedRole).toBool();
    const QColor textColor = selected
        ? option.palette.color(QPalette::HighlightedText)
        : (muted ? option.palette.color(QPalette::Disabled, QPalette::Text)
                 : option.palette.color(QPalette::Text));
    painter->setPen(textColor);

    const QString elided = option.fontMetrics.elidedText(text, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

} // namespace Mattermost
