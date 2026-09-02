#pragma once

#include <QJsonArray>
#include <QSet>
#include <QStringList>

namespace Mattermost {

struct PostNavigationWindow {
    QJsonArray newestFirstOrder;
    QStringList chronologicalIds;
};

/**
 * Mattermost's before/after PostList order is newest -> oldest. Assemble one
 * request-local window around targetId, deduplicate any overlapping cursor, and
 * expose both the backend ingestion order and timeline chronological order.
 */
inline PostNavigationWindow buildPostNavigationWindow(const QJsonArray& afterNewestFirst,
                                                       const QString& targetId,
                                                       const QJsonArray& beforeNewestFirst)
{
    PostNavigationWindow result;
    if (targetId.isEmpty()) {
        return result;
    }

    QSet<QString> seen;
    const auto append = [&result, &seen](const QJsonArray& source) {
        for (const QJsonValue& value : source) {
            const QString id = value.toString();
            if (id.isEmpty() || seen.contains(id)) {
                continue;
            }
            seen.insert(id);
            result.newestFirstOrder.push_back(id);
        }
    };

    append(afterNewestFirst);
    if (!seen.contains(targetId)) {
        seen.insert(targetId);
        result.newestFirstOrder.push_back(targetId);
    }
    append(beforeNewestFirst);

    result.chronologicalIds.reserve(result.newestFirstOrder.size());
    for (int i = result.newestFirstOrder.size() - 1; i >= 0; --i) {
        result.chronologicalIds.push_back(result.newestFirstOrder.at(i).toString());
    }
    return result;
}

} // namespace Mattermost
