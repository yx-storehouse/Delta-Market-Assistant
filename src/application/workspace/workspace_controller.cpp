#include "workspace_controller.h"

#include "application/runtime/runtime.h"
#include "business/matcher.h"
#include "business/validator.h"
#include "config/profile_store.h"
#include "ledger/sqlite_event_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <optional>

namespace relink::workspace {
namespace {
QString uid(const QString& prefix) { return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString s(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
business::DecimalValue decimal(const QJsonObject& o) { return {s(o, "unscaled"), quint8(o.value(QStringLiteral("scale")).toInt())}; }
business::Money money(const QJsonObject& o) { return {decimal(o.value(QStringLiteral("value")).toObject()), s(o, "unit")}; }
QString priceText(const business::DecimalValue& value)
{
    QString result = value.unscaled;
    while (result.size() <= value.scale) result.prepend(QLatin1Char('0'));
    if (value.scale) result.insert(result.size() - value.scale, QLatin1Char('.'));
    return result;
}
QJsonObject builtinDocument()
{
    return {{QStringLiteral("schema_version"), 2}, {QStringLiteral("kind"), QStringLiteral("ConfigV2")},
        {QStringLiteral("profile"), QJsonObject{{QStringLiteral("id"), WorkspaceController::builtinProfileId()},
            {QStringLiteral("revision"), 1}, {QStringLiteral("name"), QStringLiteral("内置合成回放 · 八步验收")}}},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("format"), QStringLiteral("synthetic_fixture")}}},
        {QStringLiteral("extensions"), QJsonObject{{QStringLiteral("x-live_market_data"), false},
            {QStringLiteral("x-built-in-fixture"), true}}}};
}
business::TaskRule fixtureRule()
{
    business::TaskRule rule;
    rule.task_id = QStringLiteral("fixture-rule"); rule.revision = 1;
    rule.name = QStringLiteral("Synthetic price range"); rule.product_ref = QStringLiteral("synthetic-product-1");
    rule.enabled = true;
    rule.price_range = business::PriceRange{{{QStringLiteral("100"), 0}, QStringLiteral("synthetic-unit")},
                                          {{QStringLiteral("200"), 0}, QStringLiteral("synthetic-unit")}};
    return rule;
}
business::Selector selector(const QJsonObject& o)
{
    return {o.value(QStringLiteral("op")).toString(QStringLiteral("any")), s(o, "value")};
}
business::TaskRule savedRule(const QJsonObject& document)
{
    const auto rules = document.value(QStringLiteral("rules")).toArray();
    const auto o = rules.isEmpty() ? QJsonObject{} : rules.first().toObject();
    business::TaskRule r;
    r.task_id = s(o, "task_id"); r.revision = o.value(QStringLiteral("revision")).toInt(1);
    r.name = s(o, "name"); r.enabled = false; // Saved profiles are never activated by selecting them.
    r.review_required = o.value(QStringLiteral("review_required")).toBool();
    if (!s(o, "product_ref").isEmpty()) r.product_ref = s(o, "product_ref");
    if (o.value(QStringLiteral("price_range")).isObject()) {
        const auto p = o.value(QStringLiteral("price_range")).toObject();
        r.price_range = business::PriceRange{money(p.value(QStringLiteral("min")).toObject()), money(p.value(QStringLiteral("max")).toObject())};
    }
    if (o.value(QStringLiteral("max_wear")).isObject()) r.max_wear = decimal(o.value(QStringLiteral("max_wear")).toObject());
    const auto f = o.value(QStringLiteral("filters")).toObject();
    r.filters = {selector(f.value(QStringLiteral("season")).toObject()), selector(f.value(QStringLiteral("ownership")).toObject()),
        selector(f.value(QStringLiteral("grade")).toObject()), selector(f.value(QStringLiteral("condition")).toObject()),
        selector(f.value(QStringLiteral("publicity")).toObject()), selector(f.value(QStringLiteral("rarity")).toObject())};
    return r;
}
bool profileValid(const QJsonObject& doc)
{
    const auto p = doc.value(QStringLiteral("profile")).toObject();
    static const QRegularExpression idPattern(QStringLiteral("^profile-[0-9a-f]{24}$"));
    if (!idPattern.match(s(p, "id")).hasMatch() || p.value(QStringLiteral("revision")).toInt() < 1 || s(p, "name").trimmed().isEmpty()) return false;
    const auto source = doc.value(QStringLiteral("source")).toObject();
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hashPattern.match(s(source,"sha256")).hasMatch()
        || !QStringList{QStringLiteral("schema_v1"),QStringLiteral("bbzps_13")}.contains(s(source,"format"))
        || !QStringList{QStringLiteral("UTF-8"),QStringLiteral("UTF-8-BOM"),QStringLiteral("UTF-16LE-BOM"),QStringLiteral("GB18030-explicit")}.contains(s(source,"encoding"))) return false;
    const QByteArray identity = (s(p,"name").trimmed() + QChar(0x1f) + s(source,"sha256")).toUtf8();
    if (s(p,"id") != QStringLiteral("profile-") + QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex().left(24))) return false;
    const auto extensions = doc.value(QStringLiteral("extensions")).toObject();
    if (!extensions.value(QStringLiteral("x-activation_required")).toBool()
        || extensions.value(QStringLiteral("x-live_market_data")).toBool(true)
        || !doc.value(QStringLiteral("rules")).isArray()) return false;
    const auto decisions = doc.value(QStringLiteral("review_decisions")).toObject();
    const QString quantumText=s(decisions,"unit_quantum").trimmed();
    static const QRegularExpression quantumPattern(QStringLiteral("^(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?$"));
    if (!quantumPattern.match(quantumText).hasMatch()) return false;
    const auto quantum=business::parseDecimal(QStringView(quantumText));
    const auto* quantumValue=std::get_if<business::DecimalValue>(&quantum);
    if (!quantumValue || !business::decimalIsPositive(*quantumValue)) return false;
    for (const auto* key : {"products", "price_range", "taxonomy", "preserve_unknown_columns", "keep_quantity_unbound"})
        if (!decisions.value(QLatin1String(key)).toBool()) return false;
    for (const auto& value : doc.value(QStringLiteral("rules")).toArray()) {
        const auto r = value.toObject();
        if (r.value(QStringLiteral("enabled")).toBool(true) || r.value(QStringLiteral("review_required")).toBool(true)
            || !r.value(QStringLiteral("activation_required")).toBool()) return false;
    }
    return true;
}
} // namespace

