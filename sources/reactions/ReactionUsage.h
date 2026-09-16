#pragma once

#include <algorithm>
#include <cmath>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace Mattermost {

struct ReactionUsageEntry
{
    QString name;
    quint64 count = 0;
    double heat = 0.0;
};

/**
 * Small backend-independent popularity model for reactions.
 *
 * Every use cools all existing entries. A reaction with effective usage count
 * c keeps exp(-0.5 / sqrt(c)) of its previous heat per subsequent reaction
 * event. Therefore its heat half-life is about 1.386 * sqrt(c) events: rarely
 * used reactions cool quickly, while established habits remain useful for
 * longer. The selected reaction is then incremented and reheated to 1.0, which
 * makes the most recently selected reaction the hottest entry unconditionally.
 *
 * count is deliberately an aging familiarity score rather than a lifetime
 * total. When any entry reaches CountAgingThreshold all counts are halved
 * (rounding up). This keeps the counter bounded and gradually forgets old
 * habits, so newly introduced emoji can become established without competing
 * with an effectively infinite historical count.
 */
class ReactionUsageModel
{
public:
    static constexpr quint64 CountAgingThreshold = 128;

    explicit ReactionUsageModel(int capacity = 10)
        : capacity_(std::max(1, capacity))
    {
    }

    static double coolingFactor(quint64 count)
    {
        const double establishedCount = static_cast<double>(std::max<quint64>(1, count));
        return std::exp(-0.5 / std::sqrt(establishedCount));
    }

    void recordUse(QString name)
    {
        name = name.trimmed();
        if (name.isEmpty()) {
            return;
        }

        for (ReactionUsageEntry& entry : entries_) {
            entry.heat *= coolingFactor(entry.count);
        }

        auto selected = std::find_if(entries_.begin(), entries_.end(),
                                     [&name](const ReactionUsageEntry& entry) {
            return entry.name == name;
        });
        if (selected == entries_.end()) {
            entries_.push_back(ReactionUsageEntry {name, 1, 1.0});
        } else {
            ++selected->count;
            selected->heat = 1.0;
        }

        ageCountsIfNeeded();
        trim();
    }

    void restore(const QVector<ReactionUsageEntry>& restored)
    {
        entries_.clear();
        for (ReactionUsageEntry entry : restored) {
            entry.name = entry.name.trimmed();
            if (entry.name.isEmpty() || entry.count == 0
                || !std::isfinite(entry.heat) || entry.heat <= 0.0) {
                continue;
            }
            entry.heat = std::min(1.0, entry.heat);

            auto existing = std::find_if(entries_.begin(), entries_.end(),
                                         [&entry](const ReactionUsageEntry& candidate) {
                return candidate.name == entry.name;
            });
            if (existing == entries_.end()) {
                entries_.push_back(entry);
            } else {
                existing->count = std::max(existing->count, entry.count);
                existing->heat = std::max(existing->heat, entry.heat);
            }
        }
        ageCountsIfNeeded();
        trim();
    }

    QVector<ReactionUsageEntry> ranking() const
    {
        QVector<ReactionUsageEntry> ranked = entries_;
        std::sort(ranked.begin(), ranked.end(), hotterThan);
        return ranked;
    }

    QStringList topNames(int limit = -1) const
    {
        const QVector<ReactionUsageEntry> ranked = ranking();
        const int rankedSize = static_cast<int>(ranked.size());
        const int count = limit < 0 ? rankedSize : std::min(limit, rankedSize);
        QStringList names;
        names.reserve(count);
        for (int i = 0; i < count; ++i) {
            names.push_back(ranked.at(i).name);
        }
        return names;
    }

    int size() const { return static_cast<int>(entries_.size()); }

private:
    static bool hotterThan(const ReactionUsageEntry& left,
                           const ReactionUsageEntry& right)
    {
        if (left.heat != right.heat) {
            return left.heat > right.heat;
        }
        if (left.count != right.count) {
            return left.count > right.count;
        }
        return left.name < right.name;
    }

