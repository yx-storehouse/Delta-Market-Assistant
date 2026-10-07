#include "lease_pool.h"

#include <QUuid>
#include <limits>

namespace relink::vision {
namespace {
bool reject(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}
}

QString toString(LeaseState state)
{
    switch (state) {
    case LeaseState::Free: return QStringLiteral("Free");
    case LeaseState::Writing: return QStringLiteral("Writing");
    case LeaseState::Pending: return QStringLiteral("Pending");
    case LeaseState::InUse: return QStringLiteral("InUse");
    case LeaseState::AwaitRelease: return QStringLiteral("AwaitRelease");
    case LeaseState::Quarantined: return QStringLiteral("Quarantined");
    }
    return {};
}

LeasePool::LeasePool(qint64 slotCapacityBytes) : m_capacity(slotCapacityBytes)
{
    m_slots.resize(2);
    for (int i = 0; i < m_slots.size(); ++i) m_slots[i].lease.slotIndex = i;
}

LeaseOperation LeasePool::fail(const QString& error) const { return {false, error, {}}; }

bool LeasePool::sameLease(const LeaseHandle& lhs, const LeaseHandle& rhs)
{
    return lhs.slotIndex == rhs.slotIndex && lhs.generation == rhs.generation
        && !lhs.leaseId.isEmpty() && lhs.leaseId == rhs.leaseId;
}

void LeasePool::rememberReleased(const LeaseHandle& lease)
{
    if (m_releasedLeases.contains(lease.leaseId)) return;
    while (m_releaseOrder.size() >= releasedHistoryLimit)
        m_releasedLeases.remove(m_releaseOrder.dequeue());
    m_releasedLeases.insert(lease.leaseId, lease);
    m_releaseOrder.enqueue(lease.leaseId);
}

bool LeasePool::wasReleased(const LeaseHandle& lease) const
{
    const auto previous = m_releasedLeases.constFind(lease.leaseId);
    return previous != m_releasedLeases.cend() && sameLease(previous.value(), lease);
}

LeasePool::Slot* LeasePool::find(const LeaseHandle& lease)
{
    if (lease.slotIndex < 0 || lease.slotIndex >= m_slots.size()) return nullptr;
    auto& slot = m_slots[lease.slotIndex];
    return sameLease(slot.lease, lease) ? &slot : nullptr;
}

const LeasePool::Slot* LeasePool::find(const LeaseHandle& lease) const
{
    if (lease.slotIndex < 0 || lease.slotIndex >= m_slots.size()) return nullptr;
    const auto& slot = m_slots[lease.slotIndex];
    return sameLease(slot.lease, lease) ? &slot : nullptr;
}

LeaseOperation LeasePool::beginWrite(qint64 byteLength)
{
    if (m_capacity < 4 || m_capacity > 134217728 || byteLength < 1 || byteLength > m_capacity)
        return fail(QStringLiteral("E_SHM_BOUNDS"));
    if (m_quarantineActive) return fail(QStringLiteral("E_LEASE_QUARANTINED"));
    for (auto& slot : m_slots) {
        if (slot.lease.state != LeaseState::Free) continue;
        if (slot.nextGeneration > 9007199254740991LL) return fail(QStringLiteral("E_LEASE_UNKNOWN"));
        slot.lease.leaseId = QStringLiteral("lease-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        slot.lease.generation = slot.nextGeneration++;
        slot.lease.byteLength = byteLength;
        slot.lease.state = LeaseState::Writing;
        slot.processExited = false;
        return {true, {}, slot.lease};
    }
    return fail(QStringLiteral("E_LEASE_BUSY"));
}

LeaseOperation LeasePool::replacePending(const LeaseHandle& pending, qint64 byteLength)
{
    if (m_quarantineActive) return fail(QStringLiteral("E_LEASE_QUARANTINED"));
    auto* slot = find(pending);
    if (!slot) return fail(QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != LeaseState::Pending) return fail(QStringLiteral("E_LEASE_BUSY"));
    if (byteLength < 1 || byteLength > m_capacity) return fail(QStringLiteral("E_SHM_BOUNDS"));
    if (slot->nextGeneration > 9007199254740991LL) return fail(QStringLiteral("E_LEASE_UNKNOWN"));
    rememberReleased(slot->lease);
    slot->lease.leaseId = QStringLiteral("lease-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    slot->lease.generation = slot->nextGeneration++;
    slot->lease.byteLength = byteLength;
    slot->lease.state = LeaseState::Writing;
    return {true, {}, slot->lease};
}

bool LeasePool::transition(const LeaseHandle& lease, LeaseState expected,
                           LeaseState next, QString* error)
{
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != expected) return reject(error, QStringLiteral("E_LEASE_STATE"));
    slot->lease.state = next;
    if (error) error->clear();
    return true;
}

bool LeasePool::publish(const LeaseHandle& writing, QString* error)
{
    return transition(writing, LeaseState::Writing, LeaseState::Pending, error);
}

bool LeasePool::acquireForWorker(const LeaseHandle& pending, QString* error)
{
    for (const auto& slot : m_slots)
        if (slot.lease.state == LeaseState::InUse || slot.lease.state == LeaseState::AwaitRelease || slot.lease.state == LeaseState::Quarantined)
            return reject(error, QStringLiteral("E_LEASE_BUSY"));
    return transition(pending, LeaseState::Pending, LeaseState::InUse, error);
}

bool LeasePool::release(const LeaseHandle& lease, QString* error)
{
    if (wasReleased(lease)) { if (error) error->clear(); return true; }
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != LeaseState::AwaitRelease && slot->lease.state != LeaseState::InUse)
        return reject(error, QStringLiteral("E_LEASE_STATE"));
    rememberReleased(slot->lease);
    slot->lease.state = LeaseState::Free;
    slot->lease.byteLength = 0;
    if (error) error->clear();
    return true;
}

bool LeasePool::cancel(const LeaseHandle& lease, QString* error)
{
    if (wasReleased(lease)) { if (error) error->clear(); return true; }
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state == LeaseState::InUse) slot->lease.state = LeaseState::AwaitRelease;
    else if (slot->lease.state == LeaseState::Writing || slot->lease.state == LeaseState::Pending) {
        rememberReleased(slot->lease);
        slot->lease.state = LeaseState::Free;
        slot->lease.byteLength = 0;
    } else if (slot->lease.state != LeaseState::AwaitRelease && slot->lease.state != LeaseState::Quarantined)
        return reject(error, QStringLiteral("E_LEASE_STATE"));
    if (error) error->clear();
    return true;
}

