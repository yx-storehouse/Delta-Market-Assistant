#pragma once

#include <QHash>
#include <QQueue>
#include <QString>
#include <QVector>

namespace relink::vision {

enum class LeaseState { Free, Writing, Pending, InUse, AwaitRelease, Quarantined };

QString toString(LeaseState state);

struct LeaseHandle {
    QString leaseId;
    int slotIndex = -1;
    qint64 generation = 0;
    qint64 byteLength = 0;
    LeaseState state = LeaseState::Free;
};

struct LeaseOperation {
    bool ok = false;
    QString error;
    LeaseHandle lease;
};

// Two-slot lifecycle used by the future worker bridge. A slot is never
// overwritten while published/in use; cancellation and release remain
// separate events, and crash quarantine lasts until process exit plus local
// mapping close have both been confirmed.
class LeasePool final {
public:
    explicit LeasePool(qint64 slotCapacityBytes = 134217728);

    LeaseOperation beginWrite(qint64 byteLength);
    LeaseOperation replacePending(const LeaseHandle& pending, qint64 byteLength);
    bool publish(const LeaseHandle& writing, QString* error = nullptr);
    bool acquireForWorker(const LeaseHandle& pending, QString* error = nullptr);
    bool release(const LeaseHandle& lease, QString* error = nullptr);
    bool cancel(const LeaseHandle& lease, QString* error = nullptr);

    bool workerUnresponsive(const LeaseHandle& lease, QString* error = nullptr);
    bool processExitConfirmed(const LeaseHandle& lease, QString* error = nullptr);
    bool closeMapping(const LeaseHandle& lease, QString* error = nullptr);

    QVector<LeaseHandle> snapshot() const;
    LeaseHandle current(int slotIndex) const;
    int freeCount() const;
    qint64 slotCapacityBytes() const { return m_capacity; }
    int releasedHistorySize() const { return m_releasedLeases.size(); }
    static constexpr int releasedHistoryLimit = 1024;

private:
    struct Slot {
        LeaseHandle lease;
        qint64 nextGeneration = 1;
        bool processExited = false;
    };

    Slot* find(const LeaseHandle& lease);
    const Slot* find(const LeaseHandle& lease) const;
    LeaseOperation fail(const QString& error) const;
    bool transition(const LeaseHandle& lease, LeaseState expected,
                    LeaseState next, QString* error);
    static bool sameLease(const LeaseHandle& lhs, const LeaseHandle& rhs);
    void rememberReleased(const LeaseHandle& lease);
    bool wasReleased(const LeaseHandle& lease) const;

    qint64 m_capacity;
    QVector<Slot> m_slots;
    QHash<QString, LeaseHandle> m_releasedLeases;
    QQueue<QString> m_releaseOrder;
    bool m_quarantineActive = false;
};

} // namespace relink::vision
