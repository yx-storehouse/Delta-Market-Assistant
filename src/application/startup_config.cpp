#include "application/startup_config.h"
#include "domain.h"
#include <QDir>
#include <QFileInfo>

namespace relink::application {

QString prepareStartupConfig(AppState& state, const QString& requested, bool loadExisting) {
    QString destination = QFileInfo(requested).absoluteFilePath();
    QDir().mkpath(QFileInfo(destination).absolutePath());
    if (loadExisting && QFileInfo::exists(destination)) {
        QString error;
        if (!state.loadFrom(destination, &error)) {
            const QFileInfo original(destination);
            const QString stem = original.absolutePath() + "/" + original.completeBaseName() + ".recovered";
            destination = stem + ".json";
            for (int suffix = 1; QFileInfo::exists(destination); ++suffix)
                destination = stem + "-" + QString::number(suffix) + ".json";
            state.addLog("WARN", QStringLiteral("原配置未载入且保持原样：%1；恢复配置另存到 %2").arg(error, destination));
        }
    }
    state.configPath = destination;
    return destination;
}

} // namespace relink::application