class WorkspaceController::Impl {
public:
    explicit Impl(WorkspaceController* controller) : owner(controller) {}
    WorkspaceController* owner;
    QThread* const thread = QThread::currentThread();
    ledger::SqliteEventStore store;
    WorkspaceProjection p;
    QHash<QString, QJsonObject> profiles;
    QJsonObject runDocument;
    runtime::RunSnapshot runtime;
    QString activeRun;
    bool batching = false;
    void notify() { if (!batching) emit owner->changed(); }
    bool error(const QString& message) {
        p.lastError = message; emit owner->errorOccurred(message); notify(); return false;
    }
    bool ready() {
        if (QThread::currentThread() != thread) return error(QStringLiteral("WRONG_THREAD"));
        return p.opened || error(QStringLiteral("WORKSPACE_CLOSED"));
    }
    template<class T> bool accepted(const ledger::StoreResult<T>& result) {
        if (const auto* failure = std::get_if<ledger::StoreError>(&result)) {
            store.rollback(); return error(failure->code + QStringLiteral(": ") + failure->message);
        }
        return true;
    }
    bool active() const { return !activeRun.isEmpty(); }
    void flags(WorkspaceProjection& output) const {
        output.active = active();
        const bool profilePresent = std::any_of(output.profiles.cbegin(),output.profiles.cend(),
            [&](const ProfileRow& row) { return row.id == output.selectedProfileId; });
        output.canStart = output.opened && !output.readOnly && !output.dirty && !active()
            && output.mode == QStringLiteral("Replay") && profilePresent;
        output.canPause = active() && runtime.runState != QStringLiteral("Paused");
        output.canResume = active() && runtime.runState == QStringLiteral("Paused");
        output.canStop = active();
        output.canStep = active() && output.fixtureStep < output.fixtureStepCount;
    }
    QString source() const { return p.mode == QStringLiteral("Demo") ? QStringLiteral("synthetic_demo") : QStringLiteral("synthetic_replay"); }
    bool transitionAllowed() {
        if (!ready()) return false;
        if (active()) return error(QStringLiteral("RUN_ACTIVE: stop the current run first"));
        if (p.dirty) return error(QStringLiteral("REVIEW_DIRTY: save or explicitly discard the review draft first"));
        return true;
    }
    bool readProfiles(WorkspaceProjection& output, QHash<QString,QJsonObject>& documents) {
        auto builtin = builtinDocument(); const auto b = builtin.value(QStringLiteral("profile")).toObject();
        output.profiles = {{s(b,"id"),s(b,"name"),QStringLiteral("synthetic_fixture"),{},QStringLiteral("SyntheticOnly"),{},1,true,false,false}};
        documents.insert(WorkspaceController::builtinProfileId(), builtin);
        const QDir dir(QDir(p.rootPath).filePath(QStringLiteral("profiles")));
        for (const auto& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QJsonObject doc; QString message;
            if (!config::ProfileStore::load(info.absoluteFilePath(), doc, &message)) return error(QStringLiteral("PROFILE_LOAD_FAILED: ") + message);
            if (!profileValid(doc)) return error(QStringLiteral("PROFILE_INVALID: ") + info.fileName());
            const auto profile = doc.value(QStringLiteral("profile")).toObject(), sourceObject = doc.value(QStringLiteral("source")).toObject();
            const QString id = s(profile,"id");
            if (info.fileName() != id + QStringLiteral(".json") || documents.contains(id)) return error(QStringLiteral("PROFILE_ID_CONFLICT"));
            documents.insert(id,doc);
            output.profiles.push_back({id,s(profile,"name"),s(sourceObject,"format"),s(sourceObject,"sha256"),
                QStringLiteral("ReviewedDisabled"),info.absoluteFilePath(),profile.value(QStringLiteral("revision")).toInt(),false,false,true});
        }
        for (auto& row : output.profiles) row.selected = row.id == output.selectedProfileId;
        return true;
    }
    bool refresh(const WorkspaceProjection* candidate = nullptr) {
        if (!ready()) return false;
        WorkspaceProjection next = candidate ? *candidate : p;
        QHash<QString,QJsonObject> documents;
        if (!readProfiles(next,documents)) return false;
        const auto runsResult = store.committedRuns();
        if (!accepted(runsResult)) return false;
        next.runs.clear(); next.records.clear(); next.listings.clear();
        next.confirmedSuccess = next.unknownCount = next.reservationCount = 0;
        next.matchedCount = next.failedCount = next.noMatchCount = next.needsReviewCount = next.dispatchedCount = next.collectedCount = 0;
        next.runProfileId.clear(); next.runProfileName.clear(); next.runProfileRevision = 0;
        next.clockDomainId.clear(); next.source.clear(); next.nowMonoMs = 0; next.fixtureStep = 0;
        next.runState = QStringLiteral("Ready"); next.historyReadOnly = false;
        QJsonObject selectedRunDocument;
        for (const auto& run : std::get<QVector<ledger::StoredRun>>(runsResult)) {
            const auto eventResult = store.eventsForRun(run.runId);
            const auto ledgerResult = store.snapshot(run.runId);
            const auto auditResult = store.recoveryAudit(run.runId);
            if (!accepted(eventResult) || !accepted(ledgerResult) || !accepted(auditResult)) return false;
            const auto& events = std::get<QVector<ledger::EventDraft>>(eventResult);
            const auto& snap = std::get<ledger::LedgerSnapshot>(ledgerResult);
            const auto& audits = std::get<QVector<ledger::RecoveryAuditRecord>>(auditResult);
            RunRow row; row.id = run.runId; row.state = QStringLiteral("Recovered"); row.recovered = !audits.isEmpty();
            QJsonObject profileDocument;
            for (const auto& event : events) {
                if (event.type == QStringLiteral("Start")) {
                    profileDocument = event.payload.value(QStringLiteral("profile_snapshot")).toObject();
                    const auto profile = profileDocument.value(QStringLiteral("profile")).toObject();
                    row.profileId = s(profile,"id"); row.profileName = s(profile,"name"); row.profileRevision = profile.value(QStringLiteral("revision")).toInt();
                    row.mode = s(event.payload,"mode"); row.source = s(event.payload,"source");
                }
                if (event.payload.contains(QStringLiteral("run_state"))) row.state = s(event.payload,"run_state");
            }
            if (run.runId != activeRun && row.state != QStringLiteral("Completed") && row.state != QStringLiteral("Stopped")) {
                row.state = QStringLiteral("Recovered"); row.recovered = true;
            }
            row.confirmedSuccess = snap.confirmedSuccess; row.reservationCount = snap.unresolvedReservations;
            for (const auto& attempt : snap.attempts) if (attempt.state == QStringLiteral("Unknown")) ++row.unknownCount;
            next.runs.push_back(row);
            if (run.runId != next.selectedRunId) continue;
            selectedRunDocument = profileDocument;
            next.runProfileId = row.profileId; next.runProfileName = row.profileName; next.runProfileRevision = row.profileRevision;
            next.runState = row.state; next.source = row.source;
            next.confirmedSuccess = row.confirmedSuccess; next.unknownCount = row.unknownCount; next.reservationCount = row.reservationCount;
            next.historyReadOnly = run.runId != activeRun;
            QHash<QString,int> listingIndex;
            QHash<QString,QString> successfulTransactions;
            QHash<QString,QString> successfulAttempts;
            for (const auto& attempt : snap.attempts)
                if (attempt.state == QStringLiteral("Success")) successfulAttempts.insert(attempt.attemptId,attempt.transactionId);
            for (const auto& transaction : snap.transactions)
                if (transaction.terminal == QStringLiteral("Success") && !transaction.attemptId.isEmpty()
                    && !transaction.transactionId.isEmpty() && successfulAttempts.contains(transaction.attemptId)
                    && successfulAttempts.value(transaction.attemptId) == transaction.transactionId)
                    successfulTransactions.insert(transaction.attemptId,transaction.transactionId);
            for (const auto& event : events) {
                RecordRow record{event.eventId,event.runId,event.type,s(event.payload,"source"),s(event.payload,"reason"),
                    s(event.payload,"listing_id"),s(event.payload,"observation_id"),s(event.payload,"observed_price"),
                    s(event.payload,"confirmed_price"),event.clockDomainId,event.seq,event.atMonoMs};
                // A persisted receipt-shaped event alone is not a confirmation.
                // Only its matching terminal committed ledger transaction can
                // expose a simulated confirmed price in the UI or CSV.
                const QString attemptId = s(event.payload,"attempt_id"), transactionId = s(event.payload,"transaction_id");
                if (event.type != QStringLiteral("ReceiptConfirmed") || attemptId.isEmpty() || transactionId.isEmpty()
                    || !successfulTransactions.contains(attemptId) || successfulTransactions.value(attemptId) != transactionId)
                    record.confirmedPrice.clear();
                next.records.push_back(record);
                next.clockDomainId = event.clockDomainId; next.nowMonoMs = std::max(next.nowMonoMs,event.atMonoMs);
                next.fixtureStep = std::max(next.fixtureStep,event.payload.value(QStringLiteral("fixture_step")).toInt());
                if (event.type == QStringLiteral("Observation")) {
                    ListingRow listing;
                    listing.listingId = record.listingId; listing.observationId = record.observationId;
                    listing.productId = s(event.payload,"product_id"); listing.productName = s(event.payload,"product_name");
                    listing.source = record.source; listing.clockDomainId = event.clockDomainId;
                    listing.observedPrice = record.observedPrice; listing.status = s(event.payload,"decision"); listing.reason = record.reason;
                    listing.observedMonoMs = qint64(event.payload.value(QStringLiteral("observed_mono_ms")).toDouble());
                    listing.stale = event.payload.value(QStringLiteral("stale")).toBool();
                    for (const auto& field : event.payload.value(QStringLiteral("missing_fields")).toArray()) listing.missingFields.push_back(field.toString());
                    // Observation identity, not product identity, controls a row.
                    const QString key = listing.listingId + QChar(0x1f) + listing.observationId;
                    if (listingIndex.contains(key)) next.listings[listingIndex.value(key)] = listing;
                    else { listingIndex.insert(key,next.listings.size()); next.listings.push_back(listing); }
                    if (listing.status == QStringLiteral("Match")) ++next.matchedCount;
                    if (listing.status == QStringLiteral("NoMatch")) ++next.noMatchCount;
                    if (listing.status == QStringLiteral("NeedsReview")) ++next.needsReviewCount;
                }
                if (event.type == QStringLiteral("DispatchSimulated")) ++next.dispatchedCount;
                if (event.type == QStringLiteral("ReceiptConfirmed") && !record.confirmedPrice.isEmpty()) {
                    for (auto& listing : next.listings)
                        if (listing.listingId == record.listingId && listing.observationId == record.observationId) listing.confirmedPrice = record.confirmedPrice;
                }
            }
            for (const auto& attempt : snap.attempts) {
                if (attempt.state == QStringLiteral("Failed")) ++next.failedCount;
                if (attempt.kind == QStringLiteral("collection") && attempt.state == QStringLiteral("Success")) next.collectedCount += attempt.quantity;
            }
            for (const auto& audit : audits) {
                RecordRow r; r.eventId = QStringLiteral("recovery:") + audit.attemptId; r.runId = run.runId;
                r.type = QStringLiteral("Recovery"); r.source = QStringLiteral("committed_recovery_audit");
                r.reason = audit.previousState + QStringLiteral(" → ") + audit.recoveredState + QStringLiteral("; ") + audit.reason;
                next.records.push_back(r);
            }
        }
        next.lastError.clear(); flags(next); p = std::move(next); profiles = std::move(documents);
        if (!active()) runDocument = selectedRunDocument;
        notify(); return true;
    }
    ledger::EventDraft event(const QString& type, const QJsonObject& payload, const runtime::RunSnapshot& state, qint64 seq) const {
        ledger::EventDraft e;
        e.eventId = uid(QStringLiteral("event-")); e.runId = state.runId; e.sessionId = state.sessionId;
        e.clockDomainId = state.clockDomainId; e.stepId = state.stepId; e.seq = seq;
        e.atMonoMs = state.nowMonoMs; e.cancelEpoch = state.cancelEpoch; e.viewportGeneration = state.viewportGeneration;
        e.type = type; e.payload = payload;
        e.payload.insert(QStringLiteral("source"), source()); return e;
    }
    bool append(const ledger::EventDraft& e) { return accepted(store.appendEvent(e)); }
    bool commit(runtime::RunSnapshot next, int step, bool finish = false) {
        if (!accepted(store.commit())) return false;
        runtime = std::move(next);
        if (finish) activeRun.clear();
        WorkspaceProjection candidate=p; candidate.fixtureStep=step;
        return refresh(&candidate);
    }
    std::optional<runtime::RunSnapshot> reduce(const runtime::RunSnapshot& state, const QString& type, const QVariantMap& payload = {}) {
        runtime::ReplayEvent e; e.eventId = uid(QStringLiteral("runtime-")); e.context = runtime::ReplayReducer::contextOf(state);
        e.type = type; e.atMonoMs = state.nowMonoMs; e.payload = payload;
        const auto result=runtime::ReplayReducer::reduce(state,e);
        if (!result.accepted || result.ignored) {
            error(QStringLiteral("RUNTIME_COMMAND_REJECTED: ")+type+QStringLiteral("; ")+result.error);
            return {};
        }
        return result.snapshot;
    }
    bool markInflightUnknown(const ledger::EventDraft& control) {
        const auto snapshot = store.snapshot(activeRun);
        if (!accepted(snapshot)) return false;
        for (const auto& attempt : std::get<ledger::LedgerSnapshot>(snapshot).attempts) {
            if (attempt.state == QStringLiteral("Dispatching") || attempt.state == QStringLiteral("Sent") || attempt.state == QStringLiteral("AwaitingReceipt")) {
                if (!accepted(store.applyReceipt({attempt.attemptId,uid(QStringLiteral("unknown-")),QStringLiteral("unknown"),QStringLiteral("ambiguous"),control.eventId}))) return false;
            }
        }
        return true;
    }
    bool control(const QString& command) {
        if (!ready()) return false;
        if (!active()) return error(QStringLiteral("NO_ACTIVE_RUN"));
        if (command == QStringLiteral("Pause") && runtime.runState == QStringLiteral("Paused")) return error(QStringLiteral("ALREADY_PAUSED"));
        if (command == QStringLiteral("Resume") && runtime.runState != QStringLiteral("Paused")) return error(QStringLiteral("NOT_PAUSED"));
        auto next = runtime; next.nowMonoMs += 10;
        const auto reduced=reduce(next,command); if (!reduced) return false; next=*reduced;
        const auto e = event(command,{{QStringLiteral("run_state"),next.runState},{QStringLiteral("fixture_step"),p.fixtureStep}},next,next.nextSeq++);
        if (!append(e)) return false;
        if (command != QStringLiteral("Resume") && !markInflightUnknown(e)) return false;
        return commit(next,p.fixtureStep,command == QStringLiteral("Stop"));
    }
    QJsonObject observation(int n, qint64 now) {
        const QChar suffix = QChar('A' + (n == 1 ? 0 : n == 4 ? 1 : n == 5 ? 2 : n == 6 ? 3 : n == 7 ? 4 : 5));
        business::ListingObservation o;
        o.observation_id = QStringLiteral("observation-") + suffix; o.market_listing_id = QStringLiteral("listing-") + suffix;
        o.product_ref = QStringLiteral("synthetic-product-1"); o.association = {QStringLiteral("fixture-association"),QStringLiteral("confirmed")};
        o.frame_ref = QStringLiteral("memory-frame-") + suffix; o.session_id = runtime.sessionId; o.clock_domain_id = runtime.clockDomainId;
        o.viewport_generation = 1; o.observed_mono_ms = n == 5 ? 0 : now;
        if (n != 6) o.price = business::Money{{QString::number(n == 7 ? 250 : n == 4 ? 149 : n == 8 ? 155 : 150),0},QStringLiteral("synthetic-unit")};
        for (const auto* field : {"product_ref","price","wear","condition","season","ownership","grade","rarity","publicity"}) {
            business::FieldEvidence evidence; evidence.source_ref = o.frame_ref;
            if (QLatin1String(field) == QStringLiteral("product_ref") || (QLatin1String(field) == QStringLiteral("price") && o.price)) evidence.status = QStringLiteral("observed");
            o.field_evidence.insert(QLatin1String(field),evidence);
        }
        const bool builtin = s(runDocument.value(QStringLiteral("profile")).toObject(),"id") == WorkspaceController::builtinProfileId();
        const auto rule = builtin ? fixtureRule() : savedRule(runDocument);
        business::EvaluationContext context{runtime.sessionId,runtime.clockDomainId,1,quint64(now),250,QStringLiteral("filter_only"),{QStringLiteral("synthetic-unit")}};
        const auto vr = business::validateRule(rule);
        const auto vo = business::validateObservation(o);
        QString decision = QStringLiteral("NeedsReview"), reason = QStringLiteral("RULE_INVALID");
        if (const auto* validatedRule = std::get_if<business::ValidatedRule>(&vr)) {
            if (const auto* validatedObservation = std::get_if<business::ValidatedObservation>(&vo)) {
                const auto result = business::evaluate(*validatedRule,*validatedObservation,context);
                decision = result.status; reason = result.primary_reason;
            } else reason = QStringLiteral("OBSERVATION_INVALID");
        }
        QJsonArray missing; if (!o.price) missing.append(QStringLiteral("price"));
        return {{QStringLiteral("listing_id"),*o.market_listing_id},{QStringLiteral("observation_id"),o.observation_id},
            {QStringLiteral("product_id"),*o.product_ref},{QStringLiteral("product_name"),QStringLiteral("合成商品 · 非市场数据")},
            {QStringLiteral("observed_price"),o.price ? priceText(o.price->value) : QString()},
            {QStringLiteral("observed_mono_ms"),qint64(o.observed_mono_ms)}, {QStringLiteral("stale"),quint64(now)-o.observed_mono_ms>250},
            {QStringLiteral("missing_fields"),missing},{QStringLiteral("decision"),decision},{QStringLiteral("reason"),reason}};
    }
    bool dispatch(const QString& suffix, ledger::EventDraft dispatchEvent) {
        const QString id = activeRun + QStringLiteral(":attempt-") + suffix;
        ledger::AttemptDraft a; a.attemptId=id; a.intentId=id+QStringLiteral(":intent"); a.runId=activeRun;
        a.ruleTaskId=QStringLiteral("fixture-rule"); a.ruleRevision=1; a.reservationId=id+QStringLiteral(":reservation");
        a.scopeId=activeRun+QStringLiteral(":fixture-quota"); a.quotaTarget=3;
        if (!append(dispatchEvent) || !accepted(store.reserveAttempt(a))) return false;
        return accepted(store.recordDispatch({id,dispatchEvent.eventId,QStringLiteral("Dispatching"),QStringLiteral("possibly_dispatched")}));
    }
    bool receipt(const QString& suffix, bool success, ledger::EventDraft e) {
        const QString id=activeRun+QStringLiteral(":attempt-")+suffix, receiptId=id+QStringLiteral(":receipt");
        e.payload.insert(QStringLiteral("attempt_id"),id);
        e.payload.insert(QStringLiteral("transaction_id"),id+QStringLiteral(":transaction"));
        if (!append(e) || !accepted(store.applyReceipt({id,receiptId,success?QStringLiteral("success"):QStringLiteral("failed"),QStringLiteral("confirmed"),e.eventId}))) return false;
        return accepted(store.commitLedger({id+QStringLiteral(":transaction"),id,success?QStringLiteral("Success"):QStringLiteral("Failed"),false,receiptId,e.eventId}));
    }
    bool step() {
        if (!ready()) return false;
        if (!active()) return error(QStringLiteral("NO_ACTIVE_RUN"));
        if (p.fixtureStep >= p.fixtureStepCount) return error(QStringLiteral("FIXTURE_FINISHED"));
        const int n = p.fixtureStep + 1;
        auto next=runtime; next.nowMonoMs += 100;
        const bool paused=next.runState==QStringLiteral("Paused");
        const bool builtin=s(runDocument.value(QStringLiteral("profile")).toObject(),"id")==WorkspaceController::builtinProfileId();
        if (n==1 || n>=4) {
            const auto payload=observation(n,next.nowMonoMs);
            if (!append(event(QStringLiteral("Observation"),payload,next,next.nextSeq++))) return false;
            if (builtin && (n==4 || n==8) && s(payload,"decision")==QStringLiteral("Match")) {
                auto dispatchEvent=event(QStringLiteral("DispatchSimulated"),payload,next,next.nextSeq++);
                if (!dispatch(n==4?QStringLiteral("B"):QStringLiteral("F"),dispatchEvent)) return false;
                if (n==8) {
                    auto failed=event(QStringLiteral("ReceiptFailed"),payload,next,next.nextSeq++);
                    failed.payload.insert(QStringLiteral("reason"),QStringLiteral("SYNTHETIC_REJECTED"));
                    if (!receipt(QStringLiteral("F"),false,failed)) return false;
                }
            }
        } else if (n==2 && builtin) {
            const auto payload=observation(1,next.nowMonoMs);
            if (!dispatch(QStringLiteral("A"),event(QStringLiteral("DispatchSimulated"),payload,next,next.nextSeq++))) return false;
        } else if (n==3 && builtin) {
            const auto snapshot=store.snapshot(activeRun); if (!accepted(snapshot)) return false;
            bool pending=false;
            for (const auto& attempt:std::get<ledger::LedgerSnapshot>(snapshot).attempts)
                if (attempt.attemptId==activeRun+QStringLiteral(":attempt-A") && attempt.state==QStringLiteral("Dispatching")) pending=true;
            auto payload=observation(1,next.nowMonoMs);
            if (pending) {
                payload.insert(QStringLiteral("confirmed_price"),QStringLiteral("148"));
                payload.insert(QStringLiteral("reason"),QStringLiteral("SIMULATED_CONFIRMED_RECEIPT"));
                if (!receipt(QStringLiteral("A"),true,event(QStringLiteral("ReceiptConfirmed"),payload,next,next.nextSeq++))) return false;
            } else if (!append(event(QStringLiteral("ReceiptSkipped"),{{QStringLiteral("reason"),QStringLiteral("SKIPPED_UNKNOWN_NO_AUTOMATIC_RECONCILIATION")}},next,next.nextSeq++))) return false;
        }
        if (!paused) next.runState=n==8?QStringLiteral("Completed"):QStringLiteral("Watchlist");
        const auto progress=event(QStringLiteral("FixtureStep"),{{QStringLiteral("fixture_step"),n},{QStringLiteral("run_state"),next.runState},
            {QStringLiteral("reason"),builtin?QStringLiteral("SYNTHETIC_ONLY"):QStringLiteral("SAVED_PROFILE_DISABLED")}},next,next.nextSeq++);
        if (!append(progress)) return false;
        if (n==8 && !markInflightUnknown(progress)) return false;
        return commit(next,n,n==8 && !paused);
    }
};

