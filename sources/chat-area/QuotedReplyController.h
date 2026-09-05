#pragma once

#include <QObject>

class QEvent;
class QToolButton;
class QWidget;

namespace Mattermost {

class BackendPost;
class ChatArea;
class OutgoingPostCreator;
class QuotedPostPreview;

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
    QWidget* previewRow = nullptr;
    QuotedPostPreview* preview = nullptr;
    QToolButton* cancelButton = nullptr;
    OutgoingPostCreator* editor = nullptr;
};

} // namespace Mattermost
