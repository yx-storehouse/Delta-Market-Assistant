#include "ui_projection.h"

namespace relink::runtime {
namespace {
QString stateText(const QString& state)
{
    if (state == QStringLiteral("Ready")) return QStringLiteral("就绪");
    if (state == QStringLiteral("Observe")) return QStringLiteral("观察中");
    if (state == QStringLiteral("Paused")) return QStringLiteral("已暂停");
    if (state == QStringLiteral("Stopped")) return QStringLiteral("已停止");
    if (state == QStringLiteral("Completed")) return QStringLiteral("已完成");
    if (state == QStringLiteral("Recover")) return QStringLiteral("等待恢复");
    if (state == QStringLiteral("Reconcile")) return QStringLiteral("待核验");
    if (state == QStringLiteral("PersistResult")) return QStringLiteral("保存回执");
    if (state == QStringLiteral("Watchlist")) return QStringLiteral("关注列表");
    if (state == QStringLiteral("AwaitDeadline")) return QStringLiteral("等待计划");
    if (state == QStringLiteral("Revalidate")) return QStringLiteral("重新核验");
    return state;
}
}

ReplayUiProjection UiProjection::replay(const RunSnapshot& snapshot)
{
    ReplayUiProjection view;
    view.source = snapshot.mode == QStringLiteral("Replay")
        ? QStringLiteral("Replay · 离线合成回放") : QStringLiteral("Observe · 仅观察");
    view.runState = snapshot.runState;
    view.runStateText = stateText(snapshot.runState);
    view.progress = QStringLiteral("观察请求 %1 · 内存帧 %2 · 成功 %3 · 未决预留 %4")
        .arg(snapshot.demandCount).arg(snapshot.captureRequestCount)
        .arg(snapshot.confirmedSuccess).arg(snapshot.unresolvedReservations);
    if (snapshot.runState == QStringLiteral("Ready")) {
        view.matchReason = QStringLiteral("尚未开始回放；不会连接游戏或发送真实动作");
    } else if (snapshot.runState == QStringLiteral("Paused")) {
        view.matchReason = QStringLiteral("回放已暂停；未决状态保留，未发送真实动作");
    } else if (snapshot.runState == QStringLiteral("Completed")) {
        view.matchReason = QStringLiteral("回放队列已完成；本轮没有真实匹配或购买");
    } else if (snapshot.runState == QStringLiteral("Stopped")) {
        view.matchReason = QStringLiteral("回放已停止；未发送真实动作");
    } else {
        view.matchReason = QStringLiteral("合成回放：匹配原因仅用于界面验证，真实 OCR/采集尚未接入");
    }
    view.canStart = snapshot.runState == QStringLiteral("Ready");
    view.canPause = snapshot.runState != QStringLiteral("Ready")
        && snapshot.runState != QStringLiteral("Paused")
        && snapshot.runState != QStringLiteral("Stopped")
        && snapshot.runState != QStringLiteral("Completed");
    view.canResume = snapshot.runState == QStringLiteral("Paused");
    view.canStop = snapshot.runState != QStringLiteral("Ready")
        && snapshot.runState != QStringLiteral("Stopped")
        && snapshot.runState != QStringLiteral("Completed");
    return view;
}

ImportPreviewUiProjection UiProjection::importPreview(const QJsonObject& preview)
{
    ImportPreviewUiProjection view;
    view.format = preview.value(QStringLiteral("source_format")).toString();
    view.encoding = preview.value(QStringLiteral("encoding")).toString();
    view.sourceHash = preview.value(QStringLiteral("source_sha256")).toString();
    // The projection is a hard read-only boundary.  Even if a malformed
    // producer marks the payload committable, the UI projection never exposes
    // that marker as an executable capability.
    view.committable = false;
    view.readOnly = true;
    const auto countDiagnostics = [&](const QJsonArray& diagnostics) {
        for (const auto& item : diagnostics) {
            ++view.diagnosticCount;
            if (item.toObject().value(QStringLiteral("severity")).toString() == QStringLiteral("error"))
                ++view.errorCount;
        }
    };
    countDiagnostics(preview.value(QStringLiteral("diagnostics")).toArray());
    const auto rows = preview.value(QStringLiteral("rows")).toArray();
    view.rowCount = rows.size();
    for (const auto& rowValue : rows) {
        const auto row = rowValue.toObject();
        const QString status = row.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("invalid")) ++view.invalidRows;
        else if (status == QStringLiteral("review")) ++view.reviewRows;
        countDiagnostics(row.value(QStringLiteral("diagnostics")).toArray());
    }
    if (view.errorCount > 0 || view.invalidRows > 0)
        view.statusText = QStringLiteral("需要修正 %1 个错误 · 仅预览").arg(view.errorCount);
    else if (view.reviewRows > 0)
        view.statusText = QStringLiteral("%1 行待人工确认 · 仅预览").arg(view.reviewRows);
    else
        view.statusText = QStringLiteral("可查看 · 不会写入配置");
    view.readOnlyNote = QStringLiteral("只读导入预览：committable=false；不会修改配置、不会连接游戏、不会发送真实动作");
    return view;
}

} // namespace relink::runtime
