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

    void completesAtMentionWithoutReplacingSurroundingMessage()
    {
        MessageTextEditWidget editor;
        InteractiveTextEdit::CompletionRule rule;
        rule.prefix = QStringLiteral("@");
        rule.provider = [] {
            InteractiveTextEdit::CompletionCandidate alice;
            alice.displayText = QStringLiteral("Alice Example");
            alice.insertText = QStringLiteral("alice");
            alice.detailText = QStringLiteral("@alice");
            alice.filterKeys = QStringList {QStringLiteral("Example")};
            return QVector<InteractiveTextEdit::CompletionCandidate> {alice};
        };
        editor.setCompletionRules({std::move(rule)});
        editor.resize(360, 40);
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QTest::keyClicks(&editor, QStringLiteral("hello @exa"));
        QCoreApplication::processEvents();
        QVERIFY(editor.completionPopupVisible());

        QTest::keyClick(&editor, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(editor.toPlainText(), QStringLiteral("hello @alice "));
    }

    void completionQueryChangesAreDeduplicatedAndCancelled()
    {
        MessageTextEditWidget editor;
        QStringList queries;

        InteractiveTextEdit::CompletionRule rule;
        rule.prefix = QStringLiteral("@");
        rule.provider = [] {
            InteractiveTextEdit::CompletionCandidate alice;
            alice.displayText = QStringLiteral("Alice Example");
            alice.insertText = QStringLiteral("alice");
            alice.detailText = QStringLiteral("@alice");
            return QVector<InteractiveTextEdit::CompletionCandidate> {alice};
        };
        rule.queryChanged = [&queries](const QString& query) {
            queries.push_back(query);
        };
        editor.setCompletionRules({std::move(rule)});
        editor.resize(360, 40);
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QTest::keyClicks(&editor, QStringLiteral("@al"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(queries.count(QStringLiteral("al")), 1);
        QCOMPARE(queries.constLast(), QStringLiteral("al"));

        // An asynchronous provider refresh for the same query must only rebuild
        // the popup; it must not look like another user query and retrigger I/O.
        editor.refreshCompletions();
        editor.refreshCompletions();
        QCoreApplication::processEvents();
        QCOMPARE(queries.count(QStringLiteral("al")), 1);

        QTest::keyClick(&editor, Qt::Key_Space);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QVERIFY(!queries.isEmpty());
        QCOMPARE(queries.constLast(), QString());
    }
};

QTEST_MAIN(MessageTextEditWidgetTest)

#include "MessageTextEditWidgetTest.moc"
