#include "application/workspace/workspace_controller.h"
#include "application/workspace/profile_catalog.h"
#include "application/workspace/history_query_service.h"
#include "application/workspace/records_exporter.h"
#include "config/profile_store.h"
#include "ledger/sqlite_event_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <iostream>

using namespace relink::workspace;
using namespace relink::config;
using namespace relink;

namespace {
int assertions = 0;
int failures = 0;

bool check(bool passed, const QString& name)
{
    ++assertions;
    if (!passed) ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name.toStdString() << '\n';
    return passed;
}

bool good(bool passed, const WorkspaceController& controller, const QString& name)
{
    check(passed, name);
    if (!passed) std::cout << "  error=" << controller.lastError().toStdString() << '\n';
    return passed;
}

QJsonObject preview()
{
    return preview13Columns(QByteArrayLiteral("AUG Demo|any|unowned|orange|S|none|230|600|0.399|rare|default|7|9\n"));
}

ReviewChoices choices(QString name = QStringLiteral("Reviewed PR11"))
{
    ReviewChoices out;
    out.profileName = name;
    out.quantum = QStringLiteral("0.01");
    out.confirmProducts = out.confirmPriceRange = out.confirmTaxonomy = true;
    out.preserveUnknownColumns = out.keepQuantityUnbound = true;
    return out;
}

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QByteArray hash(const QString& path)
{
    return QCryptographicHash::hash(bytes(path), QCryptographicHash::Sha256).toHex();
}

bool writeBytes(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

ProfileRow savedProfile(const WorkspaceProjection& view, const QString& name = {})
{
    for (const auto& row : view.profiles)
        if (!row.builtin && (name.isEmpty() || row.name == name)) return row;
    return {};
}

QStringList diskFiles(const QString& root)
{
    QStringList out;
    QDir directory(root);
    for (const auto& item : directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
        out << item.fileName();
    return out;
}

// Deliberately excludes lastError and transient error-display capability flags.
// A failed mutation must preserve all user-observable business data.
QByteArray businessState(const WorkspaceProjection& p)
{
    QJsonArray profiles, runs, listings, records;
    for (const auto& r : p.profiles)
        profiles.append(QJsonObject{{"id",r.id},{"name",r.name},{"revision",r.revision},
            {"source",r.source},{"hash",r.sourceHash},{"selected",r.selected},{"review",r.reviewState}});
    for (const auto& r : p.runs)
        runs.append(QJsonObject{{"id",r.id},{"profile",r.profileId},{"revision",r.profileRevision},
            {"state",r.state},{"mode",r.mode},{"source",r.source},{"success",r.confirmedSuccess},
            {"unknown",r.unknownCount},{"reserved",r.reservationCount},{"recovered",r.recovered}});
    for (const auto& r : p.listings)
        listings.append(QJsonObject{{"id",r.listingId},{"observation",r.observationId},
            {"product",r.productId},{"source",r.source},{"clock",r.clockDomainId},
            {"observed",r.observedPrice},{"confirmed",r.confirmedPrice},{"state",r.status},
            {"reason",r.reason},{"missing",QJsonArray::fromStringList(r.missingFields)},
            {"time",r.observedMonoMs},{"stale",r.stale}});
    for (const auto& r : p.records)
        records.append(QJsonObject{{"id",r.eventId},{"run",r.runId},{"type",r.type},
            {"source",r.source},{"reason",r.reason},{"listing",r.listingId},{"observation",r.observationId},
            {"observed",r.observedPrice},{"confirmed",r.confirmedPrice},{"clock",r.clockDomainId},
            {"seq",r.seq},{"time",r.atMonoMs}});
    return QJsonDocument(QJsonObject{{"profiles",profiles},{"runs",runs},{"listings",listings},{"records",records},
        {"selectedProfile",p.selectedProfileId},{"selectedRun",p.selectedRunId},{"mode",p.mode},
        {"runState",p.runState},{"profile",p.runProfileId},{"revision",p.runProfileRevision},
        {"source",p.source},{"clock",p.clockDomainId},{"step",p.fixtureStep},{"now",p.nowMonoMs},
        {"success",p.confirmedSuccess},{"unknown",p.unknownCount},{"reserved",p.reservationCount},
        {"matched",p.matchedCount},{"failed",p.failedCount},{"noMatch",p.noMatchCount},
        {"needsReview",p.needsReviewCount},{"dispatched",p.dispatchedCount},{"collected",p.collectedCount},
        {"active",p.active},{"dirty",p.dirty}}).toJson(QJsonDocument::Compact);
}

class RawConnection {
public:
    explicit RawConnection(const QString& path, bool readOnly = false)
    {
        name = QStringLiteral("workspace_test_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        database.setConnectOptions(readOnly ? QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=30")
                                            : QStringLiteral("QSQLITE_BUSY_TIMEOUT=30"));
        opened = database.open();
    }
    ~RawConnection()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    bool exec(const QString& sql)
    {
        QSqlQuery q(database);
        const bool result = q.exec(sql);
        if (!result) std::cout << "  sql_error=" << q.lastError().text().toStdString() << '\n';
        return result;
    }
    qint64 scalar(const QString& sql, const QString& run = {})
    {
        QSqlQuery q(database);
        if (!q.prepare(sql)) return -1;
        if (sql.contains(QLatin1Char('?'))) q.addBindValue(run);
        if (!q.exec() || !q.next()) return -1;
        return q.value(0).toLongLong();
    }
    QSqlDatabase database;
    bool opened = false;
private:
    QString name;
};

void testProfileReview(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("profiles_open"))) return;
    check(c.projection().opened && !c.projection().readOnly, QStringLiteral("profiles_opened_projection"));
    const auto incoming = preview();
    const auto originalPreview = QJsonDocument(incoming).toJson();
    const QString selectedBefore = c.projection().selectedProfileId;
    auto incomplete = choices();
    incomplete.confirmProducts = false;
    check(!c.saveReviewedPreview(incoming, incomplete), QStringLiteral("unreviewed_preview_cannot_save"));
    check(!c.lastError().isEmpty() && c.projection().selectedProfileId == selectedBefore,
          QStringLiteral("unreviewed_failure_retains_selection"));
    check(savedProfile(c.projection()).id.isEmpty(), QStringLiteral("unreviewed_failure_does_not_publish_profile"));

