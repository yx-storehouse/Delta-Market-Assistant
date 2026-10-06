#include "sqlite_event_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <optional>

namespace relink::ledger {
namespace {
using Error = std::optional<StoreError>;

StoreError err(const char* code, const QString& message, const QString& id = {})
{
    return {QString::fromLatin1(code), message, id};
}

// A null QString is bound as SQL NULL by Qt. Optional IDs in this API are
// represented by an empty string, not NULL; keep the two representations apart.
QString text(const QString& value) { return value.isNull() ? QStringLiteral("") : value; }

StoreError sqlError(const QSqlError& e, const QString& id = {})
{
    bool ok = false;
    const int primary = e.nativeErrorCode().section(QLatin1Char(' '), 0, 0).toInt(&ok) & 255;
    const char* code = "DB_ERROR";
    if (ok) {
        switch (primary) {
        case 5: case 6: code = "DB_BUSY"; break;
        case 8: code = "DB_READ_ONLY"; break;
        case 10: code = "DB_IO_ERROR"; break;
        case 11: case 26: code = "DB_CORRUPT"; break;
        case 13: code = "DB_FULL"; break;
        case 14: code = "DB_OPEN_FAILED"; break;
        case 19: code = "DB_CONSTRAINT"; break;
        default: break;
        }
    }
    return err(code, e.text(), id);
}

bool execute(QSqlQuery& query, const QString& sql, const QVariantList& values = {})
{
    if (!query.prepare(sql)) return false;
    for (const QVariant& value : values) query.addBindValue(value);
    return query.exec();
}

QJsonValue canonicalValue(const QJsonValue& value)
{
    if (value.isObject()) {
        QJsonObject output;
        auto keys = value.toObject().keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& key : keys) output.insert(key, canonicalValue(value.toObject().value(key)));
        return output;
    }
    if (value.isArray()) {
        QJsonArray output;
        for (const auto& item : value.toArray()) output.append(canonicalValue(item));
        return output;
    }
    return value;
}

QString canonical(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(canonicalValue(object).toObject()).toJson(QJsonDocument::Compact));
}

struct SchemaObject { const char* name; const char* ddl; };
const SchemaObject baseObjects[] = {
    {"schema_meta", "CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)"},
    {"runs", "CREATE TABLE runs (run_id TEXT PRIMARY KEY NOT NULL, session_id TEXT NOT NULL DEFAULT '')"},
    {"events", R"SQL(CREATE TABLE events (
        event_id TEXT PRIMARY KEY NOT NULL, run_id TEXT NOT NULL REFERENCES runs(run_id),
        session_id TEXT NOT NULL, clock_domain_id TEXT NOT NULL, step_id TEXT NOT NULL,
        seq INTEGER NOT NULL CHECK(seq>=0), at_mono_ms INTEGER NOT NULL CHECK(at_mono_ms>=0),
        cancel_epoch INTEGER NOT NULL CHECK(cancel_epoch>=0),
        viewport_generation INTEGER NOT NULL CHECK(viewport_generation>=0),
        type TEXT NOT NULL, payload_json TEXT NOT NULL))SQL"},
    {"attempts", R"SQL(CREATE TABLE attempts (
        attempt_id TEXT PRIMARY KEY NOT NULL, intent_id TEXT NOT NULL UNIQUE,
        run_id TEXT NOT NULL REFERENCES runs(run_id), rule_task_id TEXT NOT NULL,
        rule_revision INTEGER NOT NULL CHECK(rule_revision>0),
        kind TEXT NOT NULL CHECK(kind IN ('purchase','collection')),
        state TEXT NOT NULL CHECK(state IN ('Prepared','Dispatching','Sent','AwaitingReceipt','Unknown','Success','Failed','Cancelled')),
        quantity INTEGER NOT NULL CHECK(quantity>0),
        reservation_held INTEGER NOT NULL CHECK(reservation_held IN (0,1)),
        dispatch_proof TEXT NOT NULL CHECK(dispatch_proof IN ('not_dispatched','possibly_dispatched','acknowledged')),
        receipt_identity TEXT NOT NULL DEFAULT '', reservation_id TEXT NOT NULL UNIQUE,
        scope_id TEXT NOT NULL, quota_target INTEGER NOT NULL CHECK(quota_target>0),
        transaction_id TEXT NOT NULL DEFAULT '', dispatch_event_id TEXT NOT NULL DEFAULT ''))SQL"},
    {"quota_reservations", R"SQL(CREATE TABLE quota_reservations (
        reservation_id TEXT PRIMARY KEY NOT NULL,
        attempt_id TEXT NOT NULL UNIQUE REFERENCES attempts(attempt_id),
        scope_id TEXT NOT NULL, quantity INTEGER NOT NULL CHECK(quantity>0),
        state TEXT NOT NULL CHECK(state IN ('held','released','consumed','unresolved'))))SQL"},
    {"receipts", R"SQL(CREATE TABLE receipts (
        receipt_identity TEXT NOT NULL, attempt_id TEXT NOT NULL REFERENCES attempts(attempt_id),
        outcome TEXT NOT NULL CHECK(outcome IN ('success','failed','unknown')),
        association TEXT NOT NULL CHECK(association IN ('confirmed','ambiguous')),
        event_id TEXT NOT NULL, PRIMARY KEY(receipt_identity,attempt_id)))SQL"},
    {"ledger_transactions", R"SQL(CREATE TABLE ledger_transactions (
        transaction_id TEXT PRIMARY KEY NOT NULL,
        attempt_id TEXT NOT NULL UNIQUE REFERENCES attempts(attempt_id),
        terminal TEXT NOT NULL CHECK(terminal IN ('Success','Failed')),
        queue_empty INTEGER NOT NULL CHECK(queue_empty IN (0,1)),
        receipt_identity TEXT NOT NULL, event_id TEXT NOT NULL,
        FOREIGN KEY(receipt_identity,attempt_id) REFERENCES receipts(receipt_identity,attempt_id)))SQL"},
    {"events_run_seq", "CREATE UNIQUE INDEX events_run_seq ON events(run_id,seq) WHERE run_id<>''"},
    {"attempts_run_state", "CREATE INDEX attempts_run_state ON attempts(run_id,state)"},
    {"reservations_scope_state", "CREATE INDEX reservations_scope_state ON quota_reservations(scope_id,state)"}
};
const SchemaObject recoveryObjects[] = {
    {"recovery_audit", R"SQL(CREATE TABLE recovery_audit (
        recovery_id INTEGER PRIMARY KEY AUTOINCREMENT,
        attempt_id TEXT NOT NULL REFERENCES attempts(attempt_id),
        previous_state TEXT NOT NULL, recovered_state TEXT NOT NULL,
        previous_proof TEXT NOT NULL, reason TEXT NOT NULL))SQL"},
    {"recovery_attempt", "CREATE INDEX recovery_attempt ON recovery_audit(attempt_id,recovery_id)"}
};

