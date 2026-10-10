#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

class QTimer;

struct Skin {
    QString id, name, series, condition, rarity;
    double wear = 0, price = 0, change = 0;
    bool followed = false;
    // Product identity is separate from a listing's condition/price/wear.
    // A catalogue skin's 品阶 is in `rarity` (待核对 when not recorded).
    QString catalogProductId = {}, variant = {}, skinSeries = {};
    // A zero is not evidence that an unobserved market value is known.
    bool priceKnown = false, wearKnown = false, changeKnown = false;
    QString dataSource = QStringLiteral("catalog");
};

// Immutable import provenance, independent from editable task conditions.
// Empty format means this task was created manually. Original values stay here
// even when a user subsequently edits the task in the collection editor.
struct TaskImportSource {
    QString format, sourceSha256, dictionarySha256, productId, conditionId;
    int row = -1;
    QJsonObject fields;
};

struct Task {
    QString id, name, skinId;
    double minPrice = 0, maxPrice = 1000, maxWear = 10;
    int quantity = 1; // 0 = unlimited, matching the original collection task.
    bool enabled = true;
    QString status = QStringLiteral("待启动");
    // Condition filter from the original task row; "不限" keeps older files unfiltered.
    QString condition = QStringLiteral("不限");
    // Decode-only provenance distinguishes a missing legacy condition field
    // from the user's explicit 不限 choice. New tasks are always explicit.
    bool conditionExplicit = true;
    TaskImportSource importSource;
};

// Parameters mirrored from the original assistant's main screen. They are
// validated and saved with the configuration; this frontend does not act on them.
struct RunSettings {
    QString profile = QStringLiteral("S11新赛季1103");
    QString hotkey = QStringLiteral("F2");
    // User 2026-10-09: the delay after the purchase dialog's countdown reaches
    // zero before the green button is clicked ("归零后多少延迟之后准确的点击购买").
    int purchaseDelayMs = 830;
    // Seconds before zero at which the purchase dialog is opened (1..5: the
    // price button appears 5 s before zero, live preentry01/02).
    int enterBeforeSeconds = 3;
    bool dynamicDelay = false;
    int queueFullTrigger = 1;
    double queueFullStepMs = 1.0;
    int publicityTrigger = 1;
    double publicityStepMs = 1.0;
    bool burstClick = true;
    int clickIntervalMs = 10;
    int limitOrange = 10, limitPurple = 10, limitBlue = 10;
    bool refreshPage = false;
    bool skipLotteryPage = true;
    bool skipSuccessPage = true;
    bool autoCollect = true;
    bool collectOsd = false;
    bool scheduleEnabled = false;
    QString scheduleStart = QStringLiteral("09:00");
    QString scheduleStop = QStringLiteral("23:00");
};

struct LogEntry {
    QString time, level, message;
};

// Pure, side-effect-free schema v1 codec shared by the old loader and the
// import adapters. decodeV1Config validates the complete input with the same
// rules as the original parser before writing anything: on failure every out
// parameter is left exactly as the caller passed it in.
bool decodeV1Config(const QByteArray& bytes, QVector<Skin>& skins, QVector<Task>& tasks,
                    RunSettings& run, QString* error = nullptr);
// Serializes the exact schema v1 layout written by AppState::saveTo.
QJsonObject encodeV1Config(const QVector<Skin>& skins, const QVector<Task>& tasks,
                           const RunSettings& run);
QJsonObject encodeRunSettings(const RunSettings& run);

// Configuration/presentation model. Construction never invents products, prices,
// follows or tasks. The startup composition supplies the verified catalogue.
// Synthetic matching remains available only after explicit test-fixture loading.
class AppState : public QObject {
    Q_OBJECT
public:
    explicit AppState(QObject* parent = nullptr);

    QVector<Skin> skins;
    QVector<Task> tasks;
    RunSettings run;
    QVector<LogEntry> logs;
    bool simulationRunning = false;
    int simulatedScans = 0, simulatedMatches = 0, simulatedSuccess = 0;
    QString configPath;

    void loadTestFixture();
    void loadDemo(); // Compatibility alias for old explicit tests; never called at startup.
    bool saveTo(const QString& path, QString* error = nullptr) const;
    bool loadFrom(const QString& path, QString* error = nullptr);
    bool exportPricesCsv(const QString& path, QString* error = nullptr) const;
    void notifyChanged();
    void startSimulation();
    void pauseSimulation();
    void simulateTick();
    void resetTaskSimulation(const QString& id);
    void addLog(const QString& level, const QString& message);

signals:
    void changed();

private:
    bool m_testFixture = false;
    QTimer* m_timer = nullptr;
    QHash<QString, int> m_simulatedByTask;
};