    if (!good(c.saveReviewedPreview(incoming, choices()), c, QStringLiteral("reviewed_profile_atomic_save"))) return;
    ProfileRow first = savedProfile(c.projection());
    if (!check(!first.id.isEmpty() && QFileInfo::exists(first.path), QStringLiteral("saved_profile_file_exists"))) return;
    check(first.revision == 1 && first.sourceHash == incoming.value(QStringLiteral("source_sha256")).toString(),
          QStringLiteral("profile_revision_one_and_source_hash"));
    check(c.projection().selectedProfileId == selectedBefore, QStringLiteral("save_does_not_implicitly_select"));
    check(QJsonDocument(incoming).toJson() == originalPreview, QStringLiteral("save_keeps_preview_read_only"));
    QJsonObject loaded;
    QString error;
    check(ProfileStore::load(first.path, loaded, &error), QStringLiteral("saved_profile_loads_as_config_v2"));
    const auto rules = loaded.value(QStringLiteral("rules")).toArray();
    check(!rules.isEmpty(), QStringLiteral("reviewed_profile_contains_rule_snapshot"));
    for (const auto& rule : rules) {
        check(!rule.toObject().value(QStringLiteral("enabled")).toBool(true),
              QStringLiteral("reviewed_rule_stays_disabled"));
        check(!rule.toObject().value(QStringLiteral("review_required")).toBool(true),
              QStringLiteral("explicit_review_is_recorded"));
    }
    check(first.activationRequired && loaded.value(QStringLiteral("extensions")).toObject()
              .value(QStringLiteral("x-activation_required")).toBool(),
          QStringLiteral("profile_activation_stays_required"));

    if (!good(c.selectProfile(first.id), c, QStringLiteral("explicit_profile_selection"))) return;
    check(c.selectedProfileDocument().value(QStringLiteral("profile")).toObject()
              .value(QStringLiteral("id")).toString() == first.id, QStringLiteral("selected_document_matches_profile"));
    const QByteArray originalFile = bytes(first.path);
    const QByteArray stateBefore = businessState(c.projection());
    check(!c.saveReviewedPreview(incoming, incomplete), QStringLiteral("incomplete_second_review_rejected"));
    check(bytes(first.path) == originalFile && c.projection().selectedProfileId == first.id,
          QStringLiteral("failed_review_keeps_file_and_selection"));
    check(businessState(c.projection()) == stateBefore, QStringLiteral("failed_review_keeps_business_projection"));

    QJsonObject malformed = incoming;
    malformed.insert(QStringLiteral("committable"), true);
    check(!c.saveReviewedPreview(malformed, choices()), QStringLiteral("committable_preview_marker_rejected"));
    check(bytes(first.path) == originalFile && c.projection().selectedProfileId == first.id,
          QStringLiteral("malformed_preview_keeps_file_and_selection"));

    if (!good(c.saveReviewedPreview(incoming, choices()), c, QStringLiteral("same_profile_second_save"))) return;
    const auto second = savedProfile(c.projection());
    check(second.id == first.id && second.revision == 2, QStringLiteral("same_identity_increments_revision"));
    check(second.sourceHash == first.sourceHash, QStringLiteral("revision_preserves_original_source_hash"));

    // A real invalid existing file causes the ProfileStore boundary to fail.
    // Its bytes and the currently selected profile are retained.
    QFile invalid(first.path);
    if (check(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate), QStringLiteral("invalid_target_fixture_open"))) {
        invalid.write("{\"not\":\"ConfigV2\"}");
        invalid.close();
        const auto corruptBytes = bytes(first.path);
        check(!c.saveReviewedPreview(incoming, choices()), QStringLiteral("invalid_existing_target_save_rejected"));
        check(bytes(first.path) == corruptBytes && c.projection().selectedProfileId == first.id,
              QStringLiteral("failed_disk_target_keeps_bytes_and_current_selection"));
    }
    good(c.close(), c, QStringLiteral("profiles_close"));
}

void testGuardsAndControls(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("guards_open"))
        || !good(c.saveReviewedPreview(preview(), choices()), c, QStringLiteral("guards_seed_profile"))) return;
    const auto saved = savedProfile(c.projection());
    if (!good(c.selectBuiltInFixture(), c, QStringLiteral("guards_select_fixture"))) return;
    c.setReviewDirty(true);
    const auto dirtyBefore = businessState(c.projection());
    check(c.projection().dirty && !c.selectProfile(saved.id), QStringLiteral("dirty_blocks_profile_change"));
    check(!c.setMode(QStringLiteral("Demo")), QStringLiteral("dirty_blocks_mode_change"));
    check(!c.start(), QStringLiteral("dirty_blocks_start"));
    check(businessState(c.projection()) == dirtyBefore, QStringLiteral("dirty_failures_preserve_draft_and_selection"));
    c.discardReviewDraft();
    check(!c.projection().dirty, QStringLiteral("discard_explicitly_clears_review_draft"));
    if (!good(c.setMode(QStringLiteral("Demo")), c, QStringLiteral("stopped_mode_demo"))) return;
    const auto demoBefore = businessState(c.projection());
    check(!c.projection().canStart && !c.start() && c.lastError().startsWith(QStringLiteral("MODE_NOT_REPLAY")),
          QStringLiteral("demo_cannot_start_persisted_replay"));
    check(businessState(c.projection()) == demoBefore && c.projection().runs.isEmpty(),
          QStringLiteral("demo_start_rejection_creates_no_run_or_projection"));
    if (!good(c.setMode(QStringLiteral("Replay")), c, QStringLiteral("stopped_mode_replay"))) return;
    check(!c.setMode(QStringLiteral("Observe")), QStringLiteral("live_observe_mode_rejected"));
    if (!good(c.start(), c, QStringLiteral("replay_start"))) return;
    check(c.projection().active && c.projection().canPause && !c.projection().canStart,
          QStringLiteral("running_capability_projection"));
    const auto activeBefore = businessState(c.projection());
    check(!c.selectProfile(saved.id), QStringLiteral("active_blocks_profile_switch"));
    check(!c.setMode(QStringLiteral("Demo")), QStringLiteral("active_blocks_mode_switch"));
    check(!c.selectRun(c.projection().selectedRunId), QStringLiteral("active_blocks_history_selection"));
    check(!c.saveReviewedPreview(preview(), choices(QStringLiteral("active save"))),
          QStringLiteral("active_blocks_profile_save"));
    check(businessState(c.projection()) == activeBefore, QStringLiteral("active_guard_failures_preserve_run"));
    if (!good(c.pause(), c, QStringLiteral("replay_pause"))) return;
    check(c.projection().active && c.projection().canResume && !c.projection().canPause,
          QStringLiteral("paused_capability_projection"));
    const QString pausedState = c.projection().runState;
    check(!c.selectProfile(saved.id) && !c.setMode(QStringLiteral("Demo")),
          QStringLiteral("paused_run_still_guards_selection_and_mode"));
    const int oldStep = c.projection().fixtureStep;
    if (!good(c.step(), c, QStringLiteral("paused_replay_single_step"))) return;
    check(c.projection().fixtureStep == oldStep + 1 && c.projection().runState == pausedState,
          QStringLiteral("single_step_advances_while_remaining_paused"));
    if (!good(c.resume(), c, QStringLiteral("replay_resume"))
        || !good(c.step(), c, QStringLiteral("replay_dispatch_step"))) return;
    check(c.projection().reservationCount == 1, QStringLiteral("dispatch_reservation_visible_after_commit"));
    if (!good(c.stop(), c, QStringLiteral("replay_stop"))) return;
    check(!c.projection().active && !c.projection().canResume, QStringLiteral("stop_does_not_resume_automatically"));
    check(c.projection().unknownCount == 1 && c.projection().reservationCount == 1,
          QStringLiteral("stop_retains_possibly_dispatched_as_unknown"));
    check(!c.step(), QStringLiteral("stopped_run_cannot_step"));
    check(c.projection().imageFileWriteCount == 0, QStringLiteral("replay_controls_write_no_image_files"));

    if (!good(c.start(), c, QStringLiteral("pause_unknown_new_run"))
        || !good(c.step(), c, QStringLiteral("pause_unknown_observe"))
        || !good(c.step(), c, QStringLiteral("pause_unknown_dispatch"))
        || !good(c.pause(), c, QStringLiteral("pause_after_possible_dispatch"))) return;
    check(c.projection().unknownCount == 1 && c.projection().reservationCount == 1,
          QStringLiteral("pause_keeps_possible_dispatch_unknown"));
    if (!good(c.resume(), c, QStringLiteral("resume_keeps_unknown_fact"))
        || !good(c.step(), c, QStringLiteral("unknown_receipt_fixture_does_not_reconcile"))) return;
    check(c.projection().confirmedSuccess == 0 && c.projection().unknownCount == 1,
          QStringLiteral("resume_does_not_promote_unknown_to_success"));
    good(c.stop(), c, QStringLiteral("pause_unknown_final_stop"));
}