QString normalizedSql(QString sql)
{
    sql.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (sql.endsWith(QLatin1Char(';'))) sql.chop(1);
    return sql;
}

Error validateObject(QSqlDatabase& database, const SchemaObject& object)
{
    QSqlQuery query(database);
    if (!execute(query, QStringLiteral("SELECT sql FROM sqlite_master WHERE name=?"), {QString::fromLatin1(object.name)}))
        return sqlError(query.lastError());
    if (!query.next() || normalizedSql(query.value(0).toString()) != normalizedSql(QString::fromLatin1(object.ddl)))
        return err("SCHEMA_INVALID", QStringLiteral("Required schema object differs: %1").arg(QString::fromLatin1(object.name)));
    return {};
}

Error inspectSchema(QSqlDatabase& database, int& version)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA user_version"))) return sqlError(query.lastError());
    if (!query.next()) return err("SCHEMA_INVALID", QStringLiteral("Missing schema version"));
    version = query.value(0).toInt();
    query.finish();
    if (version > SqliteEventStore::CurrentSchemaVersion)
        return err("SCHEMA_TOO_NEW", QStringLiteral("Database schema is newer than this application"));
    if (version < 0) return err("SCHEMA_INVALID", QStringLiteral("Negative schema version"));
    if (!query.exec(QStringLiteral("PRAGMA quick_check"))) return sqlError(query.lastError());
    if (!query.next() || query.value(0).toString() != QStringLiteral("ok"))
        return err("DB_CORRUPT", QStringLiteral("SQLite quick_check failed"));
    query.finish();
    if (version == 0) {
        if (!query.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")))
            return sqlError(query.lastError());
        if (!query.next() || query.value(0).toInt() != 0)
            return err("SCHEMA_UNRECOGNIZED", QStringLiteral("Unversioned nonempty database is not a ledger"));
        return {};
    }
    for (const auto& object : baseObjects) if (auto e = validateObject(database, object)) return e;
    if (version == 2)
        for (const auto& object : recoveryObjects) if (auto e = validateObject(database, object)) return e;
    if (!query.exec(QStringLiteral("SELECT value FROM schema_meta WHERE key='schema_name'"))) return sqlError(query.lastError());
    if (!query.next() || query.value(0).toString() != QStringLiteral("delta_market_assistant_ledger"))
        return err("SCHEMA_UNRECOGNIZED", QStringLiteral("Database does not identify as this ledger"));
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA foreign_key_check"))) return sqlError(query.lastError());
    if (query.next()) return err("DB_CORRUPT", QStringLiteral("Foreign-key consistency check failed"));
    query.finish();
    // SQLite structural integrity is necessary but not sufficient: the
    // denormalized projection fields must agree with their relational facts.
    if (!query.exec(QStringLiteral(
        "SELECT 1 FROM attempts a LEFT JOIN quota_reservations q ON q.reservation_id=a.reservation_id "
        "LEFT JOIN ledger_transactions l ON l.attempt_id=a.attempt_id "
        "LEFT JOIN receipts r ON r.attempt_id=a.attempt_id AND r.receipt_identity=a.receipt_identity "
        "WHERE q.reservation_id IS NULL OR q.attempt_id<>a.attempt_id OR q.scope_id<>a.scope_id OR q.quantity<>a.quantity "
        "OR (a.receipt_identity<>'' AND r.receipt_identity IS NULL) "
        "OR (a.state IN ('Success','Failed') AND (a.reservation_held<>0 OR l.transaction_id IS NULL "
        " OR l.transaction_id<>a.transaction_id OR l.terminal<>a.state OR l.receipt_identity<>a.receipt_identity "
        " OR r.association<>'confirmed' OR r.outcome<>CASE a.state WHEN 'Success' THEN 'success' ELSE 'failed' END "
        " OR q.state<>CASE a.state WHEN 'Success' THEN 'consumed' ELSE 'released' END)) "
        "OR (a.state='Cancelled' AND (a.reservation_held<>0 OR q.state<>'released' OR a.dispatch_proof<>'not_dispatched' "
        " OR a.transaction_id<>'' OR l.transaction_id IS NOT NULL)) "
        "OR (a.state NOT IN ('Success','Failed','Cancelled') AND (a.reservation_held<>1 "
        " OR q.state NOT IN ('held','unresolved') OR a.transaction_id<>'' OR l.transaction_id IS NOT NULL)) "
        "OR (a.state='Unknown' AND (q.state<>'unresolved' OR a.dispatch_proof='not_dispatched')) LIMIT 1")))
        return sqlError(query.lastError());
    if (query.next()) return err("DB_CORRUPT", QStringLiteral("Ledger, attempt, receipt and reservation facts disagree"));
    return {};
}

