#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace Mattermost::EmojiDialogSupport {

enum class Platform {
    Linux,
    Windows,
    MacOS,
    Other,
};

inline Platform currentPlatform()
{
#if defined(Q_OS_WIN)
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#elif defined(Q_OS_LINUX)
    return Platform::Linux;
#else
    return Platform::Other;
#endif
}

inline QString normalizeSearchTerm(QString term)
{
    term = term.trimmed().toLower();
    while (term.startsWith(QLatin1Char(':'))) {
        term.remove(0, 1);
    }
    while (term.endsWith(QLatin1Char(':'))) {
        term.chop(1);
    }
    term.replace(QLatin1Char('-'), QLatin1Char('_'));
    term.replace(QLatin1Char(' '), QLatin1Char('_'));
    return term;
}

inline bool matchesSearch(const QString& emojiName, const QString& term)
{
    const QString needle = normalizeSearchTerm(term);
    if (needle.isEmpty()) {
        return true;
    }

    QString normalizedName = emojiName.toLower();
    normalizedName.replace(QLatin1Char('-'), QLatin1Char('_'));
    normalizedName.replace(QLatin1Char(' '), QLatin1Char('_'));
    return normalizedName.contains(needle);
}

inline QStringList legacyEmojiFontCandidates(Platform platform)
{
    switch (platform) {
    case Platform::Windows:
        return {QStringLiteral("Segoe UI Emoji"),
                QStringLiteral("Segoe UI Symbol")};
    case Platform::MacOS:
        return {QStringLiteral("Apple Color Emoji")};
    case Platform::Linux:
        return {QStringLiteral("Noto Color Emoji"),
                QStringLiteral("Noto Emoji"),
                QStringLiteral("Twemoji Mozilla")};
    case Platform::Other:
        return {QStringLiteral("Noto Color Emoji"),
                QStringLiteral("Noto Emoji"),
                QStringLiteral("Segoe UI Emoji"),
                QStringLiteral("Apple Color Emoji")};
    }
    return {};
}

inline QString chooseLegacyEmojiFontFamily(const QStringList& availableFamilies,
                                           Platform platform)
{
    const QStringList candidates = legacyEmojiFontCandidates(platform);
    for (const QString& candidate : candidates) {
        for (const QString& available : availableFamilies) {
            // QFontDatabase may append a foundry as "Family [Foundry]".
            if (available.compare(candidate, Qt::CaseInsensitive) == 0
                || available.startsWith(candidate + QStringLiteral(" ["),
                                        Qt::CaseInsensitive)) {
                return available;
            }
        }
    }
    return {};
}

inline QStringList installedFontFamilies()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QFontDatabase::families();
#else
    QFontDatabase database;
    return database.families();
#endif
}

inline QFont emojiButtonFont(QFont font, int pointSize = 16)
{
    font.setPointSize(pointSize);

#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    // Qt 6.9 gained a dedicated system emoji fallback path and prefers the
    // platform emoji font automatically for color emoji/sequences. Older Qt
    // versions need an explicit family to avoid a text-font glyph fallback.
    const QString family = chooseLegacyEmojiFontFamily(installedFontFamilies(),
                                                        currentPlatform());
    if (!family.isEmpty()) {
        font.setFamily(family);
    }
#endif
    return font;
}

} // namespace Mattermost::EmojiDialogSupport