WorkspaceController::WorkspaceController(QObject* parent) : QObject(parent),d(std::make_unique<Impl>(this)) {}
WorkspaceController::~WorkspaceController() = default;
QString WorkspaceController::builtinProfileId() { return QStringLiteral("builtin-synthetic-fixture"); }
const WorkspaceProjection& WorkspaceController::projection() const { return d->p; }
QString WorkspaceController::lastError() const { return d->p.lastError; }
QJsonObject WorkspaceController::selectedProfileDocument() const { return d->profiles.value(d->p.selectedProfileId); }
QJsonObject WorkspaceController::runProfileDocument() const { return d->runDocument; }
bool WorkspaceController::open(const QString& rootPath, bool readOnly)
{
    if (d->p.opened) return d->error(QStringLiteral("WORKSPACE_ALREADY_OPEN"));
    if (rootPath.trimmed().isEmpty()) return d->error(QStringLiteral("INVALID_PATH"));
    d->p.rootPath=QFileInfo(rootPath).absoluteFilePath();
    d->p.databasePath=QDir(d->p.rootPath).filePath(QStringLiteral("workspace.sqlite"));
    if (!readOnly && !QDir().mkpath(QDir(d->p.rootPath).filePath(QStringLiteral("profiles")))) return d->error(QStringLiteral("WORKSPACE_DIRECTORY_FAILED"));
    ledger::SqliteEventStore::OpenOptions options; options.readOnly=readOnly; options.busyTimeoutMs=100;
    if (!d->accepted(d->store.open(d->p.databasePath,options))) return false;
    d->p.readOnly=readOnly; d->p.opened=true;
    if (!d->refresh()) { d->store.close(); d->p.opened=false; return false; }
    if (!d->p.runs.isEmpty()) { auto candidate=d->p; candidate.selectedRunId=d->p.runs.first().id; return d->refresh(&candidate); }
    return true;
}
bool WorkspaceController::close()
{
    if (!d->accepted(d->store.close())) return false;
    d->activeRun.clear(); d->runtime={}; d->runDocument={}; d->profiles.clear(); d->p={}; d->notify(); return true;
}
bool WorkspaceController::refresh() { return d->refresh(); }
bool WorkspaceController::saveReviewedPreview(const QJsonObject& preview,const config::ReviewChoices& choices)
{
    if (!d->ready()) return false;
    if (d->active()) return d->error(QStringLiteral("RUN_ACTIVE"));
    if (d->p.readOnly) return d->error(QStringLiteral("DB_READ_ONLY"));
    const QString quantumText=choices.quantum.trimmed();
    static const QRegularExpression quantumPattern(QStringLiteral("^(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?$"));
    if (!quantumPattern.match(quantumText).hasMatch()) return d->error(QStringLiteral("QUANTUM_INVALID: positive plain decimal required"));
    const auto quantum=business::parseDecimal(QStringView(quantumText));
    const auto* quantumValue=std::get_if<business::DecimalValue>(&quantum);
    if (!quantumValue || !business::decimalIsPositive(*quantumValue)) return d->error(QStringLiteral("QUANTUM_INVALID: positive plain decimal required"));
    const auto result=config::ProfileStore::materializePreview(preview,choices);
    if (const auto* failure=std::get_if<config::ProfileStoreError>(&result)) return d->error(failure->code+QStringLiteral(": ")+failure->message);
    if (!profileValid(std::get<config::ProfileCommitResult>(result).document)) return d->error(QStringLiteral("PROFILE_INVALID: review source or document is invalid"));
    // Validate existing files before committing anything. A malformed catalogue
    // must not turn a failed save into an on-disk overwrite with a stale UI.
    auto catalogue=d->p; QHash<QString,QJsonObject> documents;
    if (!d->readProfiles(catalogue,documents)) return false;
    const QString id=std::get<config::ProfileCommitResult>(result).profileId;
    const QString path=QDir(d->p.rootPath).filePath(QStringLiteral("profiles/")+id+QStringLiteral(".json"));
    QString error;
    if (!config::ProfileStore::commitPreview(preview,choices,path,&error)) return d->error(QStringLiteral("PROFILE_SAVE_FAILED: ")+error);
    auto candidate=d->p; candidate.dirty=false; return d->refresh(&candidate);
}
bool WorkspaceController::selectProfile(const QString& profileId)
{
    if (!d->transitionAllowed()) return false;
    if (!d->profiles.contains(profileId)) return d->error(QStringLiteral("PROFILE_NOT_FOUND"));
    auto candidate=d->p; candidate.selectedProfileId=profileId; return d->refresh(&candidate);
}
bool WorkspaceController::selectBuiltInFixture() { return selectProfile(builtinProfileId()); }
bool WorkspaceController::selectRun(const QString& runId)
{
    if (runId.isEmpty()) {
        if (!d->ready()) return false;
        if (d->p.dirty) return d->error(QStringLiteral("REVIEW_DIRTY"));
        auto candidate=d->p; candidate.selectedRunId=d->activeRun; return d->refresh(&candidate);
    }
    if (!d->transitionAllowed()) return false;
    bool found=false; for (const auto& row:d->p.runs) if (row.id==runId) found=true;
    if (!found) return d->error(QStringLiteral("RUN_NOT_FOUND"));
    auto candidate=d->p; candidate.selectedRunId=runId; return d->refresh(&candidate);
}
bool WorkspaceController::setMode(const QString& mode)
{
    if (!d->transitionAllowed()) return false;
    if (mode!=QStringLiteral("Demo") && mode!=QStringLiteral("Replay")) return d->error(QStringLiteral("MODE_INVALID"));
    if (mode==d->p.mode) { d->p.lastError.clear(); d->notify(); return true; }
    auto candidate=d->p; candidate.mode=mode; candidate.selectedRunId.clear();
    return d->refresh(&candidate);
}
void WorkspaceController::setReviewDirty(bool dirty) { d->p.dirty=dirty; d->flags(d->p); d->notify(); }
void WorkspaceController::discardReviewDraft() { setReviewDirty(false); }
bool WorkspaceController::start()
{
    if (!d->transitionAllowed()) return false;
    if (d->p.readOnly) return d->error(QStringLiteral("DB_READ_ONLY"));
    if (d->p.mode!=QStringLiteral("Replay")) return d->error(QStringLiteral("MODE_NOT_REPLAY: select Replay before starting the persisted fixture"));
    if (!d->profiles.contains(d->p.selectedProfileId)) return d->error(QStringLiteral("PROFILE_NOT_SELECTED"));
    // Re-read selected profile at the boundary; never freeze stale or edited JSON.
    if (!d->refresh()) return false;
    if (!d->profiles.contains(d->p.selectedProfileId)) return d->error(QStringLiteral("PROFILE_NOT_FOUND"));
    const auto document=d->profiles.value(d->p.selectedProfileId);
    runtime::RunSnapshot next;
    next.runId=uid(QStringLiteral("run-")); next.sessionId=uid(QStringLiteral("session-")); next.clockDomainId=uid(QStringLiteral("clock-"));
    next.stepId=QStringLiteral("s0"); next.viewportGeneration=1; next.mode=d->p.mode; next.targetQuantity=3;
    const auto reduced=d->reduce(next,QStringLiteral("Start"),{{QStringLiteral("configuration_reviewed"),true},{QStringLiteral("schedule_due"),true}});
    if (!reduced) return false;
    next=*reduced;
    const auto event=d->event(QStringLiteral("Start"),{{QStringLiteral("profile_snapshot"),document},{QStringLiteral("mode"),d->p.mode},
        {QStringLiteral("run_state"),next.runState},{QStringLiteral("fixture_step"),0}},next,next.nextSeq++);
    if (!d->append(event) || !d->accepted(d->store.commit())) return false;
    d->runtime=next; d->activeRun=next.runId; d->runDocument=document;
    auto candidate=d->p; candidate.selectedRunId=next.runId;
    return d->refresh(&candidate);
}
bool WorkspaceController::pause() { return d->control(QStringLiteral("Pause")); }
bool WorkspaceController::resume()
{
    if (d->p.fixtureStep==d->p.fixtureStepCount && d->active()) return stop();
    return d->control(QStringLiteral("Resume"));
}
bool WorkspaceController::step() { return d->step(); }
bool WorkspaceController::advance(int steps)
{
    if (steps<1 || steps>10000) return d->error(QStringLiteral("ADVANCE_RANGE_INVALID"));
    if (!d->ready() || !d->active()) return d->error(QStringLiteral("NO_ACTIVE_RUN"));
    d->batching=true; bool ok=true;
    for (int i=0;i<steps && d->active() && d->p.fixtureStep<d->p.fixtureStepCount;++i) if (!d->step()) { ok=false; break; }
    d->batching=false; d->notify(); return ok;
}
bool WorkspaceController::stop() { return d->control(QStringLiteral("Stop")); }
QString WorkspaceController::csvCell(QString value)
{
    int index=0; while(index<value.size() && value.at(index).isSpace()) ++index;
    const bool formula=index<value.size() && QStringLiteral("=+-@").contains(value.at(index));
    if (formula || (!value.isEmpty() && (value.front()==QLatin1Char('\t') || value.front()==QLatin1Char('\r') || value.front()==QLatin1Char('\n')))) value.prepend(QLatin1Char('\''));
    value.replace(QStringLiteral("\""),QStringLiteral("\"\"")); return QLatin1Char('\"')+value+QLatin1Char('\"');
}
bool WorkspaceController::exportRecordsCsv(const QString& path)
{
    if (!d->ready()) return false;
    // QSaveFile atomically replaces its destination. Never let a record export
    // replace the open database, its sidecars, or any saved profile.
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
    const QString target = resolved(path), database = resolved(d->p.databasePath);
    const QString profileDirectory = resolved(QDir(d->p.rootPath).filePath(QStringLiteral("profiles")));
    bool protectedPath = target.compare(profileDirectory,pathCase)==0
        || target.startsWith(profileDirectory+QLatin1Char('/'),pathCase);
    for (const auto& suffix : {QString(),QStringLiteral("-wal"),QStringLiteral("-shm"),QStringLiteral("-journal"),QStringLiteral(".writer.lock")})
        protectedPath |= target.compare(database+suffix,pathCase)==0;
    if (protectedPath) return d->error(QStringLiteral("CSV_TARGET_PROTECTED: choose an export file outside workspace storage"));
    QByteArray bytes("\xEF\xBB\xBF");
    bytes += "run_id,event_id,type,mode,run_profile_id,run_profile_revision,source,clock_domain,mono_ms,listing_id,observation_id,observed_price,confirmed_simulated_price,reason\r\n";
    QString runMode;
    for (const auto& run:d->p.runs) if(run.id==d->p.selectedRunId) { runMode=run.mode; break; }
    for (const auto& row:d->p.records) {
        QStringList fields{row.runId,row.eventId,row.type,runMode,d->p.runProfileId,QString::number(d->p.runProfileRevision),row.source,row.clockDomainId,QString::number(row.atMonoMs),row.listingId,row.observationId,row.observedPrice,row.confirmedPrice,row.reason};
        for (auto& field:fields) field=csvCell(field);
        bytes+=fields.join(QLatin1Char(',')).toUtf8()+"\r\n";
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) return d->error(QStringLiteral("CSV_WRITE_FAILED: ")+file.errorString());
    d->p.lastError.clear(); d->notify(); return true;
}
} // namespace relink::workspace
