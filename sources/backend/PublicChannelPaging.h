#pragma once

#include <QString>

namespace Mattermost {

inline constexpr int PublicChannelsPerPage = 100;

inline QString publicChannelsPagePath(const QString& teamId, int page,
                                      int perPage = PublicChannelsPerPage)
{
    return QStringLiteral("teams/") + teamId
        + QStringLiteral("/channels?page=") + QString::number(page)
        + QStringLiteral("&per_page=") + QString::number(perPage);
}

inline int publicChannelLogicalCountAfterPage(int page, int returnedCount,
                                              int perPage = PublicChannelsPerPage)
{
    const int pageStart = page * perPage;
    const int concreteEnd = pageStart + returnedCount;
    return returnedCount == perPage ? concreteEnd + perPage : concreteEnd;
}

inline bool publicChannelPageProvesEnd(int returnedCount,
                                       int perPage = PublicChannelsPerPage)
{
    return returnedCount < perPage;
}

} // namespace Mattermost