void testFullReplayAndCommittedCounts(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("replay_open"))
        || !good(c.selectBuiltInFixture(), c, QStringLiteral("replay_select_fixture"))
        || !good(c.start(), c, QStringLiteral("full_replay_start"))) return;
    const QString run = c.projection().selectedRunId;
    check(!run.isEmpty() && c.projection().confirmedSuccess == 0, QStringLiteral("new_run_has_no_uncommitted_success"));
    int notifications = 0;
    QObject::connect(&c, &WorkspaceController::changed, [&] { ++notifications; });
    if (!good(c.advance(100), c, QStringLiteral("full_replay_coalesced_advance"))) return;
    const auto view = c.projection();
    check(notifications == 1, QStringLiteral("fast_replay_coalesces_projection_notification"));
    check(!view.active && view.fixtureStep == view.fixtureStepCount, QStringLiteral("full_replay_completed"));
    check(view.confirmedSuccess == 1 && view.unknownCount == 1 && view.reservationCount == 1,
          QStringLiteral("full_fixture_success_unknown_and_reservation"));
    check(view.failedCount == 1 && view.noMatchCount >= 1 && view.needsReviewCount >= 2,
          QStringLiteral("failed_no_match_and_review_categories_visible"));
    check(view.collectedCount == 0, QStringLiteral("fixture_does_not_invent_collection_success"));
    check(view.listings.size() == 6, QStringLiteral("same_product_six_listings_preserved"));
    QSet<QString> listingIds;
    QSet<QString> productIds;
    bool stale = false, missing = false, confirmed = false, observationOnly = false;
    for (const auto& row : view.listings) {
        listingIds.insert(row.listingId);
        productIds.insert(row.productId);
        stale |= row.stale;
        missing |= !row.missingFields.isEmpty();
        confirmed |= !row.confirmedPrice.isEmpty();
        observationOnly |= !row.observedPrice.isEmpty() && row.confirmedPrice.isEmpty();
        check(!row.observationId.isEmpty() && !row.source.isEmpty() && !row.clockDomainId.isEmpty(),
              QStringLiteral("listing_keeps_observation_source_and_clock_") + row.listingId);
        check(row.observedMonoMs >= 0 && !row.reason.isEmpty(), QStringLiteral("listing_time_and_reason_") + row.listingId);
    }
    check(listingIds.size() == 6 && productIds.size() == 1, QStringLiteral("listing_identity_is_not_product_identity"));
    check(stale && missing, QStringLiteral("stale_and_missing_fields_are_explicit"));
    check(confirmed && observationOnly, QStringLiteral("observed_and_confirmed_price_are_distinct"));
    RawConnection db(view.databasePath, true);
    if (check(db.opened, QStringLiteral("projection_oracle_readonly_db"))) {
        check(view.confirmedSuccess == db.scalar(QStringLiteral("SELECT coalesce(sum(quantity),0) FROM attempts WHERE run_id=? AND state='Success'"), run),
              QStringLiteral("success_projection_matches_committed_ledger"));
        check(view.failedCount == db.scalar(QStringLiteral("SELECT count(*) FROM attempts WHERE run_id=? AND state='Failed'"), run),
              QStringLiteral("failure_projection_matches_committed_ledger"));
        check(view.unknownCount == db.scalar(QStringLiteral("SELECT count(*) FROM attempts WHERE run_id=? AND state='Unknown'"), run),
              QStringLiteral("unknown_projection_matches_committed_attempts"));
        check(view.reservationCount == db.scalar(QStringLiteral("SELECT count(*) FROM attempts WHERE run_id=? AND reservation_held=1"), run),
              QStringLiteral("reservation_projection_matches_committed_attempts"));
        check(view.records.size() == db.scalar(QStringLiteral("SELECT count(*) FROM events WHERE run_id=?"), run),
              QStringLiteral("record_count_matches_committed_events"));
    }
    qint64 lastSeq = -1;
    for (const auto& row : view.records) {
        check(row.runId == run && !row.source.isEmpty() && row.seq > lastSeq,
              QStringLiteral("record_provenance_and_monotonic_order_") + row.eventId);
        lastSeq = row.seq;
    }
    check(view.imageFileWriteCount == 0, QStringLiteral("full_replay_has_no_image_file_transport"));
    const QString csv = QDir(root).filePath(QStringLiteral("records.csv"));
    good(c.exportRecordsCsv(csv), c, QStringLiteral("committed_records_csv_export"));
    const auto output = bytes(csv);
    check(!output.isEmpty() && output.contains(run.toUtf8()), QStringLiteral("csv_contains_selected_run_history"));

    if (!good(c.setMode(QStringLiteral("Demo")), c, QStringLiteral("completed_replay_switches_to_demo"))) return;
    check(c.projection().selectedRunId.isEmpty() && c.projection().records.isEmpty() && c.projection().listings.isEmpty()
              && c.projection().confirmedSuccess == 0 && c.projection().unknownCount == 0
              && c.projection().reservationCount == 0 && c.runProfileDocument().isEmpty(),
          QStringLiteral("mode_change_clears_current_projection_not_history"));
    check(c.projection().runs.size() == view.runs.size() && c.projection().runs.first().id == run,
          QStringLiteral("mode_change_retains_committed_run_catalogue"));
    if (!good(c.selectRun(run), c, QStringLiteral("demo_can_inspect_existing_replay_history"))) return;
    check(c.projection().historyReadOnly && c.projection().confirmedSuccess == 1 && c.projection().records.size() == view.records.size(),
          QStringLiteral("mode_change_history_selection_restores_committed_facts"));
}

