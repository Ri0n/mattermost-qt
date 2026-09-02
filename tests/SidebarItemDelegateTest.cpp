#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QStandardItemModel>

#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

class SidebarItemDelegateTest : public QObject
{
    Q_OBJECT

private:
    static QImage renderChannel(int channelType, const QString& presence)
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        item->setData(channelType, SidebarItem::ChannelTypeRole);
        item->setData(presence, SidebarItem::PresenceRole);

        QPixmap avatar(24, 24);
        avatar.fill(Qt::black);
        item->setIcon(QIcon(avatar));
        model.appendRow(item);

        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 240, 32);
        option.palette = QApplication::palette();
        option.font = QApplication::font();
        option.state = QStyle::State_Enabled;

        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ChannelItemDelegate delegate;
        delegate.paint(&painter, option, model.index(0, 0));
        painter.end();
        return image;
    }

private slots:
    void ignoresPresenceForNonUserRows()
    {
        const auto publicWithPresence = renderChannel(BackendChannel::publicChannel,
                                                      QStringLiteral("online"));
        const auto publicWithoutPresence = renderChannel(BackendChannel::publicChannel, QString());
        QVERIFY(publicWithPresence == publicWithoutPresence);

        const auto privateWithPresence = renderChannel(BackendChannel::privateChannel,
                                                       QStringLiteral("away"));
        const auto privateWithoutPresence = renderChannel(BackendChannel::privateChannel, QString());
        QVERIFY(privateWithPresence == privateWithoutPresence);

        const auto groupWithPresence = renderChannel(BackendChannel::groupChannel,
                                                     QStringLiteral("dnd"));
        const auto groupWithoutPresence = renderChannel(BackendChannel::groupChannel, QString());
        QVERIFY(groupWithPresence == groupWithoutPresence);
    }

    void rendersPresenceForDirectMessage()
    {
        const auto withPresence = renderChannel(BackendChannel::directChannel,
                                                QStringLiteral("online"));
        const auto withoutPresence = renderChannel(BackendChannel::directChannel, QString());
        QVERIFY(withPresence != withoutPresence);
    }
};

QTEST_MAIN(SidebarItemDelegateTest)

#include "SidebarItemDelegateTest.moc"
