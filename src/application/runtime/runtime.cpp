#include "runtime.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <limits>

namespace relink::runtime {
namespace {

QString jsonString(const QJsonObject& o, const char* key, QString* error, bool required = true) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString() || (required && v.toString().isEmpty())) {
        if (error) *error = QStringLiteral("invalid_or_missing_%1").arg(QLatin1String(key));
        return {};
    }
    return v.toString();
}

qint64 jsonInt(const QJsonObject& o, const char* key, QString* error, bool required = true) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) {
        if (error) *error = QStringLiteral("invalid_or_missing_%1").arg(QLatin1String(key));
        return 0;
    }
    const double d = v.toDouble();
    if (d < 0 || d > static_cast<double>(std::numeric_limits<qint64>::max()) || d != static_cast<qint64>(d)) {
        if (error) *error = QStringLiteral("invalid_%1").arg(QLatin1String(key));
        return 0;
    }
    Q_UNUSED(required);
    return static_cast<qint64>(d);
}

RuntimeContext contextFromJson(const QJsonObject& o, QString* error) {
    RuntimeContext c;
    c.runId = jsonString(o, "run_id", error);
    if (c.runId.isEmpty()) return c;
    c.sessionId = jsonString(o, "session_id", error);
    if (c.sessionId.isEmpty()) return c;
    c.clockDomainId = jsonString(o, "clock_domain_id", error);
    if (c.clockDomainId.isEmpty()) return c;
    c.stepId = jsonString(o, "step_id", error);
    if (c.stepId.isEmpty()) return c;
    c.cancelEpoch = jsonInt(o, "cancel_epoch", error);
    if (error && !error->isEmpty()) return c;
    c.viewportGeneration = jsonInt(o, "viewport_generation", error);
    return c;
}

bool sameContext(const RuntimeContext& a, const RuntimeContext& b) {
    return a.runId == b.runId && a.sessionId == b.sessionId
        && a.clockDomainId == b.clockDomainId && a.stepId == b.stepId
        && a.cancelEpoch == b.cancelEpoch && a.viewportGeneration == b.viewportGeneration;
}

QString nextStep(const QString& current) {
    int i = current.size() - 1;
    while (i >= 0 && current.at(i).isDigit()) --i;
    if (i == current.size() - 1) return current + QStringLiteral("-next");
    bool ok = false;
    const qint64 n = current.mid(i + 1).toLongLong(&ok);
    return ok ? current.left(i + 1) + QString::number(n + 1) : current + QStringLiteral("-next");
}

AttemptSnapshot* findAttempt(RunSnapshot& s, const QString& id) {
    for (AttemptSnapshot& a : s.attempts) if (a.attemptId == id) return &a;
    return nullptr;
}

void effect(ReduceResult& r, const QString& type, const QVariantMap& payload = {}) {
    r.effects.push_back({type, payload});
    r.snapshot.emittedEvents.push_back(type);
}

void internalEffect(ReduceResult& r, const QString& type, const QVariantMap& payload = {}) {
    r.effects.push_back({type, payload});
}

void requestObservation(ReduceResult& r, const QString& purpose) {
    ++r.snapshot.demandCount;
    ++r.snapshot.captureRequestCount;
    const QVariantMap p{{QStringLiteral("purpose"), purpose},
                        {QStringLiteral("mode"), QStringLiteral("one_shot")},
                        {QStringLiteral("frame_budget"), 1},
                        {QStringLiteral("persistence"), QStringLiteral("none")}};
    effect(r, QStringLiteral("ObservationRequested"), p);
    internalEffect(r, QStringLiteral("AcquireFrame"), p);
}

void markAttemptUnknown(ReduceResult& r, AttemptSnapshot& a) {
    if (a.state == QStringLiteral("Success") || a.state == QStringLiteral("Failed")
        || a.state == QStringLiteral("Cancelled")) return;
    a.state = QStringLiteral("Unknown");
    a.dispatchProof = QStringLiteral("possibly_dispatched");
    a.reservationHeld = true;
    effect(r, QStringLiteral("AttemptUnknown"), {{QStringLiteral("attempt_id"), a.attemptId}});
}

void releaseReservation(ReduceResult& r, AttemptSnapshot& a) {
    if (!a.reservationHeld) return;
    a.reservationHeld = false;
    r.snapshot.unresolvedReservations = std::max(0, r.snapshot.unresolvedReservations - 1);
    effect(r, QStringLiteral("ReservationReleased"), {{QStringLiteral("attempt_id"), a.attemptId}});
}