void testStorageFailures(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("storage_failures_open"))
        || !good(c.selectBuiltInFixture(), c, QStringLiteral("storage_failures_select"))
        || !good(c.start(), c, QStringLiteral("storage_failures_start"))
        || !good(c.step(), c, QStringLiteral("storage_failures_observe"))
        || !good(c.step(), c, QStringLiteral("storage_failures_dispatch"))) return;
    const QString database = c.projection().databasePath;
    const QString run = c.projection().selectedRunId;
    const auto baseline = businessState(c.projection());
    const auto baselineRecords = c.projection().records.size();
    {
        RawConnection blocker(database);
        if (!check(blocker.opened && blocker.exec(QStringLiteral("BEGIN IMMEDIATE")),
                   QStringLiteral("real_external_write_lock_acquired"))) return;
        check(!c.step() && !c.lastError().isEmpty(), QStringLiteral("busy_write_reports_storage_error"));
        check(businessState(c.projection()) == baseline, QStringLiteral("busy_keeps_old_committed_projection"));
        check(c.projection().confirmedSuccess == 0, QStringLiteral("busy_never_publishes_success"));
        check(blocker.exec(QStringLiteral("ROLLBACK")), QStringLiteral("external_write_lock_released"));
    }
    {
        RawConnection reader(database);
        if (!check(reader.opened && reader.exec(QStringLiteral("BEGIN"))
            && reader.scalar(QStringLiteral("SELECT count(*) FROM sqlite_master")) > 0,
                   QStringLiteral("real_external_read_lock_acquired"))) return;
        check(!c.step() && !c.lastError().isEmpty(), QStringLiteral("real_commit_failure_reports_error"));
        check(businessState(c.projection()) == baseline, QStringLiteral("failed_commit_keeps_old_projection"));
        check(c.projection().confirmedSuccess == 0 && c.projection().records.size() == baselineRecords,
              QStringLiteral("failed_commit_does_not_publish_partial_success_or_record"));
        check(reader.exec(QStringLiteral("ROLLBACK")), QStringLiteral("external_read_lock_released"));
    }
    if (!good(c.step(), c, QStringLiteral("same_step_retries_after_storage_rollback"))) return;
    check(c.projection().confirmedSuccess == 1 && c.projection().fixtureStep == 3,
          QStringLiteral("retry_commits_once_without_skipping_fixture_step"));
    RawConnection verify(database, true);
    check(verify.opened && verify.scalar(QStringLiteral("SELECT count(*) FROM ledger_transactions l JOIN attempts a ON a.attempt_id=l.attempt_id WHERE a.run_id=?"), run) == 1,
          QStringLiteral("retry_creates_exactly_one_terminal_ledger_record"));
}

void testReopenAndReadOnly(const QString& root)
{
    QString run;
    QString database;
    {
        WorkspaceController c;
        if (!good(c.open(root), c, QStringLiteral("recovery_open"))
            || !good(c.selectBuiltInFixture(), c, QStringLiteral("recovery_select"))
            || !good(c.start(), c, QStringLiteral("recovery_start"))
            || !good(c.step(), c, QStringLiteral("recovery_observe"))
            || !good(c.step(), c, QStringLiteral("recovery_dispatch"))) return;
        run = c.projection().selectedRunId;
        database = c.projection().databasePath;
        good(c.close(), c, QStringLiteral("close_committed_inflight_workspace"));
    }
    {
        WorkspaceController c;
        if (!good(c.open(root), c, QStringLiteral("workspace_reopen_recovers"))
            || !good(c.selectRun(run), c, QStringLiteral("select_recovered_history"))) return;
        const auto& v = c.projection();
        check(!v.active && !v.canResume && v.historyReadOnly, QStringLiteral("reopen_is_readonly_history_not_active_run"));
        check(v.unknownCount == 1 && v.reservationCount == 1 && v.confirmedSuccess == 0,
              QStringLiteral("reopen_preserves_unknown_and_quota"));
        const auto before = businessState(v);
        check(!c.resume() && !c.step(), QStringLiteral("history_cannot_resume_or_redispatch"));
        check(businessState(c.projection()) == before, QStringLiteral("recovery_actions_do_not_change_history"));
        good(c.close(), c, QStringLiteral("recovered_workspace_close"));
    }
    const auto originalHash = hash(database);
    {
        WorkspaceController reader;
        if (!good(reader.open(root, true), reader, QStringLiteral("readonly_workspace_open"))
            || !good(reader.selectRun(run), reader, QStringLiteral("readonly_history_selection"))) return;
        check(reader.projection().readOnly && !reader.projection().canStart, QStringLiteral("readonly_disables_start"));
        const auto before = businessState(reader.projection());
        const QStringList beforeFiles = diskFiles(QDir(root).filePath(QStringLiteral("profiles")));
        check(!reader.start(), QStringLiteral("readonly_start_rejected"));
        check(!reader.saveReviewedPreview(preview(), choices()), QStringLiteral("readonly_profile_write_rejected"));
        check(businessState(reader.projection()) == before, QStringLiteral("readonly_failures_preserve_projection"));
        check(diskFiles(QDir(root).filePath(QStringLiteral("profiles"))) == beforeFiles,
              QStringLiteral("readonly_save_produces_no_profile_file"));
        good(reader.close(), reader, QStringLiteral("readonly_workspace_close"));
    }
    check(hash(database) == originalHash, QStringLiteral("readonly_workspace_keeps_database_bytes"));
}

