/**
 * @file ServerDialog.cpp
 * @brief Native renderer for Mattermost interactive dialogs.
 */

#include "ServerDialog.h"

#include <utility>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

#include "backend/Backend.h"

namespace Mattermost {
namespace {

QLabel* makeHelpLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString displayName(const QJsonObject& element)
{
    const QString name = element.value(QStringLiteral("display_name")).toString();
    return name.isEmpty() ? element.value(QStringLiteral("name")).toString() : name;
}

bool boolDefault(const QJsonValue& value)
{
    if (value.isBool()) {
        return value.toBool();
    }
    const QString text = value.toString().trimmed().toLower();
    return text == QStringLiteral("true") || text == QStringLiteral("1")
        || text == QStringLiteral("yes");
}

} // namespace

ServerDialog::ServerDialog(Backend& backend,
                           const QJsonObject& dialog,
                           const QString& url,
                           const QString& channelId,
                           const QString& teamId,
                           QWidget* parent)
    : QDialog(parent)
    , backend_(backend)
    , dialog_(dialog)
    , url_(url)
    , channelId_(channelId)
    , teamId_(teamId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setWindowTitle(dialog.value(QStringLiteral("title")).toString());
    resize(440, 240);

    auto* layout = new QVBoxLayout(this);

    const QString introduction = dialog.value(QStringLiteral("introduction_text")).toString();
    if (!introduction.isEmpty()) {
        auto* introLabel = makeHelpLabel(introduction, this);
        layout->addWidget(introLabel);
    }

    const QJsonArray elements = dialog.value(QStringLiteral("elements")).toArray();
    for (const QJsonValue& value : elements) {
        if (value.isObject()) {
            addElement(value.toObject(), layout);
        }
    }

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setVisible(false);
    layout->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton* submitButton = buttons->addButton(
        dialog.value(QStringLiteral("submit_label")).toString(QStringLiteral("Submit")),
        QDialogButtonBox::AcceptRole);
    submitButton->setEnabled(!hasUnsupportedElements_);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        submit(false);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        if (dialog_.value(QStringLiteral("notify_on_cancel")).toBool(false)) {
            submit(true);
        } else {
            reject();
        }
    });
}