Error inspectRecoveryCopy(const QString& source, int busyTimeoutMs, int& version)
{
    // A spilled DELETE-journal transaction requires SQLite to roll back before
    // even a read-only PRAGMA can inspect the database. Validate the naturally
    // recovered state on independent copies first; never erase the journal or
    // overwrite the source to make a corrupt/foreign file appear valid.
    QFile journal(source + QStringLiteral("-journal"));
    if (!journal.open(QIODevice::ReadOnly) || journal.size() <= 512
        || journal.read(8) != QByteArray::fromHex("d9d505f920a163d7"))
        return err("DB_RECOVERY_REQUIRED", QStringLiteral("Database needs recovery but has no recognized rollback journal"), source);
    journal.close();
    QTemporaryDir temporary;
    if (!temporary.isValid()) return err("DB_IO_ERROR", QStringLiteral("Could not create an isolated recovery validation directory"), source);
    const QString copy = temporary.filePath(QStringLiteral("recovery.sqlite"));
    if (!QFile::copy(source, copy) || !QFile::copy(source + QStringLiteral("-journal"), copy + QStringLiteral("-journal")))
        return err("DB_IO_ERROR", QStringLiteral("Could not copy the database and journal for isolated validation"), source);
    const QString name = QStringLiteral("dma_recovery_probe_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Error failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(copy);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(busyTimeoutMs));
        if (!database.open()) failure = sqlError(database.lastError(), source);
        else failure = inspectSchema(database, version);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    if (!failure && version == 0)
        failure = err("SCHEMA_UNRECOGNIZED", QStringLiteral("Recovery copy does not identify an existing versioned ledger"), source);
    return failure;
}

const QString attemptColumns = QStringLiteral(
    "attempt_id,intent_id,run_id,rule_task_id,rule_revision,kind,state,quantity,"
    "reservation_held,dispatch_proof,receipt_identity,reservation_id,scope_id,quota_target,transaction_id");

AttemptRecord readAttempt(const QSqlQuery& query)
{
    AttemptRecord a;
    a.attemptId = query.value(0).toString(); a.intentId = query.value(1).toString();
    a.runId = query.value(2).toString(); a.ruleTaskId = query.value(3).toString();
    a.ruleRevision = query.value(4).toInt(); a.kind = query.value(5).toString();
    a.state = query.value(6).toString(); a.quantity = query.value(7).toInt();
    a.reservationHeld = query.value(8).toBool(); a.dispatchProof = query.value(9).toString();
    a.receiptIdentity = query.value(10).toString(); a.reservationId = query.value(11).toString();
    a.scopeId = query.value(12).toString(); a.quotaTarget = query.value(13).toInt();
    a.transactionId = query.value(14).toString();
    return a;
}

LedgerTransactionRecord readTransaction(const QSqlQuery& query)
{
    return {query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toBool(), query.value(4).toString()};
}

bool sameAttempt(const AttemptRecord& a, const AttemptDraft& b)
{
    return a.attemptId == b.attemptId && a.intentId == b.intentId && a.runId == b.runId
        && a.ruleTaskId == b.ruleTaskId && a.ruleRevision == b.ruleRevision && a.kind == b.kind
        && a.quantity == b.quantity && a.reservationId == b.reservationId
        && a.scopeId == b.scopeId && a.quotaTarget == b.quotaTarget;
}

bool terminal(const QString& state)
{
    return state == QStringLiteral("Success") || state == QStringLiteral("Failed") || state == QStringLiteral("Cancelled");
}
} // namespace

class SqliteEventStore::Impl {
public:
    QThread* const thread = QThread::currentThread();
    QSqlDatabase writer;
    mutable QSqlDatabase reader;
    QString writerName, readerName, path;
    std::unique_ptr<QLockFile> lock;
    OpenOptions options;
    bool opened = false, transaction = false, aborted = false;
    int version = 0;

    Error ready(bool writing = false) const
    {
        if (QThread::currentThread() != thread) return err("WRONG_THREAD", QStringLiteral("Store connection belongs to another thread"));
        if (!opened) return err("STORE_CLOSED", QStringLiteral("Store is not open"));
        if (writing && aborted) return err("TRANSACTION_ABORTED", QStringLiteral("Call rollback before beginning more work"));
        if (writing && options.readOnly) return err("DB_READ_ONLY", QStringLiteral("Store was opened read-only"));
        return {};
    }

    StoreError fail(StoreError error)
    {
        if (transaction) {
            QSqlQuery rollbackQuery(writer);
            if (!rollbackQuery.exec(QStringLiteral("ROLLBACK"))) {
                const auto rollbackError = rollbackQuery.lastError();
                // SQLITE_FULL/IOERR may already have automatically rolled
                // back the whole SQLite transaction. ROLLBACK then reports
                // SQLITE_ERROR (1), "no transaction is active"; this is a
                // completed rollback, not a reason to discard the connection.
                const bool alreadyRolledBack = rollbackError.nativeErrorCode() == QStringLiteral("1")
                    && rollbackError.databaseText().contains(QStringLiteral("no transaction is active"), Qt::CaseInsensitive);
                if (!alreadyRolledBack) {
                    // Closing SQLite is the last-resort rollback if the driver
                    // cannot end the transaction; no subsequent commit is allowed.
                    writer.close();
                    opened = false;
                    error.message += QStringLiteral("; rollback failed, connection closed: ") + rollbackError.text();
                }
            }
            transaction = false;
        }
        aborted = true;
        return error;
    }

    Error begin()
    {
        if (auto e = ready(true)) {
            if (QThread::currentThread() == thread && opened) return fail(*e);
            return e;
        }
        if (transaction) return {};
        QSqlQuery query(writer);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return fail(sqlError(query.lastError()));
        transaction = true;
        return {};
    }

    Error findAttempt(const QString& id, AttemptRecord& attempt, bool& found)
    {
        QSqlQuery query(writer);
        if (!execute(query, QStringLiteral("SELECT ") + attemptColumns + QStringLiteral(" FROM attempts WHERE attempt_id=?"), {id}))
            return fail(sqlError(query.lastError(), id));
        found = query.next();
        if (found) attempt = readAttempt(query);
        return {};
    }

    Error run(const QString& id, const QString& session)
    {
        QSqlQuery query(writer);
        if (!execute(query, QStringLiteral("SELECT session_id FROM runs WHERE run_id=?"), {text(id)}))
            return fail(sqlError(query.lastError(), id));
        if (query.next()) {
            const QString old = query.value(0).toString();
            query.finish();
            if (!old.isEmpty() && !session.isEmpty() && old != session)
                return fail(err("RUN_SESSION_CONFLICT", QStringLiteral("Run already belongs to another session"), id));
            if (old.isEmpty() && !session.isEmpty()
                && !execute(query, QStringLiteral("UPDATE runs SET session_id=? WHERE run_id=?"), {text(session), text(id)}))
                return fail(sqlError(query.lastError(), id));
        } else if (!execute(query, QStringLiteral("INSERT INTO runs(run_id,session_id) VALUES(?,?)"), {text(id), text(session)})) {
            return fail(sqlError(query.lastError(), id));
        }
        return {};
    }

