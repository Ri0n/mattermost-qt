#include <QtTest>

#include "choose-emoji-dialog/EmojiDialogSupport.h"

class EmojiDialogSupportTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesMattermostStyleQueries()
    {
        using Mattermost::EmojiDialogSupport::normalizeSearchTerm;

        QCOMPARE(normalizeSearchTerm(QStringLiteral(" :Rolling-On The-Floor: ")),
                 QStringLiteral("rolling_on_the_floor"));
        QCOMPARE(normalizeSearchTerm(QStringLiteral("pizza")),
                 QStringLiteral("pizza"));
    }

    void matchesCanonicalEmojiNames()
    {
        using Mattermost::EmojiDialogSupport::matchesSearch;

        QVERIFY(matchesSearch(QStringLiteral("rolling_on_the_floor_laughing"),
                              QStringLiteral("rolling floor")));
        QVERIFY(matchesSearch(QStringLiteral("face_vomiting"),
                              QStringLiteral(":vomit:")));
        QVERIFY(!matchesSearch(QStringLiteral("floor_rolling"),
                               QStringLiteral("rolling floor")));
        QVERIFY(!matchesSearch(QStringLiteral("pizza"),
                               QStringLiteral("cherry")));
    }

    void choosesPlatformLegacyEmojiFonts()
    {
        using Mattermost::EmojiDialogSupport::Platform;
        using Mattermost::EmojiDialogSupport::chooseLegacyEmojiFontFamily;

        const QStringList installed = {
            QStringLiteral("Arial"),
            QStringLiteral("Noto Color Emoji"),
            QStringLiteral("Segoe UI Emoji"),
            QStringLiteral("Apple Color Emoji"),
        };

        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Linux),
                 QStringLiteral("Noto Color Emoji"));
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Windows),
                 QStringLiteral("Segoe UI Emoji"));
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::MacOS),
                 QStringLiteral("Apple Color Emoji"));
        QVERIFY(chooseLegacyEmojiFontFamily({QStringLiteral("Arial")},
                                            Platform::Linux).isEmpty());
    }

    void preservesFoundryQualifiedFamilyName()
    {
        using Mattermost::EmojiDialogSupport::Platform;
        using Mattermost::EmojiDialogSupport::chooseLegacyEmojiFontFamily;

        const QStringList installed = {
            QStringLiteral("Noto Color Emoji [Google]"),
        };
        QCOMPARE(chooseLegacyEmojiFontFamily(installed, Platform::Linux),
                 QStringLiteral("Noto Color Emoji [Google]"));
    }
};

QTEST_MAIN(EmojiDialogSupportTest)

#include "EmojiDialogSupportTest.moc"
