#include <QtTest>

#include "backend/PostUnreadRequest.h"

using namespace Mattermost;

class PostUnreadRequestTest : public QObject
{
    Q_OBJECT

private slots:
    void pathAndCrtCapabilityMatchMattermostApi()
    {
        QCOMPARE(postUnreadPath(QStringLiteral("user"), QStringLiteral("post")),
                 QStringLiteral("users/user/posts/post/set_unread"));
        QCOMPARE(postUnreadPayload(true).value(QStringLiteral("collapsed_threads_supported")).toBool(),
                 true);
        QCOMPARE(postUnreadPayload(false).value(QStringLiteral("collapsed_threads_supported")).toBool(),
                 false);
    }
};

QTEST_MAIN(PostUnreadRequestTest)
#include "PostUnreadRequestTest.moc"