void testHistoricalProfileRevision(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("revision_history_open"))
        || !good(c.saveReviewedPreview(preview(), choices()), c, QStringLiteral("revision_history_save_v1"))) return;
    const auto profile = savedProfile(c.projection());
    if (!good(c.selectProfile(profile.id), c, QStringLiteral("revision_history_select_v1"))
        || !good(c.start(), c, QStringLiteral("revision_history_start_disabled_profile"))) return;
    const QString oldRun = c.projection().selectedRunId;
    const auto runDocument = c.runProfileDocument();
    check(c.projection().runProfileRevision == 1 && c.projection().runProfileId == profile.id,
          QStringLiteral("run_captures_profile_identity_and_revision"));
    if (!good(c.advance(), c, QStringLiteral("disabled_profile_evaluation_finishes"))) return;
    check(c.projection().confirmedSuccess == 0 && c.projection().reservationCount == 0,
          QStringLiteral("saved_disabled_profile_never_dispatches_or_reserves"));
    RawConnection oracle(c.projection().databasePath, true);
    check(oracle.opened && oracle.scalar(QStringLiteral("SELECT count(*) FROM attempts WHERE run_id=?"), oldRun) == 0,
          QStringLiteral("disabled_rules_have_no_attempt_rows"));
    if (!good(c.saveReviewedPreview(preview(), choices()), c, QStringLiteral("revision_history_save_v2"))) return;
    check(savedProfile(c.projection()).revision == 2, QStringLiteral("catalog_now_has_revision_two"));
    if (!good(c.selectBuiltInFixture(), c, QStringLiteral("revision_history_switch_to_fixture"))
        || !good(c.start(), c, QStringLiteral("revision_history_second_run"))
        || !good(c.advance(), c, QStringLiteral("revision_history_second_run_finishes"))
        || !good(c.selectRun(oldRun), c, QStringLiteral("revision_history_reselect_old_run"))) return;
    check(c.projection().runProfileRevision == 1 && c.projection().runProfileId == profile.id,
          QStringLiteral("history_retains_original_profile_revision"));
    check(c.runProfileDocument() == runDocument, QStringLiteral("history_retains_original_profile_document"));
    check(!c.projection().records.isEmpty(), QStringLiteral("old_run_records_remain_available"));
    for (const auto& row : c.projection().records)
        check(row.runId == oldRun && !row.source.isEmpty(), QStringLiteral("historical_record_source_") + row.eventId);
    if (!good(c.setMode(QStringLiteral("Demo")), c, QStringLiteral("historical_csv_demo_view_mode"))
        || !good(c.selectRun(oldRun), c, QStringLiteral("historical_csv_select_old_replay"))) return;
    const QString historyCsv = QDir(root).filePath(QStringLiteral("historical.csv"));
    if (!good(c.exportRecordsCsv(historyCsv), c, QStringLiteral("historical_csv_export"))) return;
    const auto exported = bytes(historyCsv);
    const auto expectedIdentity = (QStringLiteral("\"Replay\",\"") + profile.id + QStringLiteral("\",\"1\"")).toUtf8();
    check(exported.contains(expectedIdentity) && !exported.contains("\"Demo\""),
          QStringLiteral("csv_uses_historical_run_mode_and_revision_not_current_selection"));
    if (!good(c.close(), c, QStringLiteral("revision_history_close"))
        || !good(c.open(root), c, QStringLiteral("revision_history_reopen"))
        || !good(c.selectRun(oldRun), c, QStringLiteral("revision_history_reopen_select"))) return;
    check(c.runProfileDocument() == runDocument && c.projection().runProfileRevision == 1,
          QStringLiteral("profile_revision_snapshot_survives_process_lifetime"));
}

QString decodedCell(QString encoded)
{
    if (encoded.startsWith(QLatin1Char('"')) && encoded.endsWith(QLatin1Char('"'))) {
        encoded = encoded.mid(1, encoded.size() - 2);
        encoded.replace(QStringLiteral("\"\""), QStringLiteral("\""));
    }
    return encoded;
}

void testQuantumAndSourceValidation(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("validation_open"))
        || !good(c.saveReviewedPreview(preview(), choices()), c, QStringLiteral("validation_seed_profile"))) return;
    const auto profile = savedProfile(c.projection());
    if (!good(c.selectProfile(profile.id), c, QStringLiteral("validation_select_profile"))) return;
    c.setReviewDirty(true);
    const auto original = bytes(profile.path);
    const auto before = businessState(c.projection());
    const QStringList invalidQuantum{
        QString(),QStringLiteral(" "),QStringLiteral("0"),QStringLiteral("0.000"),
        QStringLiteral("-1"),QStringLiteral("+1"),QStringLiteral("1e-2"),QStringLiteral("0,01"),
        QStringLiteral("1..2"),QStringLiteral("00.1"),QStringLiteral(".1"),QStringLiteral("1."),
        QStringLiteral("0.0000000000001"),QStringLiteral("1234567890123456789012345"),QString(QChar(0x0661))};
    for (int i = 0; i < invalidQuantum.size(); ++i) {
        auto review = choices(); review.quantum = invalidQuantum.at(i);
        check(!c.saveReviewedPreview(preview(),review) && c.lastError().startsWith(QStringLiteral("QUANTUM_INVALID")),
              QStringLiteral("invalid_quantum_rejected_%1").arg(i));
        check(bytes(profile.path) == original && businessState(c.projection()) == before,
              QStringLiteral("invalid_quantum_preserves_profile_and_dirty_draft_%1").arg(i));
    }
    c.discardReviewDraft();
    const auto validState = businessState(c.projection());
    const auto document = QJsonDocument::fromJson(original).object();
    QVector<QPair<QString,QJsonObject>> invalidDocuments;
    const auto badSource = [&](const QString& name, const QString& key, const QJsonValue& value) {
        auto changed = document; auto source = changed.value(QStringLiteral("source")).toObject();
        source.insert(key,value); changed.insert(QStringLiteral("source"),source);
        invalidDocuments.push_back({name,changed});
    };
    badSource(QStringLiteral("missing_hash"),QStringLiteral("sha256"),QString());
    badSource(QStringLiteral("invalid_hash"),QStringLiteral("sha256"),QString(64,QLatin1Char('g')));
    badSource(QStringLiteral("identity_mismatch"),QStringLiteral("sha256"),QString(64,QLatin1Char('a')));
    badSource(QStringLiteral("missing_format"),QStringLiteral("format"),QJsonValue());
    badSource(QStringLiteral("unsupported_format"),QStringLiteral("format"),QStringLiteral("remote_data"));
    badSource(QStringLiteral("missing_encoding"),QStringLiteral("encoding"),QJsonValue());
    badSource(QStringLiteral("unsupported_encoding"),QStringLiteral("encoding"),QStringLiteral("unknown"));
    auto changed = document; changed.remove(QStringLiteral("source"));
    invalidDocuments.push_back({QStringLiteral("missing_source"),changed});
    changed = document; changed.insert(QStringLiteral("rules"),QJsonObject{});
    invalidDocuments.push_back({QStringLiteral("invalid_rules_type"),changed});
    changed = document;
    auto extensions = changed.value(QStringLiteral("extensions")).toObject();
    extensions.insert(QStringLiteral("x-live_market_data"),true); changed.insert(QStringLiteral("extensions"),extensions);
    invalidDocuments.push_back({QStringLiteral("live_source_claim"),changed});
    changed = document; extensions = changed.value(QStringLiteral("extensions")).toObject();
    extensions.insert(QStringLiteral("x-activation_required"),false); changed.insert(QStringLiteral("extensions"),extensions);
    invalidDocuments.push_back({QStringLiteral("missing_activation_review"),changed});
    changed = document; auto decisions = changed.value(QStringLiteral("review_decisions")).toObject();
    decisions.insert(QStringLiteral("unit_quantum"),QStringLiteral("0")); changed.insert(QStringLiteral("review_decisions"),decisions);
    invalidDocuments.push_back({QStringLiteral("zero_saved_quantum"),changed});
    for (const auto& item : invalidDocuments) {
        const auto invalid = QJsonDocument(item.second).toJson();
        if (!check(writeBytes(profile.path,invalid),QStringLiteral("invalid_profile_fixture_") + item.first)) return;
        check(!c.refresh() && c.lastError().startsWith(QStringLiteral("PROFILE_INVALID")),
              QStringLiteral("invalid_saved_profile_refresh_rejected_") + item.first);
        check(!c.start(),QStringLiteral("invalid_saved_profile_cannot_start_") + item.first);
        check(!c.saveReviewedPreview(preview(),choices()),QStringLiteral("invalid_saved_profile_not_overwritten_") + item.first);
        check(bytes(profile.path) == invalid && businessState(c.projection()) == validState,
              QStringLiteral("invalid_saved_profile_preserves_bytes_and_projection_") + item.first);
        if (!check(writeBytes(profile.path,original),QStringLiteral("restore_valid_profile_fixture_") + item.first)) return;
    }
    for (const auto& field : {QStringLiteral("source_format"),QStringLiteral("encoding")}) {
        auto invalidPreview = preview(); invalidPreview.remove(field);
        check(!c.saveReviewedPreview(invalidPreview,choices(QStringLiteral("Invalid Source"))),
              QStringLiteral("invalid_preview_source_rejected_") + field);
        check(diskFiles(QDir(root).filePath(QStringLiteral("profiles"))).size() == 1 && bytes(profile.path) == original,
              QStringLiteral("invalid_preview_source_writes_no_file_") + field);
    }
    if (!good(c.refresh(), c, QStringLiteral("validation_restored_profile_refresh"))) return;
    // A profile removed after selection must not turn into an empty Start snapshot.
    if (!check(QFile::remove(profile.path),QStringLiteral("selected_profile_removed_fixture"))) return;
    check(!c.start() && c.lastError().startsWith(QStringLiteral("PROFILE_NOT_FOUND")),
          QStringLiteral("deleted_selected_profile_cannot_start_empty_snapshot"));
    check(!c.projection().canStart && c.projection().runs.isEmpty(),
          QStringLiteral("deleted_profile_disables_start_without_creating_run"));
    check(writeBytes(profile.path,original),QStringLiteral("removed_selected_profile_restored"));
    good(c.refresh(), c, QStringLiteral("restored_selected_profile_refresh"));
    check(c.projection().canStart,QStringLiteral("restored_selected_profile_start_available"));
}

