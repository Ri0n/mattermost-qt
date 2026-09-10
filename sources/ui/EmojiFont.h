#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

namespace Mattermost::EmojiFont {

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
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return Platform::MacOS;
#elif defined(Q_OS_LINUX)
    return Platform::Linux;
#else
    return Platform::Other;
#endif
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

inline QString legacyEmojiFontFamily()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    // Qt 6.9 has a dedicated platform emoji fallback path. Do not override it.
    return {};
#else
    // Font enumeration can be relatively expensive, especially on X11. The
    // installed font set is effectively stable during a normal application run.
    static const QString family = chooseLegacyEmojiFontFamily(installedFontFamilies(),
                                                               currentPlatform());
    return family;
#endif
}

inline QFont applySystemEmojiFamily(QFont font)
{
    const QString family = legacyEmojiFontFamily();
    if (!family.isEmpty()) {
        font.setFamily(family);
    }
    return font;
}

} // namespace Mattermost::EmojiFont
