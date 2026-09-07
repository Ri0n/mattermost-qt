#pragma once

#include "PostWidget.h"

class QPaintEvent;
class QVariantAnimation;

namespace Mattermost {

/**
 * PostWidget presentation used by virtualized chat logs.
 *
 * LongListWidget owns generic row interaction feedback such as hover highlighting.
 * This subclass keeps only the Mattermost-specific visual cue used when explicit
 * navigation lands on a post.
 */
class InteractivePostWidget final : public PostWidget
{
public:
    InteractivePostWidget(Backend& backend,
                          BackendPost& post,
                          QWidget* parent,
                          ChatArea* chatArea,
                          BackendPost* lastRootPost);

    void animateNavigationHighlight();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation* navigationAnimation = nullptr;
    int navigationAlpha = 0;
};

} // namespace Mattermost
