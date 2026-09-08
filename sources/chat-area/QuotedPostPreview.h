#pragma once

#include <QFrame>
#include <functional>

class QEvent;
class QLabel;
class QMouseEvent;
class QTextBrowser;
class QUrl;

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
    void setLinkActivatedCallback(std::function<void(const QUrl&)> callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void refreshPalette();
    void refreshText();

    QLabel* authorLabel = nullptr;
    QTextBrowser* messageBrowser = nullptr;
    QFrame* bar = nullptr;
    QString fullText;
    int maximumLines = 2;
    std::function<void()> activatedCallback;
    std::function<void(const QUrl&)> linkActivatedCallback;
};

} // namespace Mattermost