    Error persistRecovery()
    {
        QSqlQuery query(writer);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return sqlError(query.lastError());
        transaction = true;
        // Already-recovered Unknown rows remain untouched and do not create
        // repeated audit entries on each open.
        if (!query.exec(QStringLiteral(
            "SELECT ") + attemptColumns + QStringLiteral(
            " FROM attempts WHERE reservation_held=1 AND state NOT IN ('Unknown','Success','Failed','Cancelled') ORDER BY attempt_id")))
            return fail(sqlError(query.lastError()));
        QVector<AttemptRecord> recover;
        while (query.next()) recover.push_back(readAttempt(query));
        query.finish();
        for (const auto& a : recover) {
            const bool cancelled = a.state == QStringLiteral("Prepared") && a.dispatchProof == QStringLiteral("not_dispatched");
            const QString state = cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Unknown");
            const QString proof = cancelled ? a.dispatchProof
                : (a.dispatchProof == QStringLiteral("acknowledged") ? a.dispatchProof : QStringLiteral("possibly_dispatched"));
            if (!execute(query, QStringLiteral("INSERT INTO recovery_audit(attempt_id,previous_state,recovered_state,previous_proof,reason) VALUES(?,?,?,?,?)"),
                         {a.attemptId, a.state, state, a.dispatchProof, cancelled ? QStringLiteral("prepared_not_dispatched") : QStringLiteral("possible_external_dispatch")}))
                return fail(sqlError(query.lastError(), a.attemptId));
            if (!execute(query, QStringLiteral("UPDATE attempts SET state=?,reservation_held=?,dispatch_proof=? WHERE attempt_id=?"),
                         {state, cancelled ? 0 : 1, proof, a.attemptId})) return fail(sqlError(query.lastError(), a.attemptId));
            if (!execute(query, QStringLiteral("UPDATE quota_reservations SET state=? WHERE reservation_id=?"),
                         {cancelled ? QStringLiteral("released") : QStringLiteral("unresolved"), a.reservationId}))
                return fail(sqlError(query.lastError(), a.attemptId));
        }
        if (!query.exec(QStringLiteral("COMMIT"))) return fail(sqlError(query.lastError()));
        transaction = false;
        return {};
    }

    void cleanup()
    {
        if (transaction && writer.isOpen()) writer.rollback();
        transaction = false;
        reader.close(); writer.close();
        reader = QSqlDatabase(); writer = QSqlDatabase();
        if (!readerName.isEmpty()) QSqlDatabase::removeDatabase(readerName);
        if (!writerName.isEmpty()) QSqlDatabase::removeDatabase(writerName);
        readerName.clear(); writerName.clear();
        lock.reset(); path.clear(); opened = false; aborted = false; version = 0;
    }
};

SqliteEventStore::SqliteEventStore() : d(std::make_unique<Impl>()) {}
SqliteEventStore::~SqliteEventStore() { d->cleanup(); }
bool SqliteEventStore::isOpen() const { return QThread::currentThread() == d->thread && d->opened; }
int SqliteEventStore::schemaVersion() const { return isOpen() ? d->version : 0; }

VoidResult SqliteEventStore::open(const QString& path) { return open(path, OpenOptions{}); }

