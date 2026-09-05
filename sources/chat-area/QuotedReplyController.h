#pragma once

#include <QObject>

class QEvent;
class QFrame;
class QLabel;
class QToolButton;
class QWidget;

namespace Mattermost {

class BackendPost;
class ChatArea;
class OutgoingPostCreator;

/** Owns the UI-only quoted-reply state around the composer QTextEdit. */
class QuotedReplyController final : public QObject
{
public:
    static QuotedReplyController& instance(ChatArea& area);

    void begin(const BackendPost& post);
    void cancel();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit QuotedReplyController(ChatArea& area);
    void ensureUi();
    void syncVisibility();

    ChatArea& area;
    QWidget* wrapper = nullptr;
    QFrame* preview = nullptr;
    QLabel* authorLabel = nullptr;
    QLabel* messageLabel = nullptr;
    QToolButton* cancelButton = nullptr;
    OutgoingPostCreator* editor = nullptr;
};

} // namespace Mattermost