void cancelPending(ReduceResult& r) {
    for (AttemptSnapshot& a : r.snapshot.attempts) {
        if (a.state == QStringLiteral("Prepared") && a.dispatchProof == QStringLiteral("not_dispatched")) {
            a.state = QStringLiteral("Cancelled");
            effect(r, QStringLiteral("AttemptCancelled"), {{QStringLiteral("attempt_id"), a.attemptId}});
            releaseReservation(r, a);
        } else if (a.state == QStringLiteral("Dispatching") || a.state == QStringLiteral("Sent")
                   || a.state == QStringLiteral("AwaitingReceipt")) {
            markAttemptUnknown(r, a);
        }
    }
}

bool isLedgerEvent(const QString& type) {
    return type == QStringLiteral("LedgerCommitted") || type == QStringLiteral("LedgerFailed");
}

} // namespace

ReplayEvent ReplayEvent::fromJson(const QJsonObject& object, QString* error) {
    ReplayEvent e;
    if (error) error->clear();
    e.eventId = jsonString(object, "event_id", error);
    if (e.eventId.isEmpty()) return e;
    e.atMonoMs = jsonInt(object, "at_mono_ms", error);
    if (error && !error->isEmpty()) return e;
    const QJsonObject ctx = object.value(QStringLiteral("context")).toObject();
    e.context = contextFromJson(ctx, error);
    if (error && !error->isEmpty()) return e;
    e.type = jsonString(object, "type", error);
    if (e.type.isEmpty()) return e;
    e.payload = object.value(QStringLiteral("payload")).toObject().toVariantMap();
    return e;
}

RuntimeContext ReplayReducer::contextOf(const RunSnapshot& s) {
    return {s.runId, s.sessionId, s.clockDomainId, s.stepId, s.cancelEpoch, s.viewportGeneration};
}

