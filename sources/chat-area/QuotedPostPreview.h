#pragma once

#include <QFrame>
#include <functional>

class QEvent;
class QLabel;
class QMouseEvent;
class QResizeEvent;

namespace Mattermost {

class BackendPost;

class QuotedPostPreview final : public QFrame
{
public:
    explicit QuotedPostPreview(QWidget* parent = nullptr, int maximumLines = 2);

    void setPost(const BackendPost& post);
    void setPreview(const QString& title,
                    const QString& message,
                    bool hasAttachments = false);
    void setActivatedCallback(std::function<void()> callback);

protected:
    void changeEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshPalette();
    void refreshText();

    QLabel* authorLabel = nullptr;
    QLabel* messageLabel = nullptr;
    QFrame* bar = nullptr;
    QString fullText;
    int maximumLines = 2;
    std::function<void()> activatedCallback;
};

} // namespace Mattermost
