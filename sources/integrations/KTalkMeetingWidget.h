/**
 * @file KTalkMeetingWidget.h
 * @brief Native rendering for KTalk meeting posts.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QFrame>

class QEvent;
class QLabel;
class QPushButton;

namespace Mattermost {

class Backend;
class BackendPost;

/**
 * Displays the structured metadata carried by a KTalk meeting post.
 *
 * The meeting itself remains server-owned. This widget only presents the
 * advertised link/id/topic and reuses the dynamically discovered integration
 * icon when one is available.
 */
class KTalkMeetingWidget final : public QFrame
{
    Q_OBJECT
public:
    static bool supports(const BackendPost& post);

    explicit KTalkMeetingWidget(Backend& backend,
                                const BackendPost& post,
                                QWidget* parent = nullptr);

    /** True when the post contains enough metadata for the native card. */
    bool isValid() const { return valid_; }

protected:
    void changeEvent(QEvent* event) override;

private:
    void refreshAccent();
    void refreshIcon();
    void openMeeting();

    Backend& backend_;
    QString meetingLink_;
    QFrame* accentBar_ = nullptr;
    QLabel* iconLabel_ = nullptr;
    QPushButton* joinButton_ = nullptr;
    bool valid_ = false;
};

} // namespace Mattermost