    static quint64 agedCount(quint64 count)
    {
        // ceil(count / 2) without count + 1, which could overflow on corrupt or
        // very old persisted data containing the maximum quint64 value.
        return std::max<quint64>(1, count / 2 + count % 2);
    }

    void ageCountsIfNeeded()
    {
        for (;;) {
            const bool needsAging = std::any_of(
                entries_.cbegin(), entries_.cend(), [](const ReactionUsageEntry& entry) {
                    return entry.count >= CountAgingThreshold;
                });
            if (!needsAging) {
                return;
            }

            for (ReactionUsageEntry& entry : entries_) {
                entry.count = agedCount(entry.count);
            }
        }
    }

    void trim()
    {
        std::sort(entries_.begin(), entries_.end(), hotterThan);
        if (static_cast<int>(entries_.size()) > capacity_) {
            entries_.resize(capacity_);
        }
    }

    int capacity_ = 10;
    QVector<ReactionUsageEntry> entries_;
};

inline QByteArray serializeReactionUsage(const QVector<ReactionUsageEntry>& entries)
{
    QJsonArray array;
    for (const ReactionUsageEntry& entry : entries) {
        array.push_back(QJsonObject {
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("count"), QString::number(entry.count)},
            {QStringLiteral("heat"), entry.heat},
        });
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

inline QVector<ReactionUsageEntry> deserializeReactionUsage(const QByteArray& data)
{
    QVector<ReactionUsageEntry> entries;
    if (data.isEmpty()) {
        return entries;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return entries;
    }

    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        bool countOk = false;
        const quint64 count = object.value(QStringLiteral("count"))
                                  .toString().toULongLong(&countOk);
        const double heat = object.value(QStringLiteral("heat")).toDouble(-1.0);
        const QString name = object.value(QStringLiteral("name")).toString();
        if (!countOk || count == 0 || name.trimmed().isEmpty()
            || !std::isfinite(heat) || heat <= 0.0) {
            continue;
        }
        entries.push_back(ReactionUsageEntry {name, count, heat});
    }
    return entries;
}

inline QStringList defaultReactionFavorites()
{
    return {
        QStringLiteral("+1"),
        QStringLiteral("eyes"),
        QStringLiteral("fire"),
        QStringLiteral("rolling_on_the_floor_laughing"),
    };
}

/**
 * Build the six-slot hover strip. Start with three popular reactions and then
 * add up to three non-overlapping favorites. Favorite collisions are replaced
 * by further entries from the popularity ranking until maxActions is reached.
 */
inline QStringList selectQuickReactionNames(const QStringList& popular,
                                            const QStringList& favorites,
                                            int maxActions = 6,
                                            int preferredPopular = 3,
                                            int preferredFavorites = 3)
{
    QStringList selected;
    QSet<QString> seen;

    maxActions = std::max(0, maxActions);
    preferredPopular = std::max(0, preferredPopular);
    preferredFavorites = std::max(0, preferredFavorites);

    const auto appendUnique = [&selected, &seen, maxActions](const QString& name) {
        if (static_cast<int>(selected.size()) >= maxActions
            || name.isEmpty() || seen.contains(name)) {
            return false;
        }
        seen.insert(name);
        selected.push_back(name);
        return true;
    };

    const int popularSize = static_cast<int>(popular.size());
    for (int i = 0; i < popularSize && i < preferredPopular; ++i) {
        appendUnique(popular.at(i));
    }

    int favoriteCount = 0;
    for (const QString& favorite : favorites) {
        if (favoriteCount >= preferredFavorites
            || static_cast<int>(selected.size()) >= maxActions) {
            break;
        }
        if (appendUnique(favorite)) {
            ++favoriteCount;
        }
    }

    if (static_cast<int>(selected.size()) < maxActions) {
        for (const QString& name : popular) {
            if (static_cast<int>(selected.size()) >= maxActions) {
                break;
            }
            appendUnique(name);
        }
    }

    return selected;
}

} // namespace Mattermost
