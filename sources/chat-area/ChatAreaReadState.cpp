#include "ChatArea.h"

#include "ChatLogWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::refreshReadState()
{
    // Navigation/presentation may require a re-check, but it never acknowledges
    // anything directly. ChatLogWidget applies the canonical lower-edge rule.
    if (ui && ui->listWidget) {
        ui->listWidget->refreshReadState();
    }
}

} // namespace Mattermost