ReduceResult ReplayReducer::reduce(const RunSnapshot& input, const ReplayEvent& event) {
    ReduceResult r{input, {}, true, false, {}};
    r.snapshot.nowMonoMs = std::max(r.snapshot.nowMonoMs, event.atMonoMs);

    if (event.eventId.isEmpty()) {
        r.accepted = false;
        r.error = QStringLiteral("missing_event_id");
        return r;
    }
    if (r.snapshot.seenEventIds.contains(event.eventId)) {
        ++r.snapshot.ignoredEventCount;
        r.ignored = true;
        effect(r, QStringLiteral("DuplicateEventIgnored"), {{QStringLiteral("event_id"), event.eventId}});
        return r;
    }
    r.snapshot.seenEventIds.insert(event.eventId);

    const bool ledger = isLedgerEvent(event.type);
    const RuntimeContext current = contextOf(r.snapshot);
    // Commit/failure callbacks are allowed to drain after Pause/Stop. All visual and
    // command events must match the current session, epoch, viewport and step.
    if (!ledger && !sameContext(current, event.context)) {
        ++r.snapshot.ignoredEventCount;
        r.ignored = true;
        effect(r, QStringLiteral("StaleEventIgnored"), {{QStringLiteral("event_id"), event.eventId}});
        return r;
    }

    const QString t = event.type;
    const QVariantMap& p = event.payload;
    if (t == QStringLiteral("Start")) {
        if (r.snapshot.runState != QStringLiteral("Ready")) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
            return r;
        }
        const bool reviewed = p.value(QStringLiteral("configuration_reviewed")).toBool();
        const bool due = p.value(QStringLiteral("schedule_due")).toBool();
        if (!reviewed) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
            return r;
        }
        if (!due) {
            r.snapshot.runState = QStringLiteral("AwaitSchedule");
            effect(r, QStringLiteral("ScheduleWaiting"));
        } else {
            r.snapshot.runState = QStringLiteral("Observe");
            r.snapshot.stepId = nextStep(r.snapshot.stepId);
            requestObservation(r, QStringLiteral("page_check"));
        }
    } else if (t == QStringLiteral("Stop") || t == QStringLiteral("Pause")) {
        ++r.snapshot.cancelEpoch;
        cancelPending(r);
        r.snapshot.runState = (t == QStringLiteral("Stop")) ? QStringLiteral("Stopped") : QStringLiteral("Paused");
        effect(r, t == QStringLiteral("Stop") ? QStringLiteral("RunStopped") : QStringLiteral("RunPaused"));
    } else if (t == QStringLiteral("Resume")) {
        if (r.snapshot.runState != QStringLiteral("Paused")) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
            return r;
        }
        ++r.snapshot.cancelEpoch;
        r.snapshot.stepId = nextStep(r.snapshot.stepId);
        r.snapshot.runState = QStringLiteral("Observe");
        effect(r, QStringLiteral("RunResumed"));
        requestObservation(r, QStringLiteral("page_check"));
    } else if (t == QStringLiteral("PageRecognized")) {
        const QString page = p.value(QStringLiteral("page")).toString();
        if (page == QStringLiteral("watchlist") && r.snapshot.runState == QStringLiteral("Observe")) {
            r.snapshot.runState = QStringLiteral("Watchlist");
            r.snapshot.stepId = nextStep(r.snapshot.stepId);
            effect(r, QStringLiteral("WatchlistEntered"));
            requestObservation(r, QStringLiteral("listing_fields"));
        } else {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        }
    } else if (t == QStringLiteral("QueueEmpty")) {
        if (r.snapshot.runState == QStringLiteral("Watchlist")) {
            r.snapshot.runState = QStringLiteral("Completed");
            effect(r, QStringLiteral("RunCompleted"));
        } else {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        }
    } else if (t == QStringLiteral("WatchTick")) {
        ++r.snapshot.ignoredEventCount; r.ignored = true;
        effect(r, QStringLiteral("UnexpectedEventIgnored"));
    } else if (t == QStringLiteral("DeadlineWake")) {
        if (r.snapshot.runState != QStringLiteral("AwaitDeadline")) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        } else {
            r.snapshot.runState = QStringLiteral("Revalidate");
            r.snapshot.stepId = nextStep(r.snapshot.stepId);
            requestObservation(r, QStringLiteral("revalidate"));
        }
    } else if (t == QStringLiteral("Receipt")) {
        const QString id = p.value(QStringLiteral("attempt_id")).toString();
        AttemptSnapshot* a = findAttempt(r.snapshot, id);
        if (!a) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        } else {
            const QString outcome = p.value(QStringLiteral("outcome")).toString();
            const QString association = p.value(QStringLiteral("association")).toString();
            a->receiptIdentity = p.value(QStringLiteral("receipt_identity")).toString();
            if (outcome == QStringLiteral("unknown") || association != QStringLiteral("confirmed")) {
                markAttemptUnknown(r, *a);
                r.snapshot.runState = QStringLiteral("Reconcile");
                effect(r, QStringLiteral("ReconcileRequired"), {{QStringLiteral("attempt_id"), id}});
            } else {
                a->state = QStringLiteral("AwaitingReceipt");
                r.snapshot.runState = QStringLiteral("PersistResult");
                r.snapshot.stepId = nextStep(r.snapshot.stepId);
                const QString tx = QStringLiteral("tx:") + a->attemptId + QStringLiteral(":") + a->receiptIdentity;
                r.snapshot.pendingTransactions.insert(a->attemptId, tx);
                effect(r, QStringLiteral("ReceiptCommitRequested"), {{QStringLiteral("attempt_id"), id}, {QStringLiteral("transaction_id"), tx}});
            }
        }
    } else if (t == QStringLiteral("LedgerCommitted")) {
        const QString id = p.value(QStringLiteral("attempt_id")).toString();
        const QString tx = p.value(QStringLiteral("transaction_id")).toString();
        if (r.snapshot.committedTransactions.contains(tx)) {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("DuplicateLedgerIgnored"), {{QStringLiteral("transaction_id"), tx}});
        } else if (AttemptSnapshot* a = findAttempt(r.snapshot, id)) {
            r.snapshot.committedTransactions.insert(tx);
            r.snapshot.pendingTransactions.remove(id);
            const QString terminal = p.value(QStringLiteral("terminal")).toString();
            if (terminal == QStringLiteral("Success")) {
                a->state = QStringLiteral("Success");
                ++r.snapshot.confirmedSuccess;
                releaseReservation(r, *a);
                effect(r, QStringLiteral("AttemptSucceeded"), {{QStringLiteral("attempt_id"), id}});
                const bool queueEmpty = p.value(QStringLiteral("queue_empty")).toBool();
                if (r.snapshot.runState != QStringLiteral("Paused") && queueEmpty
                    && r.snapshot.confirmedSuccess >= r.snapshot.targetQuantity) {
                    r.snapshot.runState = QStringLiteral("Completed");
                    effect(r, QStringLiteral("RunCompleted"));
                }
            } else {
                a->state = QStringLiteral("Failed");
                releaseReservation(r, *a);
                effect(r, QStringLiteral("AttemptFailed"), {{QStringLiteral("attempt_id"), id}});
            }
        }
    } else if (t == QStringLiteral("LedgerFailed")) {
        r.snapshot.runState = QStringLiteral("Paused");
        effect(r, QStringLiteral("StorageFailed"), {{QStringLiteral("code"), p.value(QStringLiteral("code")).toString()}});
        effect(r, QStringLiteral("RunPaused"));
    } else if (t == QStringLiteral("RestartLoaded")) {
        r.snapshot.sessionId = p.value(QStringLiteral("new_session_id")).toString();
        r.snapshot.clockDomainId = p.value(QStringLiteral("new_clock_domain_id")).toString();
        for (AttemptSnapshot& a : r.snapshot.attempts) markAttemptUnknown(r, a);
        r.snapshot.runState = QStringLiteral("Reconcile");
        effect(r, QStringLiteral("RunRecovered"));
    } else if (t == QStringLiteral("ViewportChanged")) {
        r.snapshot.viewportGeneration = p.value(QStringLiteral("new_viewport_generation")).toLongLong();
        ++r.snapshot.cancelEpoch;
        r.snapshot.runState = QStringLiteral("Recover");
        effect(r, QStringLiteral("ViewportInvalidated"));
    } else if (t == QStringLiteral("WorkerCrashed")) {
        r.snapshot.runState = QStringLiteral("Recover");
        effect(r, QStringLiteral("ObservationCancelled"));
        effect(r, QStringLiteral("WorkerLeaseQuarantined"));
        effect(r, QStringLiteral("RecoveryRequired"));
    } else if (t == QStringLiteral("Revalidated")) {
        const QString decision = p.value(QStringLiteral("decision")).toString();
        if (decision == QStringLiteral("Match") && p.value(QStringLiteral("association")).toString() == QStringLiteral("confirmed")) {
            effect(r, QStringLiteral("ReservationRequested"));
        } else {
            r.snapshot.runState = QStringLiteral("Watchlist");
        }
    } else if (t == QStringLiteral("ReservationDenied")) {
        effect(r, QStringLiteral("ReservationRejected"), {{QStringLiteral("reason"), p.value(QStringLiteral("reason")).toString()}});
        r.snapshot.runState = QStringLiteral("Reconcile");
        effect(r, QStringLiteral("ReconcileRequired"));
    } else if (t == QStringLiteral("RequestReconcile")) {
        if (r.snapshot.runState == QStringLiteral("Reconcile")) {
            r.snapshot.stepId = nextStep(r.snapshot.stepId);
            requestObservation(r, QStringLiteral("receipt"));
        } else {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        }
    } else if (t == QStringLiteral("ObservationDeadline")) {
        if (r.snapshot.runState == QStringLiteral("Reconcile")) {
            effect(r, QStringLiteral("ObservationExpired"));
            effect(r, QStringLiteral("ReconcileRequired"));
        } else {
            ++r.snapshot.ignoredEventCount; r.ignored = true;
            effect(r, QStringLiteral("UnexpectedEventIgnored"));
        }
    } else if (t == QStringLiteral("ReservationGranted")) {
        effect(r, QStringLiteral("ReservationGranted"), p);
    } else if (t == QStringLiteral("ActionPossiblySent")) {
        if (AttemptSnapshot* a = findAttempt(r.snapshot, p.value(QStringLiteral("attempt_id")).toString())) {
            a->state = QStringLiteral("Dispatching"); a->dispatchProof = QStringLiteral("possibly_dispatched");
        }
    } else if (t == QStringLiteral("DefinitelyNotSent")) {
        if (AttemptSnapshot* a = findAttempt(r.snapshot, p.value(QStringLiteral("attempt_id")).toString())) {
            a->state = QStringLiteral("Cancelled"); releaseReservation(r, *a);
        }
    } else {
        ++r.snapshot.ignoredEventCount; r.ignored = true;
        effect(r, QStringLiteral("UnexpectedEventIgnored"));
    }
    return r;
}