void testUncommittedReceiptProjection(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("receipt_projection_open"))
        || !good(c.selectBuiltInFixture(), c, QStringLiteral("receipt_projection_select"))
        || !good(c.start(), c, QStringLiteral("receipt_projection_start"))
        || !good(c.advance(), c, QStringLiteral("receipt_projection_complete_fixture"))) return;
    const QString run = c.projection().selectedRunId;
    const int beforeRecords = c.projection().records.size();
    RawConnection db(c.projection().databasePath);
    if (!check(db.opened,QStringLiteral("receipt_projection_raw_fixture_connection"))) return;
    const QString successAttempt = run + QStringLiteral(":attempt-A"), failedAttempt = run + QStringLiteral(":attempt-F");
    const QVector<QPair<QString,QString>> invalidReferences{
        {QString(),QString()},{successAttempt,QString()},
        {QString(),successAttempt+QStringLiteral(":transaction")},
        {QStringLiteral("nonexistent-attempt"),successAttempt+QStringLiteral(":transaction")},
        {successAttempt,QStringLiteral("nonexistent-transaction")},
        {failedAttempt,failedAttempt+QStringLiteral(":transaction")},
        {successAttempt,successAttempt+QStringLiteral(":transaction")}};
    qint64 seq = db.scalar(QStringLiteral("SELECT max(seq) FROM events WHERE run_id=?"),run);
    for (int i = 0; i < invalidReferences.size(); ++i) {
        const auto& ids = invalidReferences.at(i);
        QJsonObject payload{{QStringLiteral("source"),QStringLiteral("synthetic_replay")},
            {QStringLiteral("listing_id"),QStringLiteral("listing-F")},{QStringLiteral("observation_id"),QStringLiteral("observation-F")},
            {QStringLiteral("confirmed_price"),QStringLiteral("9999")},{QStringLiteral("attempt_id"),ids.first},
            {QStringLiteral("transaction_id"),ids.second},{QStringLiteral("reason"),QStringLiteral("INVALID_RECEIPT_FIXTURE")}};
        QSqlQuery insert(db.database);
        insert.prepare(QStringLiteral("INSERT INTO events(event_id,run_id,session_id,clock_domain_id,step_id,seq,at_mono_ms,cancel_epoch,viewport_generation,type,payload_json) "
                                      "SELECT ?,run_id,session_id,clock_domain_id,step_id,?,at_mono_ms,cancel_epoch,viewport_generation,?,? "
                                      "FROM events WHERE run_id=? ORDER BY seq LIMIT 1"));
        insert.addBindValue(QStringLiteral("forged-receipt-%1").arg(i)); insert.addBindValue(++seq);
        insert.addBindValue(i == invalidReferences.size()-1 ? QStringLiteral("ReceiptFailed") : QStringLiteral("ReceiptConfirmed"));
        insert.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))); insert.addBindValue(run);
        if (!check(insert.exec(),QStringLiteral("receipt_shape_without_success_ledger_insert_%1").arg(i))) return;
    }
    if (!good(c.refresh(), c, QStringLiteral("receipt_projection_refresh"))) return;
    check(c.projection().confirmedSuccess == 1 && c.projection().records.size() == beforeRecords+invalidReferences.size(),
          QStringLiteral("receipt_only_events_do_not_increment_committed_success"));
    int forgedRecords = 0;
    for (const auto& record : c.projection().records) {
        if (!record.eventId.startsWith(QStringLiteral("forged-receipt-"))) continue;
        ++forgedRecords;
        check(record.confirmedPrice.isEmpty(),QStringLiteral("unconfirmed_receipt_record_has_no_confirmed_price_")+record.eventId);
    }
    check(forgedRecords == invalidReferences.size(),QStringLiteral("all_forged_receipts_remain_auditable"));
    int confirmedListings = 0;
    for (const auto& listing : c.projection().listings) {
        if (!listing.confirmedPrice.isEmpty()) ++confirmedListings;
        if (listing.listingId == QStringLiteral("listing-F"))
            check(listing.confirmedPrice.isEmpty(),QStringLiteral("unconfirmed_event_never_sets_listing_confirmed_price"));
    }
    check(confirmedListings == 1,QStringLiteral("legitimate_committed_receipt_price_retained"));
    const QString csv = QDir(root).filePath(QStringLiteral("receipt-records.csv"));
    if (!good(c.exportRecordsCsv(csv), c, QStringLiteral("receipt_projection_csv"))) return;
    // UUIDs may legitimately contain the digit sequence 9999. Match a whole
    // quoted CSV cell, not a substring of unrelated event/run identifiers.
    check(!bytes(csv).contains(",\"9999\",") && bytes(csv).contains("\"148\""),
          QStringLiteral("csv_exposes_only_ledger_confirmed_price"));
}

