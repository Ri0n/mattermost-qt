/**
 * @file HTTPConnector.h
 * @brief
 * @author Lyubomir Filipov
 * @date Nov 29, 2021
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

#pragma once

#include <memory>

#include <QNetworkReply>
#include <QQueue>
#include <QSet>

#include "backend/HttpResponseCallback.h"
#include "backend/types/BackendError.h"

class QNetworkAccessManager;

namespace Mattermost {

class QByteArrayCreator;

class HTTPConnector: public QObject {
	Q_OBJECT
public:
	HTTPConnector ();
	virtual ~HTTPConnector ();

	void reset ();

	void get (QNetworkRequest &request, HttpResponseCallback responseHandler);
	void post (QNetworkRequest &request, const QByteArrayCreator &data, HttpResponseCallback responseHandler);
	void put (QNetworkRequest &request, const QByteArrayCreator &data, HttpResponseCallback responseHandler);
	void del (QNetworkRequest &request);

signals:
	void onNetworkError (uint32_t errorNumber, const QString& errorText);
	void onHttpError (uint32_t errorNumber, const QString& errorText);

private:
	enum class Method {
		Get,
		Post,
		Put,
		Delete,
	};

	struct PendingRequest {
		Method method = Method::Get;
		QNetworkRequest request;
		QByteArray data;
		bool jsonData = false;
		HttpResponseCallback responseHandler;
	};

	void enqueue (PendingRequest request);
	static void processQueues ();
	static HTTPConnector* connectorWithPendingRequest (bool lowPriority);
	void startRequest (PendingRequest request);
	virtual void setProcessReply (QNetworkReply* reply,
		std::function<void(QVariant,QByteArray,const QNetworkReply&)> responseHandler,
		quint64 requestGeneration);

	static constexpr int MaxConcurrentRequests = 6;
	static QSet<HTTPConnector*> connectors;
	static int globalActiveRequests;

	std::unique_ptr<QNetworkAccessManager> qnetworkManager;
	QQueue<PendingRequest> highPriorityRequests;
	QQueue<PendingRequest> lowPriorityRequests;
	int activeRequests = 0;
	quint64 generation = 0;
};

} /* namespace Mattermost */
