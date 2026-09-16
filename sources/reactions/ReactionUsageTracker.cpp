#include "ReactionUsageTracker.h"

#include <QSettings>

namespace Mattermost {
namespace {

const char* const reactionUsageSettingsKey = "reaction_usage/popularity_v1";

} // namespace

ReactionUsageTracker& ReactionUsageTracker::instance()
{
    static ReactionUsageTracker tracker;
    return tracker;
}

ReactionUsageTracker::ReactionUsageTracker()
{
    QSettings settings;
    model_.restore(deserializeReactionUsage(
        settings.value(QLatin1String(reactionUsageSettingsKey)).toByteArray()));
}

void ReactionUsageTracker::recordUse(const QString& emojiName)
{
    if (emojiName.trimmed().isEmpty()) {
        return;
    }
    model_.recordUse(emojiName);
    save();
}

QVector<ReactionUsageEntry> ReactionUsageTracker::ranking() const
{
    return model_.ranking();
}

QStringList ReactionUsageTracker::topNames(int limit) const
{
    return model_.topNames(limit);
}

void ReactionUsageTracker::save() const
{
    QSettings settings;
    settings.setValue(QLatin1String(reactionUsageSettingsKey),
                      serializeReactionUsage(model_.ranking()));
}

} // namespace Mattermost
