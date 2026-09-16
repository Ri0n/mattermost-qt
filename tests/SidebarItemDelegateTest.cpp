#include <QtTest>

#include <QImage>
#include <QListView>
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
    static QImage renderItem(SidebarItem::Kind kind, int channelType, const QString& presence,
                             bool unread = false)
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(kind, SidebarItem::KindRole);
        item->setData(channelType, SidebarItem::ChannelTypeRole);
        item->setData(presence, SidebarItem::PresenceRole);
        item->setData(unread, SidebarItem::UnreadRole);

        QPixmap avatar(24, 24);
        avatar.fill(Qt::black);
        item->setIcon(QIcon(avatar));
        model.appendRow(item);

        // A styled item delegate is normally invoked by an item view. Keep the
        // unit test on that real contract as well: some platform styles expect
        // option.widget to be a valid view while painting CE_ItemViewItem.
        QListView view;
        view.setModel(&model);

        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.widget = &view;
        option.rect = QRect(0, 0, 240, 32);
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
        const auto publicWithPresence = renderItem(SidebarItem::Channel,
                                                   BackendChannel::publicChannel,
                                                   QStringLiteral("online"));
        const auto publicWithoutPresence = renderItem(SidebarItem::Channel,
                                                      BackendChannel::publicChannel,
                                                      QString());
        QVERIFY(publicWithPresence == publicWithoutPresence);

        const auto privateWithPresence = renderItem(SidebarItem::Channel,
                                                    BackendChannel::privateChannel,
                                                    QStringLiteral("away"));
        const auto privateWithoutPresence = renderItem(SidebarItem::Channel,
                                                       BackendChannel::privateChannel,
                                                       QString());
        QVERIFY(privateWithPresence == privateWithoutPresence);

        const auto groupWithPresence = renderItem(SidebarItem::Channel,
                                                  BackendChannel::groupChannel,
                                                  QStringLiteral("dnd"));
        const auto groupWithoutPresence = renderItem(SidebarItem::Channel,
                                                     BackendChannel::groupChannel,
                                                     QString());
        QVERIFY(groupWithPresence == groupWithoutPresence);
    }

    void unreadRoleMakesConversationVisuallyBold()
    {
        const auto read = renderItem(SidebarItem::Channel,
                                     BackendChannel::publicChannel, QString(), false);
        const auto unread = renderItem(SidebarItem::Channel,
                                       BackendChannel::publicChannel, QString(), true);
        QVERIFY(read != unread);
    }

    void rendersPresenceForDirectMessage()
    {
        const auto withPresence = renderItem(SidebarItem::Channel,
                                             BackendChannel::directChannel,
                                             QStringLiteral("online"));
        const auto withoutPresence = renderItem(SidebarItem::Channel,
                                                BackendChannel::directChannel,
                                                QString());
        QVERIFY(withPresence != withoutPresence);
    }

    void rendersVirtualDestinationAsConversationRow()
    {
        const auto channel = renderItem(SidebarItem::Channel,
                                        BackendChannel::directChannel,
                                        QString());
        const auto destination = renderItem(SidebarItem::VirtualDestination,
                                            BackendChannel::directChannel,
                                            QString());
        QCOMPARE(destination, channel);
    }

    void rendersVirtualDirectDestinationWithSamePresenceChrome()
    {
        const auto channel = renderItem(SidebarItem::Channel,
                                        BackendChannel::directChannel,
                                        QStringLiteral("online"));
        const auto destination = renderItem(SidebarItem::VirtualDestination,
                                            BackendChannel::directChannel,
                                            QStringLiteral("online"));
        QCOMPARE(destination, channel);
    }
};

QTEST_MAIN(SidebarItemDelegateTest)

#include "SidebarItemDelegateTest.moc"