void ServerDialog::addElement(const QJsonObject& element, QVBoxLayout* layout)
{
    const QString name = element.value(QStringLiteral("name")).toString();
    const QString type = element.value(QStringLiteral("type")).toString();
    const QString labelText = displayName(element);
    const QString helpText = element.value(QStringLiteral("help_text")).toString();
    const QString placeholder = element.value(QStringLiteral("placeholder")).toString();
    const bool optional = element.value(QStringLiteral("optional")).toBool(false);
    const int minLength = element.value(QStringLiteral("min_length")).toInt(0);
    const int maxLength = element.value(QStringLiteral("max_length")).toInt(0);

    auto* group = new QGroupBox(labelText, this);
    auto* groupLayout = new QVBoxLayout(group);

    FieldBinding binding;
    binding.name = name;
    binding.displayName = labelText;
    binding.optional = optional;
    binding.minLength = minLength;
    binding.maxLength = maxLength;

    if (type == QStringLiteral("text")) {
        auto* edit = new QLineEdit(group);
        edit->setText(element.value(QStringLiteral("default")).toString());
        edit->setPlaceholderText(placeholder);
        if (maxLength > 0) {
            edit->setMaxLength(maxLength);
        }
        const QString subtype = element.value(QStringLiteral("subtype")).toString();
        if (subtype == QStringLiteral("password")) {
            edit->setEchoMode(QLineEdit::Password);
        }
        groupLayout->addWidget(edit);
        binding.value = [edit, subtype] {
            if (subtype == QStringLiteral("number")) {
                bool ok = false;
                const double number = edit->text().toDouble(&ok);
                if (ok) {
                    return QJsonValue(number);
                }
            }
            return QJsonValue(edit->text());
        };
    } else if (type == QStringLiteral("textarea")) {
        auto* edit = new QPlainTextEdit(group);
        edit->setPlainText(element.value(QStringLiteral("default")).toString());
        edit->setPlaceholderText(placeholder);
        edit->setMinimumHeight(100);
        groupLayout->addWidget(edit);
        binding.value = [edit] {
            return QJsonValue(edit->toPlainText());
        };
    } else if (type == QStringLiteral("bool")) {
        auto* check = new QCheckBox(placeholder, group);
        check->setChecked(boolDefault(element.value(QStringLiteral("default"))));
        groupLayout->addWidget(check);
        binding.value = [check] {
            return QJsonValue(check->isChecked());
        };
    } else if (type == QStringLiteral("radio")) {
        auto* buttons = new QButtonGroup(group);
        const QString defaultValue = element.value(QStringLiteral("default")).toString();
        const QJsonArray options = element.value(QStringLiteral("options")).toArray();
        for (const QJsonValue& optionValue : options) {
            const QJsonObject option = optionValue.toObject();
            auto* radio = new QRadioButton(option.value(QStringLiteral("text")).toString(), group);
            const QString value = option.value(QStringLiteral("value")).toVariant().toString();
            radio->setProperty("dialogValue", value);
            buttons->addButton(radio);
            groupLayout->addWidget(radio);
            if (!defaultValue.isEmpty() && value == defaultValue) {
                radio->setChecked(true);
            }
        }
        binding.value = [buttons] {
            if (QAbstractButton* checked = buttons->checkedButton()) {
                return QJsonValue(checked->property("dialogValue").toString());
            }
            return QJsonValue();
        };
    } else if (type == QStringLiteral("select")
               && element.value(QStringLiteral("data_source")).toString().isEmpty()) {
        const bool multiselect = element.value(QStringLiteral("multiselect")).toBool(false);
        const QJsonArray options = element.value(QStringLiteral("options")).toArray();
        const QString defaultValue = element.value(QStringLiteral("default")).toString();

        if (multiselect) {
            auto* list = new QListWidget(group);
            list->setSelectionMode(QAbstractItemView::MultiSelection);
            const QStringList defaults = defaultValue.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QJsonValue& optionValue : options) {
                const QJsonObject option = optionValue.toObject();
                const QString value = option.value(QStringLiteral("value")).toVariant().toString();
                auto* item = new QListWidgetItem(option.value(QStringLiteral("text")).toString(), list);
                item->setData(Qt::UserRole, value);
                item->setSelected(defaults.contains(value));
            }
            groupLayout->addWidget(list);
            binding.value = [list] {
                QJsonArray values;
                for (QListWidgetItem* item : list->selectedItems()) {
                    values.append(item->data(Qt::UserRole).toString());
                }
                return QJsonValue(values);
            };
        } else {
            auto* combo = new QComboBox(group);
            if (optional) {
                combo->addItem(placeholder, QVariant());
            }
            int defaultIndex = -1;
            for (const QJsonValue& optionValue : options) {
                const QJsonObject option = optionValue.toObject();
                const QString value = option.value(QStringLiteral("value")).toVariant().toString();
                combo->addItem(option.value(QStringLiteral("text")).toString(), value);
                if (!defaultValue.isEmpty() && value == defaultValue) {
                    defaultIndex = combo->count() - 1;
                }
            }
            if (defaultIndex >= 0) {
                combo->setCurrentIndex(defaultIndex);
            }
            groupLayout->addWidget(combo);
            binding.value = [combo] {
                const QVariant value = combo->currentData();
                return value.isValid() ? QJsonValue(value.toString()) : QJsonValue();
            };
        }
    } else if (type == QStringLiteral("date")) {
        auto* edit = new QDateEdit(group);
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        const QDate value = QDate::fromString(
            element.value(QStringLiteral("default")).toString(), Qt::ISODate);
        if (value.isValid()) {
            edit->setDate(value);
        }
        groupLayout->addWidget(edit);
        binding.value = [edit] {
            return QJsonValue(edit->date().toString(Qt::ISODate));
        };
    } else if (type == QStringLiteral("datetime")) {
        auto* edit = new QDateTimeEdit(group);
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        const QDateTime value = QDateTime::fromString(
            element.value(QStringLiteral("default")).toString(), Qt::ISODate);
        if (value.isValid()) {
            edit->setDateTime(value);
        }
        groupLayout->addWidget(edit);
        binding.value = [edit] {
            return QJsonValue(edit->dateTime().toString(Qt::ISODate));
        };
    } else {
        hasUnsupportedElements_ = true;
        auto* unsupported = makeHelpLabel(
            tr("Unsupported interactive dialog field type: %1").arg(type), group);
        groupLayout->addWidget(unsupported);
    }

    if (!helpText.isEmpty()) {
        groupLayout->addWidget(makeHelpLabel(helpText, group));
    }

    layout->addWidget(group);
    if (binding.value) {
        fields_.push_back(std::move(binding));
    }
}

bool ServerDialog::collectSubmission(QJsonObject& submission)
{
    for (const FieldBinding& field : fields_) {
        const QJsonValue value = field.value();
        const bool isEmptyString = value.isString() && value.toString().isEmpty();
        const bool isEmptyArray = value.isArray() && value.toArray().isEmpty();
        const bool isMissing = value.isUndefined() || value.isNull() || isEmptyString || isEmptyArray;

        if (!field.optional && isMissing) {
            showValidationError(tr("%1 is required.").arg(field.displayName));
            return false;
        }

        if (value.isString()) {
            const int length = value.toString().size();
            if (field.minLength > 0 && length < field.minLength) {
                showValidationError(tr("%1 must contain at least %2 characters.")
                                        .arg(field.displayName)
                                        .arg(field.minLength));
                return false;
            }
            if (field.maxLength > 0 && length > field.maxLength) {
                showValidationError(tr("%1 must contain at most %2 characters.")
                                        .arg(field.displayName)
                                        .arg(field.maxLength));
                return false;
            }
        }

        if (!isMissing || !field.optional) {
            submission.insert(field.name, value);
        }
    }
    return true;
}

void ServerDialog::submit(bool cancelled)
{
    QJsonObject submission;
    if (!cancelled && !collectSubmission(submission)) {
        return;
    }

    QJsonObject request {
        {QStringLiteral("callback_id"), dialog_.value(QStringLiteral("callback_id")).toString()},
        {QStringLiteral("channel_id"), channelId_},
        {QStringLiteral("state"), dialog_.value(QStringLiteral("state")).toString()},
        {QStringLiteral("submission"), submission},
        {QStringLiteral("team_id"), teamId_},
        {QStringLiteral("url"), url_},
    };
    if (cancelled) {
        request.insert(QStringLiteral("cancelled"), true);
    }

    backend_.sendSubmitDialog(QJsonDocument(request));
    done(cancelled ? QDialog::Rejected : QDialog::Accepted);
}

void ServerDialog::showValidationError(const QString& message)
{
    errorLabel_->setText(message);
    errorLabel_->setVisible(true);
}

} // namespace Mattermost
