#pragma once

#include <QString>
#include <QtGlobal>

class QTextDocument;

namespace Mattermost {
namespace MessageFormatter {

QString formatMessageText(const QString& text);

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
void buildMarkdownDocument(QTextDocument& document, const QString& text);
#endif

} // namespace MessageFormatter
} // namespace Mattermost
