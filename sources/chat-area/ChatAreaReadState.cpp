#include "ChatArea.h"

#include "ChatLogWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::requestExplicitReadAcknowledgement()
{
    // Compatibility entry point for semantic navigation. Navigation itself is
    // never a read acknowledgement: it only asks ChatLogWidget to re-evaluate
    // the concrete viewport using the same lower-edge rule as ordinary scrolling.
    if (ui && ui->listWidget) {
        ui->listWidget->refreshReadState();
    }
}

void ChatArea::tryExplicitReadAcknowledgement()
{
    requestExplicitReadAcknowledgement();
}

void ChatArea::requestThreadReadAcknowledgement()
{
    // Thread reads use the exact same viewport cursor as channel reads. The
    // thread-specific server acknowledgement is issued only when that cursor
    // reaches an authoritative thread tail.
    if (ui && ui->listWidget) {
        ui->listWidget->refreshReadState();
    }
}

void ChatArea::tryThreadReadAcknowledgement()
{
    requestThreadReadAcknowledgement();
}

} // namespace Mattermost
