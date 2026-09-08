/**
 * @file NewPollDialog.cpp
 * @brief New Poll Dialog - shown when creating a poll
 * @author Lyubomir Filipov
 * @date Mar 15, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include <QPushButton>
#include <QSet>

#include "NewPollDialog.h"
#include "OutgoingPostCreator.h"
#include "ui_NewPollDialog.h"

namespace Mattermost {
namespace {

QStringList pollOptionsFromText(const QString& text)
{
    QStringList options;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString option = line.trimmed();
        if (!option.isEmpty()) {
            options.push_back(option);
        }
    }
    return options;
}

} // namespace

NewPollDialog::NewPollDialog(QWidget* parent, BackendNewPollData initialPollData)
    : QDialog(parent)
    , ui(new Ui::NewPollDialog)
    , rootId(initialPollData.rootId)
{
    ui->setupUi(this);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Create"));

    // The legacy /poll path constructs BackendNewPollData inside the composer.
    // Preserve the thread root from that composer so Matterpoll can create the
    // generated post as a real thread reply.
    if (rootId.isEmpty()) {
        if (const auto* creator = qobject_cast<const OutgoingPostCreator*>(parent)) {
            rootId = creator->rootId();
        }
    }

    connect(ui->questionValue, &QLineEdit::textChanged,
            this, &NewPollDialog::validateInput);
    connect(ui->optionsValue, &QPlainTextEdit::textChanged,
            this, &NewPollDialog::validateInput);

    ui->questionValue->setText(initialPollData.question);

    QStringList initialOptions;
    initialOptions.reserve(initialPollData.options.size());
    for (const QString& option : initialPollData.options) {
        initialOptions.push_back(option);
    }
    ui->optionsValue->setPlainText(initialOptions.join(QLatin1Char('\n')));

    ui->checkBoxAnonymous->setChecked(initialPollData.isAnonymous);
    ui->checkBoxAnonymousCreator->setChecked(initialPollData.isAnonymousCreator);
    ui->checkBoxProgress->setChecked(initialPollData.showProgress);
    ui->checkBoxAllowAdditional->setChecked(initialPollData.allowAddOptions);
    ui->maxVotesValue->setValue(initialPollData.maxVotes);

    validateInput();
    setAttribute(Qt::WA_DeleteOnClose);
}

NewPollDialog::~NewPollDialog()
{
    delete ui;
}

void NewPollDialog::validateInput()
{
    const QStringList options = pollOptionsFromText(ui->optionsValue->toPlainText());
    ui->maxVotesValue->setMaximum(qMax(2, options.size()));

    if (ui->questionValue->text().trimmed().isEmpty()) {
        return disableSendButton(tr("'Question' is empty"));
    }

    if (options.size() < 2) {
        return disableSendButton(tr("At least two options are required"));
    }

    QSet<QString> uniqueOptions;
    for (const QString& option : options) {
        if (uniqueOptions.contains(option)) {
            return disableSendButton(tr("Duplicate options are not allowed"));
        }
        uniqueOptions.insert(option);
    }

    auto* okButton = ui->buttonBox->button(QDialogButtonBox::Ok);
    okButton->setEnabled(true);
    okButton->setToolTip(QString());
}

void NewPollDialog::disableSendButton(const QString& tooltip)
{
    auto* okButton = ui->buttonBox->button(QDialogButtonBox::Ok);
    okButton->setEnabled(false);
    okButton->setToolTip(tooltip);
}

BackendNewPollData NewPollDialog::getData()
{
    BackendNewPollData ret;
    ret.question = ui->questionValue->text().trimmed();

    const QStringList options = pollOptionsFromText(ui->optionsValue->toPlainText());
    ret.options.reserve(options.size());
    for (const QString& option : options) {
        ret.options.push_back(option);
    }

    ret.rootId = rootId;
    ret.isAnonymous = ui->checkBoxAnonymous->isChecked();
    ret.isAnonymousCreator = ui->checkBoxAnonymousCreator->isChecked();
    ret.showProgress = ui->checkBoxProgress->isChecked();
    ret.allowAddOptions = ui->checkBoxAllowAdditional->isChecked();
    ret.maxVotes = ui->maxVotesValue->value();

    if (auto* creator = qobject_cast<OutgoingPostCreator*>(parentWidget())) {
        creator->armPollRealtimeAcknowledgement(ret);
    }

    return ret;
}

} /* namespace Mattermost */
