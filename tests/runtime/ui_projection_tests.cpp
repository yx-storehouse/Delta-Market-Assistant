#include "application/runtime/ui_projection.h"
#include "config/import_types.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfoList>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

using relink::config::preview13Columns;
using relink::runtime::ImportPreviewUiProjection;
using relink::runtime::UiProjection;

namespace {

bool check(bool condition, const char* message)
{
    if (condition) return true;
    QTextStream(stderr) << "ui_projection_tests: FAIL: " << message << Qt::endl;
    return false;
}

int entryCount(const QString& path)
{
    return QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                    QDir::Name).size();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The fixture intentionally contains one review row and one invalid row.
    // The adapter receives bytes only; it must not open a path, spawn a process,
    // or create an image/temporary-file side effect.
    const QByteArray input = QByteArrayLiteral(
        "AUG Demo|any|unowned|orange|S|none|230|600|0.399|rare|default|7|9\n"
        "Broken|any|any|any|S|none|601|600|0.399|any|default||\n");
    QTemporaryDir sideEffectDir;
    if (!check(sideEffectDir.isValid(), "temporary side-effect directory is valid")) return 1;
    const int entriesBefore = entryCount(sideEffectDir.path());

    const QString originalCwd = QDir::currentPath();
    QDir::setCurrent(sideEffectDir.path());
    const QJsonObject preview = preview13Columns(input);
    QDir::setCurrent(originalCwd);
    const int entriesAfter = entryCount(sideEffectDir.path());
    if (!check(entriesBefore == entriesAfter, "preview creates no files or image artifacts")) return 1;

    const ImportPreviewUiProjection view = UiProjection::importPreview(preview);
    bool ok = true;
    ok &= check(view.format == QStringLiteral("bbzps_13"), "format is bbzps_13");
    ok &= check(view.encoding == QStringLiteral("UTF-8"), "encoding is UTF-8");
    ok &= check(view.sourceHash.size() == 64, "source hash is SHA-256 text");
    ok &= check(view.rowCount == 2, "projection keeps all rows");
    ok &= check(view.reviewRows == 1, "review row count is projected");
    ok &= check(view.invalidRows == 1, "invalid row count is projected");
    ok &= check(view.errorCount >= 1, "row error diagnostics are counted");
    ok &= check(view.diagnosticCount >= view.errorCount, "diagnostic count includes errors");
    ok &= check(!view.committable, "projection is never committable");
    ok &= check(view.readOnly, "projection is explicitly read-only");
    ok &= check(view.statusText.contains(QStringLiteral("仅预览")), "status advertises preview-only state");
    ok &= check(view.readOnlyNote.contains(QStringLiteral("committable=false")),
                "read-only note explains committable=false");
    if (!ok) return 1;

    const auto rows = preview.value(QStringLiteral("rows")).toArray();
    ok &= check(rows.size() == 2, "preview contains two rows");
    if (!rows.isEmpty()) {
        const auto first = rows.first().toObject();
        const auto candidate = first.value(QStringLiteral("candidate")).toObject();
        ok &= check(first.value(QStringLiteral("columns")).toArray().size() == 13,
                    "13-column row is preserved");
        ok &= check(candidate.value(QStringLiteral("enabled")).toBool() == false,
                    "candidate is disabled");
        ok &= check(candidate.value(QStringLiteral("review_required")).toBool(),
                    "candidate requires review");
    }
    if (rows.size() > 1) {
        const auto second = rows.at(1).toObject();
        ok &= check(second.value(QStringLiteral("status")).toString() == QStringLiteral("invalid"),
                    "invalid row remains invalid in preview");
        const auto invalidCandidate = second.value(QStringLiteral("candidate")).toObject();
        ok &= check(!invalidCandidate.value(QStringLiteral("enabled")).toBool(),
                    "invalid row candidate remains disabled");
        ok &= check(invalidCandidate.value(QStringLiteral("review_required")).toBool(),
                    "invalid row candidate still requires review");
    }
    if (!ok) return 1;

    // A malformed producer cannot turn the projection into an execution path:
    // readOnly remains true and committable remains false even when the marker
    // is missing or set to true.
    QJsonObject malformed = preview;
    malformed.insert(QStringLiteral("committable"), true);
    const auto malformedView = UiProjection::importPreview(malformed);
    ok &= check(!malformedView.committable, "malformed marker cannot make projection committable");
    ok &= check(malformedView.readOnly, "malformed marker still cannot enable UI actions");
    ok &= check(malformedView.readOnlyNote.contains(QStringLiteral("committable=false")),
                "read-only note remains explicit for malformed marker");
    if (!ok) return 1;

    QTextStream(stdout) << "ui_projection_tests: PASS" << Qt::endl;
    return 0;
}
