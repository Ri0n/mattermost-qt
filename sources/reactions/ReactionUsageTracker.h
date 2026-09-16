#pragma once

#include <QStringList>
#include <QVector>

#include "ReactionUsage.h"

namespace Mattermost {

class ReactionUsageTracker
{
public:
    static ReactionUsageTracker& instance();

    void recordUse(const QString& emojiName);
    QVector<ReactionUsageEntry> ranking() const;
    QStringList topNames(int limit = 10) const;

private:
    ReactionUsageTracker();
    void save() const;

    ReactionUsageModel model_ {10};
};

} // namespace Mattermost
