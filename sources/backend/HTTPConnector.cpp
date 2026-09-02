/**
 * @file HTTPConnector.cpp
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

#include "HTTPConnector.h"

#include <QAbstractNetworkCache>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>

#include "QByteArrayCreator.h"
#include "Settings.h"
#include "log.h"

namespace Mattermost {

static QNetworkDiskCache* createDiskCache ()
{
	QNetworkDiskCache* diskCache = new QNetworkDiskCache ();
	diskCache->setCacheDirectory (QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

	QSettings settings;
	unsigned cacheSizeMB = settings.value(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT).toUInt();

	diskCache->setMaximumCacheSize (cacheSizeMB * 1024 * 1024);
	return diskCache;
}

HTTPConnector::HTTPConnector ()
:qnetworkManager (std::make_unique <QNetworkAccessManager> ())
{
	// qnetworkManager takes ownership over the disk cache.
	qnetworkManager->setCache (createDiskCache ());
	qnetworkManager->setAutoDeleteReplies(true);
}

HTTPConnector::~HTTPConnector () = default;

void HTTPConnector::reset ()
{
	// Invalidate callbacks from requests owned by the old network manager and
	// discard work that has not started yet. This is important on websocket
	// reconnect, where Backend deliberately resets the HTTP side.
	++generation;
	highPriorityRequests.clear ();
	lowPriorityRequests.clear ();
	activeRequests = 0;

	// qnetworkManager takes ownership over the disk cache.
	qnetworkManager.reset(new QNetworkAccessManager());
	qnetworkManager->setCache (createDiskCache ());
	qnetworkManager->setAutoDeleteReplies(true);
}

void HTTPConnector::get (QNetworkRequest& request, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

	enqueue(PendingRequest {
		Method::Get,
		request,
		QByteArray(),
		false,
		std::move(responseHandler),
	});
}

void HTTPConnector::post (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	enqueue(PendingRequest {
		Method::Post,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
	});
}

void HTTPConnector::put (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	enqueue(PendingRequest {
		Method::Put,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
	});
}

void HTTPConnector::del (QNetworkRequest& request)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	enqueue(PendingRequest {
		Method::Delete,
		request,
		QByteArray(),
		false,
		HttpResponseCallback([](QVariant, QByteArray, const QNetworkReply&) {}),
	});
}

void HTTPConnector::enqueue(PendingRequest request)
{
	if (request.request.priority() == QNetworkRequest::LowPriority) {
		lowPriorityRequests.enqueue(std::move(request));
	} else {
		highPriorityRequests.enqueue(std::move(request));
	}
	processQueue();
}

void HTTPConnector::processQueue()
{
	while (activeRequests < MaxConcurrentRequests
	       && (!highPriorityRequests.isEmpty() || !lowPriorityRequests.isEmpty())) {
		PendingRequest request = !highPriorityRequests.isEmpty()
			? highPriorityRequests.dequeue()
			: lowPriorityRequests.dequeue();
		startRequest(std::move(request));
	}
}

void HTTPConnector::startRequest(PendingRequest request)
{
	if (request.jsonData) {
		request.request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	}

	QNetworkReply* reply = nullptr;
	switch (request.method) {
	case Method::Get:
		reply = qnetworkManager->get(request.request);
		break;
	case Method::Post:
		reply = qnetworkManager->post(request.request, request.data);
		break;
	case Method::Put:
		reply = qnetworkManager->put(request.request, request.data);
		break;
	case Method::Delete:
		reply = qnetworkManager->deleteResource(request.request);
		break;
	}

	++activeRequests;
	setProcessReply(reply, std::move(request.responseHandler), generation);
}

void HTTPConnector::setProcessReply (QNetworkReply* reply,
		std::function<void (QVariant, QByteArray, const QNetworkReply&)> responseHandler,
		quint64 requestGeneration)
{
	connect(reply, &QNetworkReply::finished, this,
		[this, reply, responseHandler = std::move(responseHandler), requestGeneration]() mutable {
			const int statusCode = reply->error();
			auto data = reply->readAll();

			// Keep the historical callback behavior: callers receive the response
			// regardless of HTTP/network status and decide how to interpret it.
			responseHandler(statusCode, qMove(data), *reply);
			reply->deleteLater();

			// reset() replaces the network manager and starts a new request
			// generation. Replies from the old generation must not alter the new
			// queue's accounting.
			if (requestGeneration != generation) {
				return;
			}

			if (activeRequests > 0) {
				--activeRequests;
			}
			processQueue();
		});

#if QT_VERSION <= QT_VERSION_CHECK(5,15,0)
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::error),
#else
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::errorOccurred),
#endif
			this, [this, reply](QNetworkReply::NetworkError error) {

		emit onNetworkError (error, reply->errorString());
	});
}

} /* namespace Mattermost */
