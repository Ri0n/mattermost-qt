#include <QtTest>

#include "chat-area/QuotedReplyFormat.h"

using namespace Mattermost;

class QuotedReplyFormatTest final : public QObject
{
    Q_OBJECT

private slots:
    void buildsOfficialClientFallback()
    {
        const QString prefix = QuotedReplyFormat::buildFallback(
            QStringLiteral("post-id"), QStringLiteral("Alice"),
            QStringLiteral("first line\nsecond line"), false);

        QVERIFY(prefix.startsWith(
            QStringLiteral("> [Replying to Alice](/_redirect/pl/post-id)\n> ")));
        QVERIFY(prefix.contains(QStringLiteral("first line second line")));
        QVERIFY(prefix.endsWith(QStringLiteral("\n>\n\n")));
    }

    void stripsOnlyOurGeneratedFallback()
    {
        const QString prefix = QuotedReplyFormat::buildFallback(
            QStringLiteral("post-id"), QStringLiteral("Alice"),
            QStringLiteral("quoted"), false);
        const QString body = QStringLiteral("my answer");

        QCOMPARE(QuotedReplyFormat::stripFallback(prefix + body), body);
        QCOMPARE(QuotedReplyFormat::fallbackPrefix(prefix + body), prefix);

        const QString userBlockquote = QStringLiteral("> ordinary quote\n\nanswer");
        QCOMPARE(QuotedReplyFormat::stripFallback(userBlockquote), userBlockquote);
    }

    void compactsLongQuotedText()
    {
        const QString longText(400, QLatin1Char('x'));
        const QString compact = QuotedReplyFormat::compactText(longText, false, 40);
        QCOMPARE(compact.size(), 40);
        QVERIFY(compact.endsWith(QChar(0x2026)));
    }
};

QTEST_MAIN(QuotedReplyFormatTest)
#include "QuotedReplyFormatTest.moc"
