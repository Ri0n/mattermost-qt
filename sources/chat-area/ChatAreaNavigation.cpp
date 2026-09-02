#include "ChatArea.h"

#include <QListWidgetItem>

#include "PostsListWidget.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

bool ChatArea::ensurePostVisible(const QString& postId)
{
    if (isThread || postId.isEmpty() || !ui || !ui->listWidget) {
        return false;
    }

    PostsListWidget* list = ui->listWidget;
    if (list->findPost(postId)) {
        return true;
    }

    BackendPost* post = channel.postIdToPost.value(postId, nullptr);
    // Replies are intentionally hidden from the main channel timeline. Pinned
    // reply navigation resolves to its root before reaching this method.
    if (!post || post->hidden || !post->root_id.isEmpty()) {
        return false;
    }

    int insertRow = list->count();
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isPostItem(item)) {
            continue;
        }

        auto* existing = qobject_cast<PostWidget*>(list->itemWidget(item));
        if (!existing) {
            continue;
        }

        const BackendPost& current = existing->post;
        if (current.create_at > post->create_at
            || (current.create_at == post->create_at && current.id > post->id)) {
            insertRow = row;
            break;
        }
    }

    list->insertPost(insertRow, new PostWidget(backend, *post, list, this, nullptr));
    list->updateGeometry();
    return list->findPost(postId) != nullptr;
}

} // namespace Mattermost
