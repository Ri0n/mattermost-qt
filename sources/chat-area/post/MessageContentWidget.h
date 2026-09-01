#pragma once

#include <QString>
#include <QtGlobal>
#include <QWidget>

class QVBoxLayout;

namespace Mattermost {

class MessageContentWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MessageContentWidget(QWidget* parent = nullptr);

    void setMessage(const QString& message);
    void clear();
    QString selectedText() const;

signals:
    void linkHovered(const QString& link);
    void dimensionsChanged();

private:
    void clearContent();
    void addRichText(const QString& html);
    void scheduleDimensionsChanged();

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    void addMarkdownContent(const QString& message);
    void addCodeBlock(const QString& code, const QString& language);
#endif

    QVBoxLayout* contentLayout;
    bool dimensionsChangePending = false;
};

} // namespace Mattermost
