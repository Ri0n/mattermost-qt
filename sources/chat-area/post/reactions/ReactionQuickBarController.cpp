#include <algorithm>
#include <cstring>

#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/emoji/EmojiInfo.h"
#include "chat-area/ChatArea.h"
#include "chat-area/post/PostWidget.h"
#include "reactions/ReactionUsage.h"
#include "reactions/ReactionUsageTracker.h"
#include "ui/EmojiPresentation.h"

namespace Mattermost {
namespace {

QString customEmojiSource(const QString& presentation)
{
    static const QRegularExpression sourceExpression(
        QStringLiteral(R"(\bsrc\s*=\s*["']([^"']+)["'])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = sourceExpression.match(presentation);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString pixmapPath(QString source)
{
    if (source.startsWith(QStringLiteral("qrc://"))) {
        source = QStringLiteral(":/") + source.mid(6);
    }
    return EmojiPresentation::imagePath(source);
}

bool reactionIsRenderable(const QString& name)
{
    const EmojiID id = EmojiInfo::findByName(name);
    if (!id) {
        return false;
    }

    const Emoji emoji = EmojiInfo::getEmoji(id);
    const QString source = customEmojiSource(emoji.unicodeString);
    if (source.isEmpty()) {
        return !emoji.unicodeString.trimmed().isEmpty();
    }
    return !QPixmap(pixmapPath(source)).isNull();
}

QStringList renderableNames(const QStringList& names)
{
    QStringList result;
    result.reserve(names.size());
    for (const QString& name : names) {
        if (reactionIsRenderable(name)) {
            result.push_back(name);
        }
    }
    return result;
}

bool configureReactionButton(QPushButton& button, const QString& name)
{
    const EmojiID id = EmojiInfo::findByName(name);
    if (!id) {
        return false;
    }

    const Emoji emoji = EmojiInfo::getEmoji(id);
    const QString source = customEmojiSource(emoji.unicodeString);
    if (!source.isEmpty()) {
        const QPixmap pixmap(pixmapPath(source));
        if (pixmap.isNull()) {
            return false;
        }
        button.setIcon(QIcon(pixmap));
        button.setIconSize(QSize(20, 20));
    } else {
        const QString text = emoji.unicodeString.trimmed();
        if (text.isEmpty()) {
            return false;
        }
        button.setText(text);
        button.setFont(EmojiPresentation::fontForMode(
            button.font(), EmojiPresentation::Mode::Reaction));
    }

    button.setToolTip(QStringLiteral(":%1:").arg(name));
    button.setAccessibleName(QObject::tr("React with :%1:").arg(name));
    return true;
}

QSet<QString> stringSet(const QStringList& values)
{
    QSet<QString> result;
    for (const QString& value : values) {
        result.insert(value);
    }
    return result;
}

QStringList storedFavoriteNames(const QByteArray& data)
{
    QStringList names;
    using ByteSize = decltype(data.size());
    const ByteSize stride = static_cast<ByteSize>(sizeof(EmojiID));
    if (data.isEmpty() || data.size() % stride != 0) {
        return names;
    }

    for (ByteSize offset = 0; offset < data.size(); offset += stride) {
        EmojiID id {0, 0};
        std::memcpy(&id, data.constData() + offset, sizeof(id));
        const Emoji emoji = EmojiInfo::getEmoji(id);
        if (!emoji.name.isEmpty()) {
            names.push_back(emoji.name);
        }
    }
    return names;
}

void saveFavoriteNames(QSettings& settings, const QStringList& names)
{
    QVector<EmojiID> ids;
    ids.reserve(names.size());
    for (const QString& name : names) {
        const EmojiID id = EmojiInfo::findByName(name);
        if (id) {
            ids.push_back(id);
        }
    }

    const QByteArray data(reinterpret_cast<const char*>(ids.constData()),
                          ids.size() * static_cast<int>(sizeof(EmojiID)));
    settings.setValue(QStringLiteral("emoji_favorites"), data);
}

void ensureFourDefaultFavorites()
{
    QSettings settings;
    const QString key = QStringLiteral("emoji_favorites");
    const QStringList desired = defaultReactionFavorites();

    if (!settings.contains(key)) {
        saveFavoriteNames(settings, desired);
        return;
    }

    const QStringList legacyDefaults = {
        QStringLiteral("+1"),
        QStringLiteral("pray"),
        QStringLiteral("brain"),
        QStringLiteral("smiley"),
        QStringLiteral("rolling_on_the_floor_laughing"),
        QStringLiteral("sunglasses"),
        QStringLiteral("mask"),
        QStringLiteral("face_vomiting"),
        QStringLiteral("yawning_face"),
        QStringLiteral("cherries"),
        QStringLiteral("pizza"),
        QStringLiteral("warning"),
        QStringLiteral("radioactive_sign"),
        QStringLiteral("white_check_mark"),
        QStringLiteral("heavy_check_mark"),
    };

    const QStringList stored = storedFavoriteNames(settings.value(key).toByteArray());
    if (stored.size() == legacyDefaults.size()
        && stringSet(stored) == stringSet(legacyDefaults)) {
        // Migrate the old untouched default list, but preserve a user's custom
        // favorites even if they were created by an older version.
        saveFavoriteNames(settings, desired);
    }
}

} // namespace

class ReactionQuickBarController : public QObject
{
public:
    static ReactionQuickBarController& instance()
    {
        static ReactionQuickBarController controller;
        return controller;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        const QEvent::Type type = event->type();
        if (type != QEvent::Enter && type != QEvent::Leave
            && type != QEvent::MouseButtonPress && type != QEvent::Destroy
            && type != QEvent::Show && type != QEvent::Move) {
            return QObject::eventFilter(watched, event);
        }

        if (watched == popup_.data()) {
            if (type == QEvent::Enter) {
                hideTimer_.stop();
            } else if (type == QEvent::Leave) {
                scheduleHide();
            }
            return QObject::eventFilter(watched, event);
        }

        auto* button = qobject_cast<QPushButton*>(watched);
        auto* post = button ? qobject_cast<PostWidget*>(button->parentWidget()) : nullptr;
        if (!post || button != post->reactionAffordance_) {
            return QObject::eventFilter(watched, event);
        }

        if (type == QEvent::Show || type == QEvent::Move) {
            positionThreadAffordance(*post, *button);
            return QObject::eventFilter(watched, event);
        }

        if (type == QEvent::Enter) {
            positionThreadAffordance(*post, *button);
            hideTimer_.stop();
            showFor(*post, *button);
        } else if (type == QEvent::Leave) {
            scheduleHide();
        } else if (type == QEvent::MouseButtonPress) {
            // Clicking the heart opens the complete emoji chooser. Do not leave
            // the hover strip floating behind that dialog.
            hidePopup();
        } else if (type == QEvent::Destroy) {
            hidePopup();
        }

        return QObject::eventFilter(watched, event);
    }

private:
    ReactionQuickBarController()
    {
        hideTimer_.setSingleShot(true);
        hideTimer_.setInterval(180);
        connect(&hideTimer_, &QTimer::timeout, this,
                [this] { hidePopup(); });
    }

    void positionThreadAffordance(PostWidget& post, QPushButton& heart)
    {
        if (!post.parentChatArea || !post.parentChatArea->isThread) {
            return;
        }

        QLabel* timeLabel = post.findChild<QLabel*>(QStringLiteral("time"));
        if (!timeLabel || timeLabel->text().isEmpty()) {
            return;
        }

        const QFontMetrics metrics = timeLabel->fontMetrics();
        const QSize textSize(metrics.horizontalAdvance(timeLabel->text()),
                             metrics.height());
        const QRect textRect = QStyle::alignedRect(
            timeLabel->layoutDirection(), timeLabel->alignment(), textSize,
            timeLabel->contentsRect());
        const QPoint textTopLeft = timeLabel->mapTo(&post, textRect.topLeft());

        const QPoint position(
            std::max(4, textTopLeft.x() - heart.width() - 4),
            std::max(2, textTopLeft.y()
                            + (textRect.height() - heart.height()) / 2));
        if (heart.pos() != position) {
            heart.move(position);
        }
    }

    void scheduleHide()
    {
        if (popup_) {
            hideTimer_.start();
        }
    }

    void hidePopup()
    {
        hideTimer_.stop();
        if (popup_) {
            popup_->hide();
            popup_->deleteLater();
        }
        popup_.clear();
        activePost_.clear();
        activeHeart_.clear();
    }

    void showFor(PostWidget& post, QPushButton& heart)
    {
        if (post.post.isDeleted) {
            hidePopup();
            return;
        }

        const QStringList popular = renderableNames(
            ReactionUsageTracker::instance().topNames(10));
        const QStringList favorites = renderableNames(defaultReactionFavorites());
        const QStringList quickNames = selectQuickReactionNames(popular, favorites);
        if (quickNames.isEmpty()) {
            hidePopup();
            return;
        }

        if (activePost_ == &post && activeHeart_ == &heart && popup_) {
            return;
        }

        hidePopup();
        activePost_ = &post;
        activeHeart_ = &heart;

        // Keep the quick bar inside the post's widget hierarchy. A top-level
        // Qt::Tool window cannot be positioned reliably on Wayland: the
        // compositor owns its placement and may ignore QWidget::move(), which
        // made the bar appear near the middle of the screen instead of next to
        // the heart. Local widget coordinates are deterministic on every
        // windowing system and the bar also follows the post while it moves.
        auto* popup = new QFrame(&post);
        popup_ = popup;
        popup->setFrameShape(QFrame::StyledPanel);
        popup->setFrameShadow(QFrame::Raised);
        popup->setAutoFillBackground(true);

        auto* layout = new QHBoxLayout(popup);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->setSpacing(2);

        QPointer<PostWidget> postGuard(&post);
        for (const QString& name : quickNames) {
            auto* reaction = new QPushButton(popup);
            reaction->setFlat(true);
            reaction->setFixedSize(28, 28);
            reaction->setCursor(Qt::PointingHandCursor);
            if (!configureReactionButton(*reaction, name)) {
                delete reaction;
                continue;
            }
            layout->addWidget(reaction);
            connect(reaction, &QPushButton::clicked, popup,
                    [this, postGuard, name] {
                if (postGuard && !postGuard->post.isDeleted) {
                    postGuard->getBackend().addPostReaction(postGuard->post.id, name);
                }
                hidePopup();
            });
        }

        if (layout->count() == 0) {
            hidePopup();
            return;
        }

        connect(&post, &QObject::destroyed, popup, [this] {
            if (!activePost_) {
                hidePopup();
            }
        });

        popup->adjustSize();
        const QPoint heartPosition = heart.mapTo(&post, QPoint(0, 0));
        const int rightX = heartPosition.x() + heart.width() + 4;
        int x = heartPosition.x() - popup->width() - 4;
        if (x < 0 && rightX + popup->width() <= post.width()) {
            x = rightX;
        }

        const int maxX = std::max(0, post.width() - popup->width());
        const int maxY = std::max(0, post.height() - popup->height());
        x = std::max(0, std::min(x, maxX));
        const int y = std::max(
            0,
            std::min(heartPosition.y() + (heart.height() - popup->height()) / 2,
                     maxY));

        popup->move(x, y);
        popup->show();
        popup->raise();
    }

    QTimer hideTimer_;
    QPointer<QFrame> popup_;
    QPointer<PostWidget> activePost_;
    QPointer<QPushButton> activeHeart_;
};

namespace {

void installReactionQuickBarController()
{
    QCoreApplication* application = QCoreApplication::instance();
    if (!application) {
        return;
    }

    application->installEventFilter(&ReactionQuickBarController::instance());

    // Do not touch arbitrary QSettings stores used by unit-test executables.
    if (QCoreApplication::organizationName() == QStringLiteral("mattermost-native")
        && QCoreApplication::applicationName() == QStringLiteral("Mattermost")) {
        ensureFourDefaultFavorites();
    }
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installReactionQuickBarController)

} // namespace Mattermost
