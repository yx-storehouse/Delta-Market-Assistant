#include "application/collection_task_commit.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
namespace relink::application {
bool commitCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
    const catalog::Catalog& catalog, AppState& state, CollectionTaskImportPreview* report, QString* error) {
    CollectionTaskImportPreview candidate;
    if (!previewCollectionTaskImport(bytes, dictionary, catalog, state, &candidate, error)) return false;
    if (!candidate.addedCount) { if (report) *report = candidate; return true; }
    if (state.configPath.isEmpty()) { if (error) *error = QStringLiteral("配置保存路径为空。"); return false; }
    const QString path=QFileInfo(state.configPath).absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (error) *error = QStringLiteral("配置目录创建失败。"); return false; }
    QLockFile lock(path + QStringLiteral(".import.lock"));lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { if (error) *error = QStringLiteral("另一个任务导入正在保存，请稍后重试。"); return false; }
    AppState draft;draft.skins=state.skins;draft.tasks=candidate.mergedTasks;draft.run=state.run;
    // A copy preserves the last on-disk configuration while QSaveFile commits
    // the entire candidate. Failed persistence never publishes tasks to UI.
    if (QFileInfo(path).isFile()) {
        QString backup=path+QStringLiteral(".before-task-import.bak");
        for(int n=1;QFileInfo::exists(backup);++n) backup=path+QStringLiteral(".before-task-import-%1.bak").arg(n);
        if (!QFile::copy(path,backup)) { if(error) *error=QStringLiteral("原配置备份失败，未导入任务。");return false; }
    }
    if (!draft.saveTo(path,error)) return false;
    state.tasks=candidate.mergedTasks;
    state.addLog(QStringLiteral("INFO"),QStringLiteral("已追加 %1 条收藏任务；跳过 %2 条重复来源。").arg(candidate.addedCount).arg(candidate.duplicateCount));
    if (report) *report=candidate;
    state.notifyChanged();
    return true;
}
}
