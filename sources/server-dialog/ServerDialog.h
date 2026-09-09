/**
 * @file ServerDialog.h
 * @brief Native renderer for Mattermost interactive dialogs.
 */

#pragma once

#include <functional>

#include <QDialog>
#include <QJsonObject>
#include <QJsonValue>
#include <QVector>

class QLabel;
class QVBoxLayout;

namespace Mattermost {

class Backend;

class ServerDialog : public QDialog {
public:
    ServerDialog(Backend& backend,
                 const QJsonObject& dialog,
                 const QString& url,
                 const QString& channelId,
                 const QString& teamId,
                 QWidget* parent = nullptr);

private:
    struct FieldBinding {
        QString name;
        QString displayName;
        bool optional = false;
        int minLength = 0;
        int maxLength = 0;
        std::function<QJsonValue()> value;
    };

    void addElement(const QJsonObject& element, QVBoxLayout* layout);
    bool collectSubmission(QJsonObject& submission);
    void submit(bool cancelled);
    void showValidationError(const QString& message);

    Backend& backend;
    QJsonObject dialog;
    QString url;
    QString channelId;
    QString teamId;
    QVector<FieldBinding> fields;
    QLabel* errorLabel = nullptr;
    bool hasUnsupportedElements = false;
};

} // namespace Mattermost
