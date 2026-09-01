/**
 * @file WebSocketConnector.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 28, 2021
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

#include "WebSocketConnector.h"

#include <iostream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtWebSockets/QWebSocket>

#include "backend/WebSocketEventHandler.h"
#include "log.h"

namespace Mattermost {

namespace {

constexpr int HeartbeatIntervalMs = 30000;
constexpr int MinReconnectDelayMs = 3000;
constexpr int MaxReconnectDelayMs = 300000;
constexpr int ReconnectJitterMs = 2000;
constexpr int BackoffThreshold = 7;

template<typename T>
void handler (WebSocketConnector& conn, const QJsonObject& data, const QJsonObject& broadcast)
{
	conn.eventHandler.handleEvent (T (data, broadcast));
}

const QMap<QString, void(*)(WebSocketConnector&, const QJsonObject&, const QJsonObject&)> eventHandlers {
	{"hello", [] (WebSocketConnector&, const QJsonObject&, const QJsonObject&) {
		std::cout << "Hello" << std::endl;
	}},
	{"channel_viewed",		handler<ChannelViewedEvent>},
	{"posted", 				handler<PostEvent>},
	{"post_edited", 		handler<PostEditedEvent>},
	{"post_deleted",		handler<PostDeletedEvent>},
	{"reaction_added",		handler<PostReactionAddedEvent>},
	{"reaction_removed",	handler<PostReactionRemovedEvent>},
	{"typing",				handler<TypingEvent>},
	{"status_change", 		handler<StatusChangeEvent>},
	{"direct_added", 		handler<NewDirectChannelEvent>},		//new direct channel created
	{"new_user",			handler<NewUserEvent>},				//user added to the server
	{"user_updated",		handler<UserUpdatedEvent>},			//user data updated
	{"user_added",			handler<UserAddedToChannelEvent>},		//user added to channel
	{"added_to_team",		handler<UserAddedToTeamEvent>},			//user added to team
	{"leave_team",			handler<UserLeaveTeamEvent>},			//a user has left a team
	{"user_removed",		handler<UserRemovedFromChannelEvent>},	//a user (the logged-in user, or someone else) was removed from a channel
	{"channel_created",		handler<ChannelCreatedEvent>},			//a new channel was created
	{"channel_updated",		handler<ChannelUpdatedEvent>},			//a channel was updated
	{"open_dialog",			handler<OpenDialogEvent>},				//a server-side dialog
};

bool printEvent (const QString& name)
{
	if (	name == "channel_viewed" 	||
			name == "channel_updated" 	||
			name == "reaction_added" 	||
			name == "status_change" 	||
			name == "posted" 			||
			//name == "typing" 			||
			name == "reaction_removed"	||
			name == "user_removed"		||
			name == "user_updated"		||
			name == "leave_team"
	) {
		return false;
	}

	return true;
}

} // namespace

struct WebSocketConnector::Private {
	QWebSocket webSocket;
	QString token;
	QUrl endpointUrl;
	QTimer heartbeatTimer;
	QTimer reconnectTimer;
	QString connectionId;
	int responseSequence = 1;
	int serverSequence = 0;
	int pendingPingSequence = 0;
	int reconnectAttempt = 0;
	bool waitingForPong = false;
	bool hasReconnect = false;
	bool helloReceived = false;
	bool suppressReconnect = false;
};

WebSocketConnector::WebSocketConnector (WebSocketEventHandler& eventHandler)
:eventHandler (eventHandler)
,d (std::make_unique<Private>())
{
	auto errorHandler = [this] (QAbstractSocket::SocketError error) {
		LOG_DEBUG ("WebSocket error " << error << " " << d->webSocket.errorString());
		if (!d->suppressReconnect && d->webSocket.state() == QAbstractSocket::UnconnectedState) {
			scheduleReconnect ();
		}
	};
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	connect (&d->webSocket, &QWebSocket::errorOccurred, this, errorHandler);
#else
	connect (&d->webSocket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, errorHandler);
#endif

	connect (&d->webSocket, &QWebSocket::connected, this, [this] {
		LOG_DEBUG ("WebSocket connected");
		d->reconnectTimer.stop ();
		d->responseSequence = 1;
		d->helloReceived = false;
		doHandshake ();
		startHeartbeat ();
	});

	connect (&d->webSocket, &QWebSocket::disconnected, this, [this] {
		stopHeartbeat ();
		d->responseSequence = 1;

		const bool reconnectSuppressed = d->suppressReconnect;
		d->suppressReconnect = false;

		LOG_DEBUG ("WebSocket disconnected. Code: " << d->webSocket.closeCode() << " " << d->webSocket.closeReason());
		emit onDisconnect ();
		d->helloReceived = false;

		if (!reconnectSuppressed && !d->token.isEmpty()) {
			scheduleReconnect ();
		}
	});

	connect (&d->webSocket, &QWebSocket::textMessageReceived,
			 this, &WebSocketConnector::onNewPacket);

	d->heartbeatTimer.setInterval (HeartbeatIntervalMs);
	connect (&d->heartbeatTimer, &QTimer::timeout, this, &WebSocketConnector::sendPing);

	d->reconnectTimer.setSingleShot (true);
	connect (&d->reconnectTimer, &QTimer::timeout, this, [this] {
		if (d->token.isEmpty() || d->suppressReconnect) {
			return;
		}
		if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
			return;
		}

		LOG_DEBUG ("WebSocket Reconnecting (connection_id=" << d->connectionId
				   << ", sequence_number=" << d->serverSequence << ")");
		openSocket ();
	});
}

WebSocketConnector::~WebSocketConnector () = default;

void WebSocketConnector::open (const QString& urlString, const QString& authToken)
{
	d->endpointUrl = QUrl (urlString + "websocket");
	d->endpointUrl.setScheme ("wss");

	d->token = authToken;
	d->connectionId.clear ();
	d->responseSequence = 1;
	d->serverSequence = 0;
	d->pendingPingSequence = 0;
	d->reconnectAttempt = 0;
	d->waitingForPong = false;
	d->hasReconnect = false;
	d->helloReceived = false;
	d->suppressReconnect = false;
	d->reconnectTimer.stop ();
	stopHeartbeat ();

	openSocket ();
}

void WebSocketConnector::close ()
{
	d->token.clear ();
	reset ();
}

void WebSocketConnector::scheduleReconnect ()
{
	stopHeartbeat ();

	if (d->token.isEmpty() || d->suppressReconnect || d->reconnectTimer.isActive()) {
		return;
	}
	if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
		return;
	}

	d->hasReconnect = true;
	++d->reconnectAttempt;

	int delay = MinReconnectDelayMs;
	if (d->reconnectAttempt > BackoffThreshold) {
		const qint64 scaledDelay = static_cast<qint64>(MinReconnectDelayMs)
			* d->reconnectAttempt * d->reconnectAttempt;
		delay = static_cast<int>(qMin<qint64>(scaledDelay, MaxReconnectDelayMs));
	}
	delay += static_cast<int>(QRandomGenerator::global()->bounded(static_cast<quint32>(ReconnectJitterMs)));

	LOG_DEBUG ("WebSocket reconnect scheduled in " << delay << " ms");
	d->reconnectTimer.start (delay);
}

QUrl WebSocketConnector::socketUrl () const
{
	QUrl url (d->endpointUrl);
	QUrlQuery query (url);
	query.removeAllQueryItems (QStringLiteral("connection_id"));
	query.removeAllQueryItems (QStringLiteral("sequence_number"));
	query.addQueryItem (QStringLiteral("connection_id"), d->connectionId);
	query.addQueryItem (QStringLiteral("sequence_number"), QString::number(d->serverSequence));
	url.setQuery (query);
	return url;
}

void WebSocketConnector::openSocket ()
{
	if (d->token.isEmpty() || d->endpointUrl.isEmpty()) {
		return;
	}

	const QUrl url = socketUrl ();
	LOG_DEBUG ("WebSocket opening " << url.toString(QUrl::RemovePassword));
	d->webSocket.open (url);
}

void WebSocketConnector::doHandshake ()
{
	QJsonObject jsonData {
		{"token", d->token},
	};

	const int sequence = d->responseSequence++;
	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "authentication_challenge"},
		{"data", jsonData},
	});

	d->webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::startHeartbeat ()
{
	stopHeartbeat ();
	sendPing ();
	d->heartbeatTimer.start ();
}

void WebSocketConnector::stopHeartbeat ()
{
	d->heartbeatTimer.stop ();
	d->waitingForPong = false;
	d->pendingPingSequence = 0;
}

void WebSocketConnector::sendPing ()
{
	if (d->webSocket.state() != QAbstractSocket::ConnectedState) {
		return;
	}

	// Mattermost's web client uses an application-level websocket action named
	// "ping". It does not rely on RFC6455 control-frame ping/pong for its
	// liveness check. The response is a normal {status, seq_reply} packet.
	if (d->waitingForPong) {
		LOG_DEBUG ("Mattermost WebSocket ping received no response within "
				   << HeartbeatIntervalMs << " ms. Reconnecting");
		d->webSocket.abort ();
		return;
	}

	const int sequence = d->responseSequence++;
	d->pendingPingSequence = sequence;
	d->waitingForPong = true;

	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "ping"},
	});
	d->webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::reset ()
{
	d->reconnectTimer.stop ();
	stopHeartbeat ();
	d->connectionId.clear ();
	d->responseSequence = 1;
	d->serverSequence = 0;
	d->reconnectAttempt = 0;
	d->hasReconnect = false;
	d->helloReceived = false;

	if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
		d->suppressReconnect = true;
		d->webSocket.close (QWebSocketProtocol::CloseCodeNormal, QStringLiteral("Client Close"));
	} else {
		d->suppressReconnect = false;
	}
}

void WebSocketConnector::onNewPacket (const QString& string)
{
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson (string.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		LOG_DEBUG ("Invalid WebSocket JSON: " << parseError.errorString());
		return;
	}

	const QJsonObject jsonObject = doc.object ();

	// Replies to websocket actions use the request's sequence number and are
	// separate from the server event stream sequence.
	const QJsonValue seqReply = jsonObject.value (QStringLiteral("seq_reply"));
	if (!seqReply.isUndefined()) {
		const int replySequence = seqReply.toInt ();
		if (d->waitingForPong && replySequence == d->pendingPingSequence) {
			d->waitingForPong = false;
			d->pendingPingSequence = 0;
		}

		if (jsonObject.contains(QStringLiteral("error"))) {
			LOG_DEBUG ("WebSocket action failed: " << doc.toJson(QJsonDocument::Compact));
		}
		return;
	}

	const QString eventName = jsonObject.value (QStringLiteral("event")).toString ();

	// A hello packet identifies the reliable websocket stream. On reconnect the
	// server reuses connection_id and resumes from sequence_number when its
	// backlog is still available. A different id means replay was not possible;
	// reset the expected sequence and let Backend's reconnect sync fill the gap.
	if (eventName == QStringLiteral("hello")) {
		const QString newConnectionId = jsonObject.value(QStringLiteral("data"))
			.toObject().value(QStringLiteral("connection_id")).toString();
		if (!d->connectionId.isEmpty() && !newConnectionId.isEmpty()
			&& d->connectionId != newConnectionId) {
			LOG_DEBUG ("Mattermost started a new WebSocket stream (old connection_id="
					   << d->connectionId << ", new connection_id=" << newConnectionId
					   << "). Falling back to HTTP resync");
			d->serverSequence = 0;
		}
		if (!newConnectionId.isEmpty()) {
			d->connectionId = newConnectionId;
		}
	}

	const QJsonValue eventSequenceValue = jsonObject.value (QStringLiteral("seq"));
	if (!eventSequenceValue.isUndefined()) {
		const int eventSequence = eventSequenceValue.toInt (-1);
		if (eventSequence != d->serverSequence) {
			LOG_DEBUG ("Missed WebSocket event: received seq=" << eventSequence
					   << ", expected seq=" << d->serverSequence
					   << ". Reconnecting with reliable sequence recovery");
			d->webSocket.abort ();
			return;
		}
		d->serverSequence = eventSequence + 1;
	}

	if (eventName == QStringLiteral("hello") && !d->helloReceived) {
		d->helloReceived = true;
		d->reconnectAttempt = 0;
		const bool reconnected = d->hasReconnect;
		d->hasReconnect = false;
		emit onConnect (reconnected);
	}

	auto it = eventHandlers.find (eventName);
	if (it == eventHandlers.end()) {
		//sometimes MM instance keeps spamming custom_profile_attributes_values_updated
		//custom profile attributes will not be supported in the near future
		if (eventName != QStringLiteral("custom_profile_attributes_values_updated")) {
			LOG_DEBUG ("Unhandled WebSocket event '" << eventName << "'\n");
			const QString jsonString = doc.toJson (QJsonDocument::Indented);
			std::cout << jsonString.toStdString ();
			qDebug() << "========" << '\n';
		}
		return;
	}

	if (printEvent (it.key())) {
		qDebug() << "========" << '\n';
		const QString jsonString = doc.toJson (QJsonDocument::Indented);
		std::cout << jsonString.toStdString ();
	}

	it.value() (*this,
				jsonObject.value (QStringLiteral("data")).toObject(),
				jsonObject.value (QStringLiteral("broadcast")).toObject());
}

} /* namespace Mattermost */
