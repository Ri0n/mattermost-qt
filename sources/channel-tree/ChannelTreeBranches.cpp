#include "ChannelTree.h"

#include <QAbstractItemModel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

namespace Mattermost {

void ChannelTree::drawBranches(QPainter* painter, const QRect& rect,
                               const QModelIndex& index) const
{
    if (!painter || !index.isValid() || !model()->hasChildren(index)) {
        return;
    }

    QStyleOption option;
    option.initFrom(this);

    // Do not use PE_IndicatorBranch here. Even without State_Item/State_Sibling,
    // some native styles still draw a short vertical stem around the disclosure
    // marker. Draw only the native arrow primitive instead, so category
    // expand/collapse remains styled by Qt while branch connector lines cannot
    // appear at all.
    const int extent = qMin(indentation(), rect.height());
    option.rect = QRect(0, 0, extent, extent);
    option.rect.moveCenter(rect.center());
    if (layoutDirection() == Qt::LeftToRight) {
        option.rect.moveLeft(rect.right() - extent + 1);
    } else {
        option.rect.moveRight(rect.left() + extent - 1);
    }

    style()->drawPrimitive(isExpanded(index)
                               ? QStyle::PE_IndicatorArrowDown
                               : QStyle::PE_IndicatorArrowRight,
                           &option, painter, this);
}

} // namespace Mattermost