void testExportStorageProtection(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root), c, QStringLiteral("export_protection_open"))
        || !good(c.saveReviewedPreview(preview(),choices()), c, QStringLiteral("export_protection_seed_profile"))
        || !good(c.selectBuiltInFixture(), c, QStringLiteral("export_protection_select"))
        || !good(c.start(), c, QStringLiteral("export_protection_start"))
        || !good(c.advance(), c, QStringLiteral("export_protection_finish"))) return;
    const auto profile = savedProfile(c.projection());
    const QString database = c.projection().databasePath;
    const QByteArray originalDatabase = hash(database), originalProfile = bytes(profile.path);
    const QByteArray before = businessState(c.projection());
    QStringList targets{database,profile.path,QDir(root).filePath(QStringLiteral("profiles/new-profile.csv")),
        QDir(root).filePath(QStringLiteral("profiles/../workspace.sqlite"))};
    for (const auto& suffix : {QStringLiteral("-wal"),QStringLiteral("-shm"),QStringLiteral("-journal"),QStringLiteral(".writer.lock")})
        targets.push_back(database+suffix);
#ifdef Q_OS_WIN
    targets.push_back(database.toUpper());
#endif
    for (int i=0; i<targets.size(); ++i) {
        check(!c.exportRecordsCsv(targets.at(i)) && c.lastError().startsWith(QStringLiteral("CSV_TARGET_PROTECTED")),
              QStringLiteral("csv_protected_destination_rejected_%1").arg(i));
        check(hash(database) == originalDatabase && bytes(profile.path) == originalProfile && businessState(c.projection()) == before,
              QStringLiteral("csv_protected_destination_preserves_data_%1").arg(i));
    }
    RawConnection oracle(database,true);
    check(oracle.opened && oracle.scalar(QStringLiteral("SELECT count(*) FROM runs")) == 1,
          QStringLiteral("database_remains_readable_after_rejected_csv_overwrite"));
    check(diskFiles(QDir(root).filePath(QStringLiteral("profiles"))).size() == 1,
          QStringLiteral("csv_does_not_create_files_in_profile_catalogue"));
    good(c.exportRecordsCsv(QDir(root).filePath(QStringLiteral("ordinary-export.csv"))), c,
         QStringLiteral("ordinary_record_export_still_succeeds"));
}

void testSummaryIsolation(const QString& root)
{
    WorkspaceController c;
    if (!good(c.open(root),c,QStringLiteral("summary_open"))
        || !good(c.selectBuiltInFixture(),c,QStringLiteral("summary_select_fixture"))
        || !good(c.start(),c,QStringLiteral("summary_first_start"))
        || !good(c.advance(),c,QStringLiteral("summary_first_complete"))) return;
    const QString older = c.projection().selectedRunId;
    if (!good(c.start(),c,QStringLiteral("summary_second_start"))
        || !good(c.advance(),c,QStringLiteral("summary_second_complete"))) return;
    const QString selected = c.projection().selectedRunId;
    check(c.projection().runs.size()==2,QStringLiteral("summary_contains_two_runs"));
    for (const auto& row : c.projection().runs) {
        check(row.profileId==WorkspaceController::builtinProfileId() && row.profileRevision==1
                  && !row.profileName.isEmpty() && row.mode==QStringLiteral("Replay")
                  && row.source==QStringLiteral("synthetic_replay") && row.state==QStringLiteral("Completed"),
              QStringLiteral("summary_preserves_metadata_without_detail_")+row.id);
        check(row.confirmedSuccess==1 && row.unknownCount==1 && row.reservationCount==1,
              QStringLiteral("summary_preserves_ledger_counts_without_detail_")+row.id);
    }
    const auto before=businessState(c.projection());
    const QString database=c.projection().databasePath;
    RawConnection raw(database);
    if (!check(raw.opened,QStringLiteral("summary_raw_connection"))) return;
    QString eventId,payload;
    {
        QSqlQuery q(raw.database);
        q.prepare(QStringLiteral("SELECT event_id,payload_json FROM events WHERE run_id=? AND type='Observation' ORDER BY seq LIMIT 1"));
        q.addBindValue(older);
        if (!check(q.exec() && q.next(),QStringLiteral("summary_unselected_observation_located"))) return;
        eventId=q.value(0).toString(); payload=q.value(1).toString();
    }
    {
        QSqlQuery q(raw.database);
        q.prepare(QStringLiteral("UPDATE events SET payload_json='not-json' WHERE event_id=?")); q.addBindValue(eventId);
        if (!check(q.exec(),QStringLiteral("summary_corrupt_unselected_detail"))) return;
    }
    check(c.refresh() && c.projection().selectedRunId==selected && businessState(c.projection())==before,
          QStringLiteral("summary_refresh_does_not_parse_unselected_event_detail"));
    check(!c.selectRun(older) && c.lastError().contains(QStringLiteral("EVENT_PAYLOAD_INVALID"))
              && c.projection().selectedRunId==selected && businessState(c.projection())==before,
          QStringLiteral("selected_corrupt_detail_rejected_without_projection_mutation"));
    {
        QSqlQuery q(raw.database);
        q.prepare(QStringLiteral("UPDATE events SET payload_json=? WHERE event_id=?")); q.addBindValue(payload); q.addBindValue(eventId);
        check(q.exec(),QStringLiteral("summary_restore_unselected_detail"));
    }
    check(c.selectRun(older) && c.projection().confirmedSuccess==1,
          QStringLiteral("summary_restored_detail_selectable"));

    // A committed metadata-only query must use the same count semantics as
    // snapshot: success quantities, held-reservation count, not receipt events.
    check(c.close(),QStringLiteral("summary_controller_close"));
    relink::ledger::SqliteEventStore store;
    check(std::holds_alternative<relink::ledger::StoreVoid>(store.open(database)),QStringLiteral("summary_direct_store_open"));
    {
        QSqlQuery q(raw.database);
        q.prepare(QStringLiteral("UPDATE attempts SET quantity=3 WHERE run_id=? AND state='Success'")); q.addBindValue(older);
        check(q.exec(),QStringLiteral("summary_quantity_greater_than_one_fixture"));
    }
    {
        QSqlQuery q(raw.database);
        q.prepare(QStringLiteral("INSERT INTO events(event_id,run_id,session_id,clock_domain_id,step_id,seq,at_mono_ms,cancel_epoch,viewport_generation,type,payload_json) SELECT ?,run_id,session_id,clock_domain_id,step_id,(SELECT max(seq)+1 FROM events WHERE run_id=?),at_mono_ms,cancel_epoch,viewport_generation,'ReceiptConfirmed','{\"confirmed_price\":\"9999\",\"attempt_id\":\"forged\",\"transaction_id\":\"forged\"}' FROM events WHERE run_id=? ORDER BY seq DESC LIMIT 1"));
        q.addBindValue(QStringLiteral("summary-forged-receipt")); q.addBindValue(older); q.addBindValue(older);
        check(q.exec(),QStringLiteral("summary_forged_receipt_without_run_state"));
    }
    const auto summaryResult=store.runSummaries();
    const auto snapshotResult=store.snapshot(older);
    check(std::holds_alternative<QVector<relink::ledger::RunSummary>>(summaryResult)
              && std::holds_alternative<relink::ledger::LedgerSnapshot>(snapshotResult),
          QStringLiteral("summary_and_snapshot_read_succeed"));
    if (const auto* rows=std::get_if<QVector<relink::ledger::RunSummary>>(&summaryResult)) {
        if (const auto* snapshot=std::get_if<relink::ledger::LedgerSnapshot>(&snapshotResult))
            for (const auto& row : *rows) if (row.runId==older)
                check(row.confirmedSuccess==3 && row.confirmedSuccess==snapshot->confirmedSuccess
                          && row.state==QStringLiteral("Completed") && row.unknownCount==1
                          && row.reservationCount==snapshot->unresolvedReservations,
                      QStringLiteral("summary_quantity_and_reservation_semantics_match_snapshot"));
    }
    check(std::holds_alternative<relink::ledger::StoreVoid>(store.close()),QStringLiteral("summary_direct_store_close"));
}