bool LeasePool::workerUnresponsive(const LeaseHandle& lease, QString* error)
{
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != LeaseState::InUse && slot->lease.state != LeaseState::AwaitRelease
        && slot->lease.state != LeaseState::Quarantined)
        return reject(error, QStringLiteral("E_LEASE_STATE"));
    slot->lease.state = LeaseState::Quarantined;
    m_quarantineActive = true;
    if (error) error->clear();
    return true;
}

bool LeasePool::processExitConfirmed(const LeaseHandle& lease, QString* error)
{
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != LeaseState::Quarantined) return reject(error, QStringLiteral("E_LEASE_STATE"));
    slot->processExited = true;
    if (error) error->clear();
    return true;
}

bool LeasePool::closeMapping(const LeaseHandle& lease, QString* error)
{
    auto* slot = find(lease);
    if (!slot) return reject(error, QStringLiteral("E_LEASE_UNKNOWN"));
    if (slot->lease.state != LeaseState::Quarantined || !slot->processExited)
        return reject(error, QStringLiteral("E_LEASE_STATE"));
    rememberReleased(slot->lease);
    slot->lease.state = LeaseState::Free;
    slot->lease.byteLength = 0;
    m_quarantineActive = false;
    if (error) error->clear();
    return true;
}

QVector<LeaseHandle> LeasePool::snapshot() const
{
    QVector<LeaseHandle> output;
    for (const auto& slot : m_slots) output.push_back(slot.lease);
    return output;
}

LeaseHandle LeasePool::current(int slotIndex) const
{
    return slotIndex < 0 || slotIndex >= m_slots.size() ? LeaseHandle{} : m_slots.at(slotIndex).lease;
}

int LeasePool::freeCount() const
{
    int count = 0;
    for (const auto& slot : m_slots) if (slot.lease.state == LeaseState::Free) ++count;
    return count;
}

} // namespace relink::vision
