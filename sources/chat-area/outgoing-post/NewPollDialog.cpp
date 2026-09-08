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
 * the Free Software Foundation; either version 3 of the License, or
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
#include "NewPollDialog.h"
#include "OutgoingPostCreator.h"
#include "ui_NewPollDialog.h"

namespace Mattermost {

NewPollDialog::NewPollDialog(QWidget *parent, BackendNewPollData initialPollData)
:QDialog(parent)
,ui(new Ui::NewPollDialog)
,rootId(initialPollData.rootId)
{
	ui->setupUi(this);
	ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Create"));

	// The legacy /poll path constructs BackendNewPollData inside the composer.
	// Preserve the thread root from that composer so Matterpoll can create the
	// generated post as a real thread reply via callback_id.
	if (rootId.isEmpty()) {
		if (const auto* creator = qobject_cast<const OutgoingPostCreator*>(parent)) {
			rootId = creator->rootId();
		}
	}

	connect (ui->questionValue, &QLineEdit::textChanged, this, &NewPollDialog::validateInput);
	connect (ui->option1Value, &QLineEdit::textChanged, this, &NewPollDialog::validateInput);
	connect (ui->option2Value, &QLineEdit::textChanged, this, &NewPollDialog::validateInput);
	connect (ui->option3Value, &QLineEdit::textChanged, this, &NewPollDialog::validateInput);

	ui->questionValue->setText (initialPollData.question);

	QLineEdit* optionsLineEditArr[] = { ui->option1Value, ui->option2Value, ui->option3Value };
	const int optionsCount = qMin<int>(initialPollData.options.size(), 3);
	for (int i = 0; i < optionsCount; ++i) {
		optionsLineEditArr[i]->setText (initialPollData.options[i]);
	}

	ui->checkBoxAnonymous->setChecked(initialPollData.isAnonymous);
	ui->checkBoxAnonymousCreator->setChecked(initialPollData.isAnonymousCreator);
	ui->checkBoxProgress->setChecked(initialPollData.showProgress);
	ui->checkBoxAllowAdditional->setChecked(initialPollData.allowAddOptions);
	ui->maxVotesValue->setValue(initialPollData.maxVotes);

	validateInput ();
	setAttribute(Qt::WA_DeleteOnClose);
}

NewPollDialog::~NewPollDialog()
{
    delete ui;
}

void NewPollDialog::validateInput ()
{
	const bool hasThirdOption = !ui->option3Value->text().trimmed().isEmpty();
	ui->maxVotesValue->setMaximum(hasThirdOption ? 3 : 2);

	if (ui->questionValue->text().trimmed().isEmpty()) {
		return disableSendButton (tr("'Question' is empty"));
	}

	if (ui->option1Value->text().trimmed().isEmpty()) {
		return disableSendButton (tr("'Option 1' is empty"));
	}

	if (ui->option2Value->text().trimmed().isEmpty()) {
		return disableSendButton (tr("'Option 2' is empty"));
	}

	auto okButton = ui->buttonBox->button(QDialogButtonBox::Ok);
	okButton->setEnabled(true);
	okButton->setToolTip ("");
}

void NewPollDialog::disableSendButton (const QString& tooltip)
{
	auto okButton = ui->buttonBox->button(QDialogButtonBox::Ok);
	okButton->setEnabled (false);
	okButton->setToolTip (tooltip);
}

BackendNewPollData NewPollDialog::getData ()
{
	BackendNewPollData ret;
	ret.question = ui->questionValue->text();
	ret.options.push_back (ui->option1Value->text());
	ret.options.push_back (ui->option2Value->text());

	//option 3 is not mandatory
	if (!ui->option3Value->text().trimmed().isEmpty()) {
		ret.options.push_back (ui->option3Value->text());
	}

	ret.rootId = rootId;
	ret.isAnonymous = ui->checkBoxAnonymous->isChecked();
	ret.isAnonymousCreator = ui->checkBoxAnonymousCreator->isChecked();
	ret.showProgress = ui->checkBoxProgress->isChecked();
	ret.allowAddOptions = ui->checkBoxAllowAdditional->isChecked();
	ret.maxVotes = ui->maxVotesValue->value();

	return ret;
}


} /* namespace Mattermost */
