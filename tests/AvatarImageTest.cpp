#include <QtTest>

#include <QBuffer>
#include <QImage>

#include "backend/AvatarImage.h"

using namespace Mattermost;

class AvatarImageTest : public QObject
{
    Q_OBJECT
private slots:
    void decodeKeepsSourceResolution()
    {
        QImage source(320, 180, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(QStringLiteral("#4b7bec")));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));

        const QPixmap decoded = decodeAvatarImage(bytes);
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.size(), source.size());
        QVERIFY(decoded.width() > 128);
    }
};

QTEST_MAIN(AvatarImageTest)
#include "AvatarImageTest.moc"