void testServiceBoundaries(const QString& root)
{
    QDir().mkpath(QDir(root).filePath(QStringLiteral("profiles")));
    ProfileCatalog catalog;
    const auto scanned = catalog.scan(root, ProfileCatalog::builtinProfileId());
    check(scanned.ok && scanned.rows.size() == 1, QStringLiteral("profile_catalog_scans_builtin_only"));
    check(scanned.documents.contains(ProfileCatalog::builtinProfileId()), QStringLiteral("profile_catalog_publishes_builtin_document"));
    check(WorkspaceController::builtinProfileId() == ProfileCatalog::builtinProfileId(), QStringLiteral("controller_uses_profile_catalog_identity"));

    relink::ledger::SqliteEventStore store;
    relink::ledger::SqliteEventStore::OpenOptions options; options.busyTimeoutMs = 30;
    check(std::holds_alternative<relink::ledger::StoreVoid>(store.open(QDir(root).filePath(QStringLiteral("workspace.sqlite")), options)),
          QStringLiteral("history_service_store_open"));
    HistoryQueryService history;
    const auto summaries = history.loadSummaries(store);
    check(std::holds_alternative<QVector<relink::ledger::RunSummary>>(summaries)
              && std::get<QVector<relink::ledger::RunSummary>>(summaries).isEmpty(),
          QStringLiteral("history_summary_does_not_load_detail_for_empty_workspace"));
    check(std::holds_alternative<relink::ledger::StoreVoid>(store.close()),
          QStringLiteral("history_service_store_close"));
    store.close();
    check(RecordsExporter::csvCell(QStringLiteral("=SUM(A1)")) == QStringLiteral("\"'=SUM(A1)\""),
          QStringLiteral("records_exporter_preserves_formula_guard"));
}
void testCsvEscaping()
{
    const QStringList dangerous{
        QStringLiteral("=1+1"), QStringLiteral("+SUM(A1)"), QStringLiteral("-2+3"),
        QStringLiteral("@SUM(A1)"), QStringLiteral("\t=1+1"), QStringLiteral("\r=1+1"),
        QStringLiteral("\n=1+1"), QStringLiteral("  =1+1"), QStringLiteral("\tordinary"),
        QStringLiteral("\rordinary"), QStringLiteral("\nordinary")};
    for (int i = 0; i < dangerous.size(); ++i) {
        const QString encoded = WorkspaceController::csvCell(dangerous.at(i));
        check(decodedCell(encoded) == QLatin1Char('\'') + dangerous.at(i),
              QStringLiteral("csv_neutralizes_formula_and_control_prefix_%1").arg(i));
    }
    const QStringList normal{QStringLiteral("Delta"), QStringLiteral("普通文本"),
        QStringLiteral("a,b"), QStringLiteral("a\"b"), QStringLiteral("a\nb"), QStringLiteral("")};
    for (int i = 0; i < normal.size(); ++i) {
        const QString encoded = WorkspaceController::csvCell(normal.at(i));
        check(decodedCell(encoded) == normal.at(i), QStringLiteral("csv_preserves_normal_field_%1").arg(i));
        if (normal.at(i).contains(QLatin1Char(',')) || normal.at(i).contains(QLatin1Char('"'))
            || normal.at(i).contains(QLatin1Char('\n')))
            check(encoded.startsWith(QLatin1Char('"')) && encoded.endsWith(QLatin1Char('"')),
                  QStringLiteral("csv_quotes_separator_quote_or_newline_%1").arg(i));
    }
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    if (!check(root.isValid(), QStringLiteral("temporary_workspace_root"))) return 1;
    const auto path = [&](const QString& name) {
        const QString result = root.filePath(name);
        QDir().mkpath(result);
        return result;
    };
    testProfileReview(path(QStringLiteral("profiles")));
    testGuardsAndControls(path(QStringLiteral("controls")));
    testFullReplayAndCommittedCounts(path(QStringLiteral("replay")));
    testStorageFailures(path(QStringLiteral("faults")));
    testReopenAndReadOnly(path(QStringLiteral("recovery")));
    testHistoricalProfileRevision(path(QStringLiteral("history")));
    testQuantumAndSourceValidation(path(QStringLiteral("validation")));
    testUncommittedReceiptProjection(path(QStringLiteral("receipt_projection")));
    testExportStorageProtection(path(QStringLiteral("export_protection")));
    testCsvEscaping();
    testServiceBoundaries(path(QStringLiteral("services")));
    testSummaryIsolation(path(QStringLiteral("summary_isolation")));
    std::cout << "WORKSPACE_TESTS=" << (failures == 0 ? "PASS" : "FAIL")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; real_database=true; real_lock_failures=true; external_actions=0\n";
    return failures == 0 ? 0 : 1;
}
