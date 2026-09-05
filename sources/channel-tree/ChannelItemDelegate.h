#pragma once

#include <QSet>
#include <QStyledItemDelegate>

namespace Mattermost {

class ChannelItemDelegate final : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChannelItemDelegate(QObject* parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    mutable QSet<QString> requestedGroupChannels;
};

} // namespace Mattermost
