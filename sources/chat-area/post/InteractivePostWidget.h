#pragma once

#include "PostWidget.h"

class QEvent;
class QPaintEvent;
class QVariantAnimation;

namespace Mattermost {

/**
 * PostWidget presentation used by virtualized chat logs.
 *
 * QListWidget used to provide a soft hovered row background and the old
 * navigation path briefly highlighted the destination item. LongListWidget
 * materializes raw widgets instead, so both presentation states live here and
 * remain independent from list implementation details.
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
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation* navigationAnimation = nullptr;
    int navigationAlpha = 0;
    bool hovered = false;
};

} // namespace Mattermost
