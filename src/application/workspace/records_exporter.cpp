#include "records_exporter.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace relink::workspace {

QString RecordsExporter::csvCell(QString value)
{
    int index=0; while(index<value.size() && value.at(index).isSpace()) ++index;
    const bool formula=index<value.size() && QStringLiteral("=+-@").contains(value.at(index));
    if (formula || (!value.isEmpty() && (value.front()==QLatin1Char('\t') || value.front()==QLatin1Char('\r') || value.front()==QLatin1Char('\n')))) value.prepend(QLatin1Char('\''));
    value.replace(QStringLiteral("\""),QStringLiteral("\"\"")); return QLatin1Char('\"')+value+QLatin1Char('\"');
}

ExportResult RecordsExporter::writeCsv(const QString& path, const QString& rootPath,
                                       const QString& databasePath, const WorkspaceProjection& projection) const
{
    const auto resolved = [](const QString& filePath) {
        const QFileInfo info(filePath);
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) return QDir::cleanPath(canonical);
        const QString parent = info.dir().canonicalPath();
        return QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName()));
    };
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    const QString target = resolved(path), database = resolved(databasePath);
    const QString profileDirectory = resolved(QDir(rootPath).filePath(QStringLiteral("profiles")));
    bool protectedPath = target.compare(profileDirectory,pathCase)==0
        || target.startsWith(profileDirectory+QLatin1Char('/'),pathCase);
    for (const auto& suffix : {QString(),QStringLiteral("-wal"),QStringLiteral("-shm"),QStringLiteral("-journal"),QStringLiteral(".writer.lock")})
        protectedPath |= target.compare(database+suffix,pathCase)==0;
    if (protectedPath) return {false, QStringLiteral("CSV_TARGET_PROTECTED: choose an export file outside workspace storage")};
    QByteArray bytes("\xEF\xBB\xBF");
    bytes += "run_id,event_id,type,mode,run_profile_id,run_profile_revision,source,clock_domain,mono_ms,listing_id,observation_id,observed_price,confirmed_simulated_price,reason\r\n";
    QString runMode;
    for (const auto& run:projection.runs) if(run.id==projection.selectedRunId) { runMode=run.mode; break; }
    for (const auto& row:projection.records) {
        QStringList fields{row.runId,row.eventId,row.type,runMode,projection.runProfileId,QString::number(projection.runProfileRevision),row.source,row.clockDomainId,QString::number(row.atMonoMs),row.listingId,row.observationId,row.observedPrice,row.confirmedPrice,row.reason};
        for (auto& field:fields) field=csvCell(field);
        bytes+=fields.join(QLatin1Char(',')).toUtf8()+"\r\n";
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
        return {false, QStringLiteral("CSV_WRITE_FAILED: ") + file.errorString()};
    return {true, {}};
}

} // namespace relink::workspace
