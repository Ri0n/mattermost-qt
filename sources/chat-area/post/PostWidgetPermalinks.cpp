/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "PostWidget.h"

#include <QJsonArray>
#include <QJsonObject>

#include "MessageContentWidget.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendUser.h"
#include "chat-area/QuotedPostPreview.h"
#include "navigation/AppNavigationService.h"
#include "ui_PostWidget.h"

namespace Mattermost {

void PostWidget::refreshPermalinkPreviews()
{
    for (auto& preview : permalinkPreviews) {
        if (preview) {
            ui->verticalLayout->removeWidget(preview.get());
        }
    }
    permalinkPreviews.clear();

    if (post.isDeleted || !messageContent) {
        return;
    }

    int insertIndex = ui->verticalLayout->indexOf(messageContent) + 1;
    for (const QJsonValue& embedValue : post.embeds) {
        const QJsonObject embed = embedValue.toObject();
        if (embed.value(QStringLiteral("type")).toString()
            != QStringLiteral("permalink")) {
            continue;
        }

        const QJsonObject data = embed.value(QStringLiteral("data")).toObject();
        const QJsonObject previewPost = data.value(QStringLiteral("post")).toObject();
        if (previewPost.isEmpty()) {
            // Mattermost omits the embedded post when the viewer is not allowed
            // to see it. Do not issue a second client-side fetch that could
            // bypass the server's preview/permission decision.
            continue;
        }

        QString postId = data.value(QStringLiteral("post_id")).toString();
        if (postId.isEmpty()) {
            postId = previewPost.value(QStringLiteral("id")).toString();
        }
        if (postId.isEmpty()) {
            continue;
        }

        const QString userId = previewPost.value(QStringLiteral("user_id")).toString();
        QString authorName;
        if (const BackendUser* author = backend.getStorage().getUserById(userId)) {
            authorName = author->getDisplayName();
        }
        if (authorName.isEmpty()) {
            authorName = previewPost.value(QStringLiteral("props")).toObject()
                .value(QStringLiteral("override_username")).toString();
        }
        if (authorName.isEmpty()) {
            authorName = tr("Message");
        }

        const QString channelName =
            data.value(QStringLiteral("channel_display_name")).toString();
        const QString title = channelName.isEmpty()
            ? authorName
            : authorName + QStringLiteral(" · ") + channelName;

        const QJsonObject previewMetadata =
            previewPost.value(QStringLiteral("metadata")).toObject();
        const bool hasAttachments =
            !previewPost.value(QStringLiteral("file_ids")).toArray().isEmpty()
            || !previewMetadata.value(QStringLiteral("files")).toArray().isEmpty();

        auto preview = std::make_unique<QuotedPostPreview>(this, 4);
        preview->setPreview(title,
                            previewPost.value(QStringLiteral("message")).toString(),
                            hasAttachments);
        preview->setActivatedCallback([this, postId] {
            AppNavigationService::instance(backend).openPost(postId);
        });

        ui->verticalLayout->insertWidget(insertIndex++, preview.get());
        permalinkPreviews.push_back(std::move(preview));
    }

    if (!permalinkPreviews.empty()) {
        emit dimensionsChanged();
    }
}

} // namespace Mattermost
