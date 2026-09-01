#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ChannelTree.h"

namespace Mattermost {

namespace {

constexpr int ChannelRowHeight = 32;
constexpr int AvatarSize = 24;
constexpr int StatusSize = 8;
constexpr int MuteIconSize = 16;
constexpr int HorizontalMargin = 4;
constexpr int ItemSpacing = 4;

QColor statusColor(const QString& status)
{
    if (status == QStringLiteral("online")) {
        return QColor(QStringLiteral("#3DB887"));
    }
    if (status == QStringLiteral("away")) {
        return QColor(QStringLiteral("#FFBC1F"));
    }
    if (status == QStringLiteral("dnd")) {
        return QColor(QStringLiteral("#D24B4E"));
    }
    return QColor(QStringLiteral("#8E9AA5"));
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
    const QIcon icon = base.icon;
    base.text.clear();
    base.icon = QIcon();

    const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &base, painter, option.widget);

    QRect contentRect = option.rect.adjusted(HorizontalMargin, 0, -HorizontalMargin, 0);
    int textLeft = contentRect.left();

    if (!icon.isNull()) {
        const QRect avatarRect(textLeft,
                               contentRect.center().y() - AvatarSize / 2,
                               AvatarSize,
                               AvatarSize);
        const QPixmap avatar = icon.pixmap(AvatarSize, AvatarSize);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addEllipse(avatarRect);
        painter->setClipPath(clip);
        painter->drawPixmap(avatarRect, avatar);
        painter->restore();

        const QString status = index.data(ChannelTree::ItemStatusRole).toString();
        if (!status.isEmpty()) {
            const QRect statusRect(avatarRect.right() - StatusSize + 2,
                                   avatarRect.bottom() - StatusSize + 2,
                                   StatusSize,
                                   StatusSize);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(option.palette.color(QPalette::Base), 2));
            painter->setBrush(statusColor(status));
            painter->drawEllipse(statusRect);
            painter->restore();
        }

        textLeft = avatarRect.right() + 1 + ItemSpacing;
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
    font.setBold(index.data(ChannelTree::ItemMentionedRole).toBool());
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
