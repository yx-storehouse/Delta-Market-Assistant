#include "application/runtime/runtime.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using namespace relink::runtime;

namespace {
int failures = 0;
void check(bool ok, const QString& label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label.toStdString() << '\n';
    if (!ok) ++failures;
}
QString s(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
RunSnapshot snapshotFromJson(const QJsonObject& o) {
    RunSnapshot s0;
    s0.runId = s(o, "run_id"); s0.sessionId = s(o, "session_id"); s0.clockDomainId = s(o, "clock_domain_id");
    s0.stepId = s(o, "step_id"); s0.cancelEpoch = o.value("cancel_epoch").toInteger();
    s0.viewportGeneration = o.value("viewport_generation").toInteger(); s0.runState = s(o, "run_state");
    s0.mode = s(o, "mode"); s0.nowMonoMs = o.value("now_mono_ms").toInteger();
    s0.autoCollect = o.value("auto_collect").toBool(); s0.targetQuantity = o.value("target_quantity").toInt(1);
    s0.confirmedSuccess = o.value("confirmed_success").toInt(); s0.unresolvedReservations = o.value("unresolved_reservations").toInt();
    for (const QJsonValue& value : o.value("attempts").toArray()) {
        const QJsonObject a = value.toObject(); AttemptSnapshot x;
        x.attemptId = s(a, "attempt_id"); x.intentId = s(a, "intent_id"); x.runId = s(a, "run_id");
        x.kind = s(a, "kind"); x.state = s(a, "state"); x.quantity = a.value("quantity").toInt(1);
        x.reservationHeld = a.value("reservation_held").toBool(); x.dispatchProof = s(a, "dispatch_proof");
        x.receiptIdentity = a.value("receipt_identity").toString();
        const QJsonObject rr = a.value("rule_ref").toObject(); x.ruleTaskId = s(rr, "task_id"); x.ruleRevision = rr.value("revision").toInt();
        s0.attempts.push_back(x);
    }
    return s0;
}
ReplayEvent eventFromJson(const QJsonObject& o) {
    QString error; ReplayEvent e = ReplayEvent::fromJson(o, &error);
    if (!error.isEmpty()) std::cerr << "fixture parse: " << error.toStdString() << '\n';
    return e;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("docs/business_rebuild/implementation/runtime/fixtures/replay_cases.json");
    QFile file(path); check(file.open(QIODevice::ReadOnly), QStringLiteral("fixture_open"));
    if (!file.isOpen()) return 1;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray cases = root.value("cases").toArray();
    check(cases.size() == 15, QStringLiteral("fixture_case_count"));
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject(); const QString id = c.value("id").toString();
        RunCoordinator coordinator(snapshotFromJson(c.value("initial").toObject()));
        for (const QJsonValue& ev : c.value("inputs").toArray()) coordinator.submit(eventFromJson(ev.toObject()));
        const RunSnapshot& got = coordinator.snapshot(); const QJsonObject ex = c.value("expected").toObject();
        check(got.runState == s(ex, "run_state"), id + QStringLiteral("_state"));
        check(got.confirmedSuccess == ex.value("confirmed_success").toInt(), id + QStringLiteral("_success"));
        check(got.unresolvedReservations == ex.value("unresolved_reservations").toInt(), id + QStringLiteral("_reservations"));
        check(got.demandCount == ex.value("demand_count").toInt(), id + QStringLiteral("_demand"));
        check(got.captureRequestCount == ex.value("capture_request_count").toInt(), id + QStringLiteral("_capture"));
        check(got.imageFileWriteCount == ex.value("image_file_write_count").toInt(), id + QStringLiteral("_no_file_writes"));
        check(got.ignoredEventCount == ex.value("ignored_event_count").toInt(), id + QStringLiteral("_ignored"));
        const QJsonObject states = ex.value("attempt_states").toObject();
        bool attemptsOk = states.size() == got.attempts.size();
        for (auto it = states.constBegin(); it != states.constEnd(); ++it) {
            const AttemptSnapshot* a = nullptr; for (const AttemptSnapshot& candidate : got.attempts) if (candidate.attemptId == it.key()) a = &candidate;
            attemptsOk = attemptsOk && a && a->state == it.value().toString();
        }
        check(attemptsOk, id + QStringLiteral("_attempt_states"));
        const QJsonArray required = ex.value("required_events").toArray();
        bool subsequence = true; int cursor = 0;
        for (const QJsonValue& v : required) {
            const QString want = v.toString(); int found = got.emittedEvents.indexOf(want, cursor);
            if (found < 0) { subsequence = false; break; }
            cursor = found + 1;
        }
        check(subsequence, id + QStringLiteral("_required_events"));
        const QJsonArray forbidden = ex.value("forbidden_events").toArray();
        bool noForbidden = true; for (const QJsonValue& v : forbidden) noForbidden = noForbidden && !got.emittedEvents.contains(v.toString());
        check(noForbidden, id + QStringLiteral("_forbidden_events"));
        const QJsonValue exact = ex.value("exact_events");
        if (exact.isArray()) {
            const QJsonArray expected = exact.toArray(); bool exactOk = expected.size() == got.emittedEvents.size();
            for (int i = 0; exactOk && i < expected.size(); ++i) exactOk = expected.at(i).toString() == got.emittedEvents.at(i);
            check(exactOk, id + QStringLiteral("_exact_events"));
        }
    }
    return failures == 0 ? 0 : 1;
}
