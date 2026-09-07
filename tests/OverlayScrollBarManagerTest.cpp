#include <QtTest>

#include <QApplication>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include "ui/OverlayScrollBarManager.h"

using namespace Mattermost;

class OverlayScrollBarManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void overlaysViewportWithoutChangingScrollModel()
    {
        OverlayScrollBarManager::install(*qApp);

        QScrollArea area;
        auto* content = new QWidget;
        content->resize(600, 1200);
        area.setWidget(content);
        area.resize(320, 240);
        area.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QScrollBar* source = area.verticalScrollBar();
        QVERIFY(source->maximum() > source->minimum());
        QCOMPARE(area.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

        auto* overlay = area.findChild<QScrollBar*>(
            QStringLiteral("mattermostOverlayVerticalScrollBar"));
        QVERIFY(overlay);
        QCOMPARE(overlay->minimum(), source->minimum());
        QCOMPARE(overlay->maximum(), source->maximum());
        QCOMPARE(overlay->pageStep(), source->pageStep());

        source->setValue(std::min(source->maximum(), source->minimum() + 20));
        QCoreApplication::processEvents();
        QVERIFY(overlay->isVisible());
        QCOMPARE(overlay->value(), source->value());

        const QRect viewportRect = area.viewport()->geometry();
        QVERIFY(viewportRect.contains(overlay->geometry().center()));
        QVERIFY(overlay->geometry().right() <= viewportRect.right());
        QVERIFY(overlay->palette().color(QPalette::Mid).alpha() < 255);

        const int target = std::min(source->maximum(), source->value() + 20);
        overlay->setValue(target);
        QCOMPARE(source->value(), target);
    }
};

QTEST_MAIN(OverlayScrollBarManagerTest)

#include "OverlayScrollBarManagerTest.moc"
