#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QSplitter>
#include <QWidget>

#include "navigation/ThreadPaneLayout.h"

using namespace Mattermost;

class ThreadPaneLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void collapsedPaneExpandsAfterShow()
    {
        QSplitter splitter(Qt::Horizontal);
        splitter.setChildrenCollapsible(true);
        splitter.resize(900, 600);

        auto* mainPane = new QWidget;
        auto* threadPane = new QWidget;
        threadPane->setMinimumWidth(280);
        splitter.addWidget(mainPane);
        splitter.addWidget(threadPane);
        splitter.show();
        QCoreApplication::processEvents();

        splitter.setSizes({900, 0});
        QCoreApplication::processEvents();
        QCOMPARE(splitter.sizes().at(1), 0);

        // Navigation presents the previously hidden thread surface before it
        // inspects QSplitter geometry. A persisted zero-width pane must not stay
        // invisible after that explicit presentation request.
        threadPane->hide();
        QCoreApplication::processEvents();
        threadPane->show();
        QCoreApplication::processEvents();

        ensureThreadPaneExpanded(splitter);
        QCoreApplication::processEvents();

        const QList<int> sizes = splitter.sizes();
        QCOMPARE(sizes.size(), 2);
        QVERIFY(sizes.at(1) > 0);
    }

    void visiblePaneKeepsUserWidth()
    {
        QSplitter splitter(Qt::Horizontal);
        splitter.resize(900, 600);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        splitter.show();
        QCoreApplication::processEvents();

        splitter.setSizes({620, 280});
        QCoreApplication::processEvents();
        const QList<int> before = splitter.sizes();
        QVERIFY(before.at(1) > 0);

        QVERIFY(!ensureThreadPaneExpanded(splitter));
        QCOMPARE(splitter.sizes(), before);
    }

    void forceDefaultOverridesInitialGeometry()
    {
        QSplitter splitter(Qt::Horizontal);
        splitter.resize(1200, 600);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        splitter.show();
        QCoreApplication::processEvents();

        splitter.setSizes({1000, 200});
        QCoreApplication::processEvents();
        QVERIFY(ensureThreadPaneExpanded(splitter, true));

        const QList<int> sizes = splitter.sizes();
        QVERIFY(sizes.at(0) > sizes.at(1));
        QVERIFY(sizes.at(1) >= 320);
    }
};

QTEST_MAIN(ThreadPaneLayoutTest)
#include "ThreadPaneLayoutTest.moc"
