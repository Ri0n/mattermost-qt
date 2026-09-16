#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost {

inline bool reorderSidebarChannel(QStringList& channelIds,
                                  const QString& channelId,
                                  const QString& targetChannelId,
                                  bool afterTarget)
{
    const QStringList before = channelIds;
    const int oldIndex = channelIds.indexOf(channelId);
    if (oldIndex < 0) {
        return false;
    }

    channelIds.removeAt(oldIndex);
    int insertIndex = channelIds.size();
    if (!targetChannelId.isEmpty()) {
        const int targetIndex = channelIds.indexOf(targetChannelId);
        if (targetIndex >= 0) {
            insertIndex = targetIndex + (afterTarget ? 1 : 0);
        }
    }
    channelIds.insert(insertIndex, channelId);
    return channelIds != before;
}

inline bool moveSidebarChannel(QStringList& sourceChannelIds,
                               QStringList& targetChannelIds,
                               const QString& channelId,
                               const QString& targetChannelId = {},
                               bool afterTarget = false)
{
    if (channelId.isEmpty()) {
        return false;
    }

    const QStringList sourceBefore = sourceChannelIds;
    const QStringList targetBefore = targetChannelIds;
    sourceChannelIds.removeAll(channelId);
    targetChannelIds.removeAll(channelId);

    int insertIndex = targetChannelIds.size();
    if (!targetChannelId.isEmpty()) {
        const int targetIndex = targetChannelIds.indexOf(targetChannelId);
        if (targetIndex >= 0) {
            insertIndex = targetIndex + (afterTarget ? 1 : 0);
        }
    }
    targetChannelIds.insert(insertIndex, channelId);
    return sourceChannelIds != sourceBefore || targetChannelIds != targetBefore;
}

} // namespace Mattermost
