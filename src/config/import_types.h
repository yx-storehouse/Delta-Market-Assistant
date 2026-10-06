#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace relink::config {

// Both adapters produce import-preview.schema.json documents. A preview is never
// an executable configuration; only ProfileStore materializes reviewed copies.
struct ImportOptions {
    QString container = QStringLiteral("text"); // text or ini, chosen explicitly
    QString encoding = QStringLiteral("UTF-8"); // or GB18030, chosen explicitly
};

inline QString sourceHash(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline bool previewHasErrors(const QJsonObject& preview) {
    if (preview.value(QStringLiteral("kind")).toString() != QStringLiteral("LegacyImportPreview")) return true;
    auto errors = [](const QJsonArray& items) {
        for (const auto& item : items)
            if (item.toObject().value(QStringLiteral("severity")).toString() == QStringLiteral("error")) return true;
        return false;
    };
    if (errors(preview.value(QStringLiteral("diagnostics")).toArray())) return true;
    for (const auto& rowValue : preview.value(QStringLiteral("rows")).toArray()) {
        const auto row = rowValue.toObject();
        if (row.value(QStringLiteral("status")).toString() == QStringLiteral("invalid")
            || errors(row.value(QStringLiteral("diagnostics")).toArray())) return true;
    }
    return false;
}

QJsonObject previewV1(const QByteArray& bytes);
QJsonObject preview13Columns(const QByteArray& bytes, const ImportOptions& options = {});

struct ReviewChoices {
    QString profileName;
    QString unitDisplayName;
    // User supplies a plain decimal quantum, e.g. 0.01; never parsed via double.
    QString quantum = QStringLiteral("0.01");
    bool confirmProducts = false;
    bool confirmPriceRange = false;
    bool confirmTaxonomy = false;
    bool preserveUnknownColumns = false;
    bool keepQuantityUnbound = false;
};

} // namespace relink::config
