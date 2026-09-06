#include <QtTest>

#include <QCoreApplication>

#include "chat-area/outgoing-post/MessageTextEditWidget.h"

using namespace Mattermost;

class MessageTextEditWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void growsAndShrinksWithExplicitLines()
    {
        MessageTextEditWidget editor;
        editor.resize(320, 40);
        editor.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        editor.setPlainText(QStringLiteral("first"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const int oneLineHeight = editor.height();
        QVERIFY(oneLineHeight > 0);

        editor.setPlainText(QStringLiteral("first\nsecond"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const int twoLineHeight = editor.height();
        QVERIFY2(twoLineHeight > oneLineHeight,
                 "Adding an explicit second line must increase composer height");

        editor.setPlainText(QStringLiteral("first"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(editor.height(), oneLineHeight);
    }

    void capsGrowthAtMaximumComposerHeight()
    {
        MessageTextEditWidget editor;
        editor.resize(320, 40);
        editor.show();
        QCoreApplication::processEvents();

        QStringList lines;
        for (int i = 0; i < 100; ++i) {
            lines.push_back(QStringLiteral("line %1").arg(i));
        }
        editor.setPlainText(lines.join(QLatin1Char('\n')));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(editor.height(), 300);
    }
};

QTEST_MAIN(MessageTextEditWidgetTest)

#include "MessageTextEditWidgetTest.moc"
