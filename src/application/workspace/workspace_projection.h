#pragma once

#include <QStringList>
#include <QVector>

// Read-only data passed from workspace services to UI; no controller/SQL dependency.
namespace relink::workspace {

struct ProfileRow {
    QString id, name, source, sourceHash, reviewState, path;
    int revision = 0;
    bool builtin = false, selected = false, activationRequired = true;
};
struct ListingRow {
    QString listingId, observationId, productId, productName, source, clockDomainId;
    QString observedPrice, confirmedPrice, status, reason;
    QStringList missingFields;
    qint64 observedMonoMs = 0;
    bool stale = false;
};
struct RecordRow {
    QString eventId, runId, type, source, reason, listingId, observationId;
    QString observedPrice, confirmedPrice, clockDomainId;
    qint64 seq = 0, atMonoMs = 0;
};
struct RunRow {
    QString id, mode, profileId, profileName, state, source;
    int profileRevision = 0, confirmedSuccess = 0, unknownCount = 0, reservationCount = 0;
    bool recovered = false;
};
struct WorkspaceProjection {
    QVector<ProfileRow> profiles;
    QVector<RunRow> runs;
    QVector<ListingRow> listings;
    QVector<RecordRow> records;
    QString rootPath, databasePath, selectedProfileId, selectedRunId;
    QString mode = QStringLiteral("Replay"), runState = QStringLiteral("Ready");
    QString source, clockDomainId, runProfileId, runProfileName, lastError;
    int runProfileRevision = 0, confirmedSuccess = 0, unknownCount = 0, reservationCount = 0;
    int matchedCount = 0, failedCount = 0, noMatchCount = 0, needsReviewCount = 0;
    int dispatchedCount = 0, collectedCount = 0;
    int fixtureStep = 0, fixtureStepCount = 8, imageFileWriteCount = 0;
    qint64 nowMonoMs = 0;
    bool opened = false, readOnly = false, dirty = false, active = false, historyReadOnly = false;
    bool canStart = false, canPause = false, canResume = false, canStop = false, canStep = false;
};

} // namespace relink::workspace