VoidResult SqliteEventStore::open(const QString& path, const OpenOptions& options)
{
    if (QThread::currentThread() != d->thread) return err("WRONG_THREAD", QStringLiteral("Open must use the construction thread"));
    if (d->opened) return err("STORE_ALREADY_OPEN", QStringLiteral("Close the existing store before opening another"));
    d->cleanup();
    if (path.trimmed().isEmpty() || path == QStringLiteral(":memory:") || options.busyTimeoutMs < 0)
        return err("INVALID_PATH", QStringLiteral("A disk database path and non-negative timeout are required"));
    QFileInfo info(path);
    if (info.isDir() || !QFileInfo(info.absolutePath()).isDir())
        return err("INVALID_PATH", QStringLiteral("Database parent directory must exist"), path);
    if (options.readOnly && !info.exists()) return err("DB_NOT_FOUND", QStringLiteral("Read-only database does not exist"), path);
    // Resolve relative paths and symlink/junction aliases before choosing the
    // sidecar lock key. A hard-link alias or an external non-cooperating SQL
    // writer is outside this single-writer ownership protocol.
    d->path = info.exists() ? info.canonicalFilePath()
                            : QDir(QFileInfo(info.absolutePath()).canonicalFilePath()).filePath(info.fileName());
    if (d->path.isEmpty()) return err("INVALID_PATH", QStringLiteral("Database path could not be resolved"), path);
    d->options = options;
    auto failure = [&](const StoreError& error) -> VoidResult { d->cleanup(); return error; };
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return failure(err("DRIVER_UNAVAILABLE", QStringLiteral("QSQLITE driver is not available")));
    if (!options.readOnly) {
        d->lock = std::make_unique<QLockFile>(d->path + QStringLiteral(".writer.lock"));
        d->lock->setStaleLockTime(0);
        if (!d->lock->tryLock(0))
            return failure(err(d->lock->error() == QLockFile::LockFailedError ? "STORE_IN_USE" : "DB_OPEN_FAILED",
                               QStringLiteral("The ledger writer lock could not be acquired"), d->path));
    }
    d->writerName = QStringLiteral("dma_writer_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), d->writerName);
    d->writer.setDatabaseName(d->path);
    const QString timeout = QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(options.busyTimeoutMs);
    // Inspect existing files through a read-only connection before any schema,
    // journal or recovery write. Unknown/new/corrupt files are never rebuilt.
    d->writer.setConnectOptions((info.exists() || options.readOnly) ? timeout + QStringLiteral(";QSQLITE_OPEN_READONLY") : timeout);
    if (!d->writer.open()) return failure(sqlError(d->writer.lastError(), d->path));
    if (auto e = inspectSchema(d->writer, d->version)) {
        if (options.readOnly || e->code != QStringLiteral("DB_READ_ONLY")) return failure(*e);
        if (auto recoveryError = inspectRecoveryCopy(d->path, options.busyTimeoutMs, d->version)) return failure(*recoveryError);
    }
    if (options.readOnly && d->version != CurrentSchemaVersion)
        return failure(err("SCHEMA_MIGRATION_REQUIRED", QStringLiteral("A writable open is required to migrate this database")));
    if (!options.readOnly && info.exists()) {
        d->writer.close(); d->writer.setConnectOptions(timeout);
        if (!d->writer.open()) return failure(sqlError(d->writer.lastError(), d->path));
        if (auto e = inspectSchema(d->writer, d->version)) return failure(*e);
    }
    Error setupError;
    {
        QSqlQuery query(d->writer);
        if (!query.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) setupError = sqlError(query.lastError());
        if (!setupError && !options.readOnly && !query.exec(QStringLiteral("PRAGMA synchronous=FULL"))) setupError = sqlError(query.lastError());
        if (!setupError && !options.readOnly && d->version < CurrentSchemaVersion) {
            if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) setupError = sqlError(query.lastError());
            else {
                d->transaction = true;
                if (d->version == 0) {
                    for (const auto& object : baseObjects) {
                        if (!query.exec(QString::fromLatin1(object.ddl))) { setupError = sqlError(query.lastError()); break; }
                    }
                    if (!setupError && !query.exec(QStringLiteral("INSERT INTO schema_meta(key,value) VALUES('schema_name','delta_market_assistant_ledger')")))
                        setupError = sqlError(query.lastError());
                    if (!setupError && !query.exec(QStringLiteral("PRAGMA user_version=1"))) setupError = sqlError(query.lastError());
                }
                if (!setupError) {
                    for (const auto& object : recoveryObjects) {
                        if (!query.exec(QString::fromLatin1(object.ddl))) { setupError = sqlError(query.lastError()); break; }
                    }
                }
                if (!setupError && !query.exec(QStringLiteral("PRAGMA user_version=2"))) setupError = sqlError(query.lastError());
                if (!setupError && !query.exec(QStringLiteral("COMMIT"))) setupError = sqlError(query.lastError());
                if (setupError) d->writer.rollback();
                d->transaction = false;
            }
        }
    }
    if (setupError) return failure(*setupError);
    if (auto e = inspectSchema(d->writer, d->version)) return failure(*e);
    if (!options.readOnly && options.recoverOnOpen)
        if (auto e = d->persistRecovery()) return failure(*e);

    d->readerName = QStringLiteral("dma_reader_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->reader = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), d->readerName);
    d->reader.setDatabaseName(d->path);
    d->reader.setConnectOptions(timeout + QStringLiteral(";QSQLITE_OPEN_READONLY"));
    if (!d->reader.open()) return failure(sqlError(d->reader.lastError(), d->path));
    d->opened = true;
    return StoreVoid{};
}

VoidResult SqliteEventStore::close()
{
    if (QThread::currentThread() != d->thread) return err("WRONG_THREAD", QStringLiteral("Close must use the construction thread"));
    d->cleanup();
    return StoreVoid{};
}

StoreResult<AppendReceipt> SqliteEventStore::appendEvent(const EventDraft& draft)
{
    if (auto e = d->begin()) return *e;
    if (draft.eventId.isEmpty() || draft.seq < 0 || draft.atMonoMs < 0 || draft.cancelEpoch < 0 || draft.viewportGeneration < 0)
        return d->fail(err("INVALID_EVENT", QStringLiteral("Event ID and non-negative sequence/clock fields are required"), draft.eventId));
    const QString payload = canonical(draft.payload);
    QSqlQuery query(d->writer);
    if (!execute(query, QStringLiteral("SELECT run_id,session_id,clock_domain_id,step_id,seq,at_mono_ms,cancel_epoch,viewport_generation,type,payload_json FROM events WHERE event_id=?"), {draft.eventId}))
        return d->fail(sqlError(query.lastError(), draft.eventId));
    if (query.next()) {
        const bool same = query.value(0).toString() == draft.runId && query.value(1).toString() == draft.sessionId
            && query.value(2).toString() == draft.clockDomainId && query.value(3).toString() == draft.stepId
            && query.value(4).toLongLong() == draft.seq && query.value(5).toLongLong() == draft.atMonoMs
            && query.value(6).toLongLong() == draft.cancelEpoch && query.value(7).toLongLong() == draft.viewportGeneration
            && query.value(8).toString() == draft.type && query.value(9).toString() == payload;
        if (!same) return d->fail(err("EVENT_ID_CONFLICT", QStringLiteral("Event identity or payload differs"), draft.eventId));
        return AppendReceipt{draft.eventId, QStringLiteral("event:") + draft.eventId, true};
    }
    if (!draft.runId.isEmpty()) {
        if (!execute(query, QStringLiteral("SELECT 1 FROM events WHERE run_id=? AND seq=?"), {draft.runId, draft.seq}))
            return d->fail(sqlError(query.lastError(), draft.eventId));
        if (query.next()) return d->fail(err("EVENT_SEQ_CONFLICT", QStringLiteral("Run sequence already exists"), draft.eventId));
        query.finish();
    }
    if (auto e = d->run(draft.runId, draft.sessionId)) return *e;
    if (!execute(query, QStringLiteral("INSERT INTO events(event_id,run_id,session_id,clock_domain_id,step_id,seq,at_mono_ms,cancel_epoch,viewport_generation,type,payload_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
                 {draft.eventId, text(draft.runId), text(draft.sessionId), text(draft.clockDomainId), text(draft.stepId), draft.seq,
                  draft.atMonoMs, draft.cancelEpoch, draft.viewportGeneration, text(draft.type), payload}))
        return d->fail(sqlError(query.lastError(), draft.eventId));
    return AppendReceipt{draft.eventId, QStringLiteral("event:") + draft.eventId, false};
}

StoreResult<AttemptRecord> SqliteEventStore::reserveAttempt(const AttemptDraft& draft)
{
    if (auto e = d->begin()) return *e;
    if (draft.attemptId.isEmpty() || draft.intentId.isEmpty() || draft.runId.isEmpty())
        return d->fail(err("INVALID_ATTEMPT", QStringLiteral("Attempt, intent and run IDs are required"), draft.attemptId));
    if (draft.quantity <= 0) return d->fail(err("INVALID_QUANTITY", QStringLiteral("Quantity must be positive"), draft.attemptId));
    if (draft.ruleRevision <= 0) return d->fail(err("INVALID_RULE_REVISION", QStringLiteral("Revision must be positive"), draft.attemptId));
    if (draft.reservationId.isEmpty()) return d->fail(err("INVALID_RESERVATION", QStringLiteral("Reservation identity is required"), draft.attemptId));
    if (draft.scopeId.isEmpty() || draft.quotaTarget <= 0)
        return d->fail(err("QUOTA_SCOPE_UNRESOLVED", QStringLiteral("Explicit quota scope and target are required"), draft.attemptId));
    if (draft.kind != QStringLiteral("purchase") && draft.kind != QStringLiteral("collection"))
        return d->fail(err("INVALID_ATTEMPT_KIND", QStringLiteral("Unsupported attempt kind"), draft.attemptId));
    AttemptRecord attempt; bool found = false;
    if (auto e = d->findAttempt(draft.attemptId, attempt, found)) return *e;
    if (found) {
        if (!sameAttempt(attempt, draft)) return d->fail(err("ATTEMPT_ID_CONFLICT", QStringLiteral("Attempt identity differs"), draft.attemptId));
        attempt.duplicate = true; return attempt;
    }
    QSqlQuery query(d->writer);
    if (!execute(query, QStringLiteral("SELECT 1 FROM attempts WHERE intent_id=?"), {draft.intentId})) return d->fail(sqlError(query.lastError()));
    if (query.next()) return d->fail(err("INTENT_ID_CONFLICT", QStringLiteral("Intent already reserved"), draft.intentId));
    if (!execute(query, QStringLiteral("SELECT 1 FROM quota_reservations WHERE reservation_id=?"), {draft.reservationId})) return d->fail(sqlError(query.lastError()));
    if (query.next()) return d->fail(err("RESERVATION_CONFLICT", QStringLiteral("Reservation already exists"), draft.reservationId));
    if (!execute(query, QStringLiteral("SELECT COALESCE(SUM(quantity),0) FROM attempts WHERE scope_id=? AND (state='Success' OR reservation_held=1)"), {draft.scopeId}))
        return d->fail(sqlError(query.lastError()));
    if (!query.next()) return d->fail(err("DB_ERROR", QStringLiteral("Quota query returned no value")));
    if (query.value(0).toLongLong() + qint64(draft.quantity) > qint64(draft.quotaTarget))
        return d->fail(err("QUOTA_FULL", QStringLiteral("Confirmed success and held reservations occupy the quota"), draft.scopeId));
    query.finish();
    if (auto e = d->run(draft.runId, {})) return *e;
    if (!execute(query, QStringLiteral("INSERT INTO attempts(attempt_id,intent_id,run_id,rule_task_id,rule_revision,kind,state,quantity,reservation_held,dispatch_proof,reservation_id,scope_id,quota_target) VALUES(?,?,?,?,?,?,'Prepared',?,1,'not_dispatched',?,?,?)"),
                 {draft.attemptId, draft.intentId, draft.runId, text(draft.ruleTaskId), draft.ruleRevision, draft.kind,
                  draft.quantity, draft.reservationId, draft.scopeId, draft.quotaTarget})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (!execute(query, QStringLiteral("INSERT INTO quota_reservations(reservation_id,attempt_id,scope_id,quantity,state) VALUES(?,?,?,?,'held')"),
                 {draft.reservationId, draft.attemptId, draft.scopeId, draft.quantity})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (auto e = d->findAttempt(draft.attemptId, attempt, found)) return *e;
    return attempt;
}

StoreResult<AttemptRecord> SqliteEventStore::recordDispatch(const DispatchDraft& draft)
{
    if (auto e = d->begin()) return *e;
    AttemptRecord attempt; bool found = false;
    if (auto e = d->findAttempt(draft.attemptId, attempt, found)) return *e;
    if (!found) return d->fail(err("ATTEMPT_NOT_FOUND", QStringLiteral("Attempt does not exist"), draft.attemptId));
    if (attempt.state == QStringLiteral("Unknown")) return d->fail(err("ATTEMPT_UNKNOWN", QStringLiteral("Unknown attempt requires reconciliation, never redispatch"), draft.attemptId));
    if (terminal(attempt.state)) return d->fail(err("ATTEMPT_TERMINAL", QStringLiteral("Terminal attempt cannot dispatch"), draft.attemptId));
    auto stateRank = [](const QString& state) { return state == QStringLiteral("Dispatching") ? 1 : state == QStringLiteral("Sent") ? 2 : state == QStringLiteral("AwaitingReceipt") ? 3 : 0; };
    auto proofRank = [](const QString& proof) { return proof == QStringLiteral("possibly_dispatched") ? 1 : proof == QStringLiteral("acknowledged") ? 2 : proof == QStringLiteral("not_dispatched") ? 0 : -1; };
    if (!stateRank(draft.state)) return d->fail(err("INVALID_DISPATCH_STATE", QStringLiteral("Unsupported dispatch state"), draft.attemptId));
    if (proofRank(draft.dispatchProof) < 0) return d->fail(err("INVALID_DISPATCH_PROOF", QStringLiteral("Unsupported dispatch proof"), draft.attemptId));
    if (stateRank(draft.state) < stateRank(attempt.state) || proofRank(draft.dispatchProof) < proofRank(attempt.dispatchProof))
        return d->fail(err("DISPATCH_REGRESSION", QStringLiteral("Dispatch state or proof cannot move backwards"), draft.attemptId));
    if (attempt.state == draft.state && attempt.dispatchProof == draft.dispatchProof) {
        attempt.duplicate = true; return attempt;
    }
    QSqlQuery query(d->writer);
    if (!execute(query, QStringLiteral("UPDATE attempts SET state=?,dispatch_proof=?,dispatch_event_id=? WHERE attempt_id=?"),
                 {draft.state, draft.dispatchProof, text(draft.eventId), draft.attemptId})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    attempt.state = draft.state; attempt.dispatchProof = draft.dispatchProof;
    return attempt;
}

StoreResult<AttemptRecord> SqliteEventStore::applyReceipt(const ReceiptDraft& draft)
{
    if (auto e = d->begin()) return *e;
    AttemptRecord attempt; bool found = false;
    if (auto e = d->findAttempt(draft.attemptId, attempt, found)) return *e;
    if (!found) return d->fail(err("ATTEMPT_NOT_FOUND", QStringLiteral("Attempt does not exist"), draft.attemptId));
    if (draft.receiptIdentity.isEmpty()) return d->fail(err("INVALID_RECEIPT", QStringLiteral("Receipt identity is required"), draft.attemptId));
    if (draft.outcome != QStringLiteral("success") && draft.outcome != QStringLiteral("failed") && draft.outcome != QStringLiteral("unknown"))
        return d->fail(err("INVALID_RECEIPT_OUTCOME", QStringLiteral("Unsupported receipt outcome"), draft.attemptId));
    if (draft.association != QStringLiteral("confirmed") && draft.association != QStringLiteral("ambiguous"))
        return d->fail(err("INVALID_ASSOCIATION", QStringLiteral("Unsupported receipt association"), draft.attemptId));
    QSqlQuery query(d->writer);
    if (!execute(query, QStringLiteral("SELECT outcome,association,event_id FROM receipts WHERE receipt_identity=? AND attempt_id=?"), {draft.receiptIdentity, draft.attemptId}))
        return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (query.next()) {
        // Re-observation may carry a new event ID. Business receipt identity
        // controls idempotency; preserve the first association in storage.
        if (query.value(0).toString() != draft.outcome || query.value(1).toString() != draft.association)
            return d->fail(err("RECEIPT_CONFLICT", QStringLiteral("Receipt identity differs"), draft.receiptIdentity));
        attempt.duplicate = true; return attempt;
    }
    if (terminal(attempt.state)) return d->fail(err("ATTEMPT_TERMINAL", QStringLiteral("Terminal attempt cannot accept new receipts"), draft.attemptId));
    if (!execute(query, QStringLiteral("INSERT INTO receipts(receipt_identity,attempt_id,outcome,association,event_id) VALUES(?,?,?,?,?)"),
                 {draft.receiptIdentity, draft.attemptId, draft.outcome, draft.association, text(draft.eventId)})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    attempt.receiptIdentity = draft.receiptIdentity;
    const bool unknown = draft.outcome == QStringLiteral("unknown") || draft.association != QStringLiteral("confirmed");
    attempt.state = unknown ? QStringLiteral("Unknown") : QStringLiteral("AwaitingReceipt");
    // A receipt is evidence of possible dispatch even if a preceding process
    // failed before recording dispatch. Never regress acknowledged evidence.
    if (attempt.dispatchProof == QStringLiteral("not_dispatched")) attempt.dispatchProof = QStringLiteral("possibly_dispatched");
    if (!execute(query, QStringLiteral("UPDATE attempts SET receipt_identity=?,state=?,dispatch_proof=? WHERE attempt_id=?"),
                 {draft.receiptIdentity, attempt.state, attempt.dispatchProof, draft.attemptId})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (unknown && !execute(query, QStringLiteral("UPDATE quota_reservations SET state='unresolved' WHERE reservation_id=?"), {attempt.reservationId}))
        return d->fail(sqlError(query.lastError(), draft.attemptId));
    return attempt;
}

StoreResult<LedgerTransactionRecord> SqliteEventStore::commitLedger(const LedgerDraft& draft)
{
    if (auto e = d->begin()) return *e;
    AttemptRecord attempt; bool found = false;
    if (auto e = d->findAttempt(draft.attemptId, attempt, found)) return *e;
    if (!found) return d->fail(err("ATTEMPT_NOT_FOUND", QStringLiteral("Attempt does not exist"), draft.attemptId));
    if (draft.transactionId.isEmpty()) return d->fail(err("INVALID_TRANSACTION", QStringLiteral("Transaction identity is required"), draft.attemptId));
    if (draft.terminal != QStringLiteral("Success") && draft.terminal != QStringLiteral("Failed"))
        return d->fail(err("INVALID_TERMINAL", QStringLiteral("Unsupported ledger terminal"), draft.transactionId));
    if (!attempt.transactionId.isEmpty() && attempt.transactionId != draft.transactionId)
        return d->fail(err("ATTEMPT_TRANSACTION_CONFLICT", QStringLiteral("Attempt already has a ledger transaction"), draft.attemptId));
    QSqlQuery query(d->writer);
    if (!execute(query, QStringLiteral("SELECT transaction_id,attempt_id,terminal,queue_empty,receipt_identity,event_id FROM ledger_transactions WHERE transaction_id=?"), {draft.transactionId}))
        return d->fail(sqlError(query.lastError(), draft.transactionId));
    if (query.next()) {
        const auto old = readTransaction(query);
        if (old.attemptId != draft.attemptId || old.terminal != draft.terminal || old.queueEmpty != draft.queueEmpty
            || old.receiptIdentity != draft.receiptIdentity)
            return d->fail(err("TRANSACTION_ID_CONFLICT", QStringLiteral("Transaction identity differs"), draft.transactionId));
        return old;
    }
    if (attempt.state != QStringLiteral("AwaitingReceipt") && attempt.state != QStringLiteral("Unknown"))
        return d->fail(err("ATTEMPT_STATE_INVALID", QStringLiteral("Ledger commit requires pending receipt evidence"), draft.attemptId));
    if (attempt.receiptIdentity.isEmpty()) return d->fail(err("RECEIPT_REQUIRED", QStringLiteral("A receipt is required"), draft.attemptId));
    if (attempt.receiptIdentity != draft.receiptIdentity) return d->fail(err("RECEIPT_MISMATCH", QStringLiteral("Ledger receipt differs from attempt receipt"), draft.attemptId));
    if (!execute(query, QStringLiteral("SELECT outcome,association FROM receipts WHERE receipt_identity=? AND attempt_id=?"), {draft.receiptIdentity, draft.attemptId}))
        return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (!query.next()) return d->fail(err("RECEIPT_REQUIRED", QStringLiteral("Receipt evidence does not exist"), draft.attemptId));
    const QString outcome = query.value(0).toString();
    if (outcome == QStringLiteral("unknown") || query.value(1).toString() != QStringLiteral("confirmed"))
        return d->fail(err("RECEIPT_UNCONFIRMED", QStringLiteral("Unknown or ambiguous receipt cannot create a terminal ledger record"), draft.attemptId));
    if ((draft.terminal == QStringLiteral("Success")) != (outcome == QStringLiteral("success")))
        return d->fail(err("RECEIPT_OUTCOME_MISMATCH", QStringLiteral("Ledger terminal disagrees with confirmed receipt outcome"), draft.attemptId));
    query.finish();
    if (!execute(query, QStringLiteral("INSERT INTO ledger_transactions(transaction_id,attempt_id,terminal,queue_empty,receipt_identity,event_id) VALUES(?,?,?,?,?,?)"),
                 {draft.transactionId, draft.attemptId, draft.terminal, draft.queueEmpty ? 1 : 0, draft.receiptIdentity, text(draft.eventId)}))
        return d->fail(sqlError(query.lastError(), draft.transactionId));
    if (!execute(query, QStringLiteral("UPDATE attempts SET state=?,reservation_held=0,transaction_id=? WHERE attempt_id=?"),
                 {draft.terminal, draft.transactionId, draft.attemptId})) return d->fail(sqlError(query.lastError(), draft.attemptId));
    if (!execute(query, QStringLiteral("UPDATE quota_reservations SET state=? WHERE reservation_id=?"),
                 {draft.terminal == QStringLiteral("Success") ? QStringLiteral("consumed") : QStringLiteral("released"), attempt.reservationId}))
        return d->fail(sqlError(query.lastError(), draft.attemptId));
    return LedgerTransactionRecord{draft.transactionId, draft.attemptId, draft.terminal, draft.queueEmpty, draft.receiptIdentity};
}

StoreResult<LedgerSnapshot> SqliteEventStore::snapshot(const QString& runId) const
{
    if (auto e = d->ready()) return *e;
    if (!d->reader.transaction()) return sqlError(d->reader.lastError());
    auto failure = [&](const StoreError& error) -> StoreResult<LedgerSnapshot> { d->reader.rollback(); return error; };
    LedgerSnapshot out; out.runId = runId;
    {
        QSqlQuery query(d->reader);
        if (!execute(query, QStringLiteral("SELECT ") + attemptColumns + QStringLiteral(" FROM attempts WHERE run_id=? ORDER BY attempt_id"), {text(runId)}))
            return failure(sqlError(query.lastError()));
        qint64 successes = 0;
        while (query.next()) {
            const auto a = readAttempt(query); out.attempts.push_back(a);
            if (a.state == QStringLiteral("Success")) successes += a.quantity;
            if (a.reservationHeld) ++out.unresolvedReservations;
        }
        if (successes > std::numeric_limits<int>::max()) return failure(err("SNAPSHOT_OVERFLOW", QStringLiteral("Committed success count exceeds the projection type")));
        out.confirmedSuccess = int(successes);
        if (!execute(query, QStringLiteral("SELECT q.reservation_id,q.attempt_id,q.scope_id,q.quantity,q.state FROM quota_reservations q JOIN attempts a ON a.attempt_id=q.attempt_id WHERE a.run_id=? ORDER BY q.reservation_id"), {text(runId)}))
            return failure(sqlError(query.lastError()));
        while (query.next()) out.reservations.push_back({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(), query.value(3).toInt(), query.value(4).toString()});
        if (!execute(query, QStringLiteral("SELECT l.transaction_id,l.attempt_id,l.terminal,l.queue_empty,l.receipt_identity FROM ledger_transactions l JOIN attempts a ON a.attempt_id=l.attempt_id WHERE a.run_id=? ORDER BY l.transaction_id"), {text(runId)}))
            return failure(sqlError(query.lastError()));
        while (query.next()) out.transactions.push_back(readTransaction(query));
    }
    if (!d->reader.commit()) return failure(sqlError(d->reader.lastError()));
    return out;
}

StoreResult<RecoverySet> SqliteEventStore::recoverUnresolved(const QString& sessionId) const
{
    if (auto e = d->ready()) return *e;
    QSqlQuery query(d->reader);
    const QString sql = QStringLiteral("SELECT ") + attemptColumns + QStringLiteral(
        " FROM attempts WHERE reservation_held=1")
        + (sessionId.isEmpty() ? QString() : QStringLiteral(" AND run_id IN (SELECT run_id FROM runs WHERE session_id=?)"))
        + QStringLiteral(" ORDER BY attempt_id");
    if (!execute(query, sql, sessionId.isEmpty() ? QVariantList{} : QVariantList{sessionId})) return sqlError(query.lastError());
    RecoverySet out;
    while (query.next()) out.attempts.push_back(readAttempt(query));
    return out;
}

StoreResult<int> SqliteEventStore::eventCount() const
{
    if (auto e = d->ready()) return *e;
    QSqlQuery query(d->reader);
    if (!query.exec(QStringLiteral("SELECT count(*) FROM events"))) return sqlError(query.lastError());
    if (!query.next()) return err("DB_ERROR", QStringLiteral("Event count query returned no value"));
    const auto count = query.value(0).toLongLong();
    if (count > std::numeric_limits<int>::max()) return err("SNAPSHOT_OVERFLOW", QStringLiteral("Event count exceeds the projection type"));
    return int(count);
}

VoidResult SqliteEventStore::commit()
{
    if (auto e = d->ready(true)) return *e;
    if (!d->transaction) return StoreVoid{};
    // QSQLITE's database-level commit()/rollback() helpers discard native
    // SQLite error codes in some Qt versions. Explicit statements preserve
    // SQLITE_BUSY/FULL and the exact already-auto-rolled-back diagnostic.
    QSqlQuery query(d->writer);
    if (!query.exec(QStringLiteral("COMMIT"))) return d->fail(sqlError(query.lastError()));
    d->transaction = false;
    return StoreVoid{};
}

VoidResult SqliteEventStore::rollback()
{
    if (auto e = d->ready()) return *e;
    if (d->transaction) {
        QSqlQuery query(d->writer);
        if (!query.exec(QStringLiteral("ROLLBACK"))) return d->fail(sqlError(query.lastError()));
    }
    d->transaction = false; d->aborted = false;
    return StoreVoid{};
}

VoidResult SqliteEventStore::backupTo(const QString& destinationPath) const
{
    if (auto e = d->ready()) return *e;
    if (d->aborted) return err("TRANSACTION_ABORTED", QStringLiteral("Roll back the aborted unit of work before backing up"));
    if (d->transaction) return err("TRANSACTION_ACTIVE", QStringLiteral("Commit or roll back before taking a backup"));
    QFileInfo destination(destinationPath);
    if (destinationPath.trimmed().isEmpty() || !QFileInfo(destination.absolutePath()).isDir())
        return err("INVALID_PATH", QStringLiteral("Backup parent directory must exist"), destinationPath);
    if (destination.exists()) return err("BACKUP_EXISTS", QStringLiteral("Backup never overwrites an existing destination"), destinationPath);
    const QString target = destination.absoluteFilePath();
    QSqlQuery query(d->reader);
    if (!execute(query, QStringLiteral("VACUUM INTO ?"), {target})) return sqlError(query.lastError(), target);
    query.finish();
    // Reopen the output independently: success means a current, readable
    // consistent database, not merely a copied main file without its journal.
    const QString name = QStringLiteral("dma_backup_verify_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Error failure;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(target); database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!database.open()) failure = sqlError(database.lastError(), target);
        else {
            int version = 0;
            failure = inspectSchema(database, version);
            if (!failure && version != CurrentSchemaVersion) failure = err("SCHEMA_INVALID", QStringLiteral("Backup has an unexpected version"), target);
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    if (failure) return *failure;
    return StoreVoid{};
}

} // namespace relink::ledger