RunCoordinator::RunCoordinator(RunSnapshot initial)
    : m_snapshot(std::move(initial)), m_clock(m_snapshot.nowMonoMs) {}

ReduceResult RunCoordinator::submit(ReplayEvent event) {
    if (event.seq <= 0) event.seq = m_snapshot.nextSeq++;
    else m_snapshot.nextSeq = std::max(m_snapshot.nextSeq, event.seq + 1);
    m_clock.advanceTo(event.atMonoMs);
    ReduceResult result = ReplayReducer::reduce(m_snapshot, event);
    m_snapshot = result.snapshot;
    m_snapshot.nowMonoMs = m_clock.nowMs();
    return result;
}

ReduceResult RunCoordinator::submitJson(const QJsonObject& object, QString* error) {
    ReplayEvent event = ReplayEvent::fromJson(object, error);
    if (error && !error->isEmpty()) return {m_snapshot, {}, false, false, *error};
    return submit(event);
}

QVector<ReduceResult> RunCoordinator::replay(const QVector<ReplayEvent>& events) {
    QVector<ReduceResult> results;
    results.reserve(events.size());
    for (ReplayEvent event : events) results.push_back(submit(event));
    return results;
}

QString runStateName(const RunSnapshot& snapshot) { return snapshot.runState; }

} // namespace relink::runtime
