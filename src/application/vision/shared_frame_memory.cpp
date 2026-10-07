#include "shared_frame_memory.h"

#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <limits>

namespace relink::vision {
namespace {
constexpr qint64 kMaxCapacity = 134217728;
const QRegularExpression kSessionId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
const QRegularExpression kLeaseId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

#ifdef Q_OS_WIN
QString winError(const QString& prefix, DWORD code = GetLastError())
{
    return QStringLiteral("%1 (win32=%2)").arg(prefix).arg(code);
}
#endif

bool validSession(const QString& sessionId, QString* error)
{
    if (!kSessionId.match(sessionId).hasMatch()) {
        setError(error, QStringLiteral("E_SHM_OPEN"));
        return false;
    }
    return true;
}

bool validSlot(int slotIndex, QString* error)
{
    if (slotIndex < 0 || slotIndex > 1) {
        setError(error, QStringLiteral("E_SHM_OPEN"));
        return false;
    }
    return true;
}

bool validCapacity(qint64 capacityBytes, QString* error)
{
    if (capacityBytes < 4 || capacityBytes > kMaxCapacity) {
        setError(error, QStringLiteral("E_SHM_BOUNDS"));
        return false;
    }
    return true;
}

QString mappingName(const QString& sessionId, int slotIndex)
{
    return QStringLiteral("Local\\RelinkVision_%1_%2").arg(sessionId).arg(slotIndex);
}

bool jsonInteger(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64* out,
                 QString* error)
{
    if (!value.isDouble()) {
        setError(error, QStringLiteral("E_SHM_BOUNDS"));
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        setError(error, QStringLiteral("E_SHM_BOUNDS"));
        return false;
    }
    const qint64 converted = value.toInteger();
    if (converted < minimum || converted > maximum) {
        setError(error, QStringLiteral("E_SHM_BOUNDS"));
        return false;
    }
    if (out) *out = converted;
    return true;
}

bool exactKeys(const QJsonObject& object, const QSet<QString>& required,
               QString* error)
{
    for (const QString& key : object.keys()) {
        if (!required.contains(key)) {
            setError(error, QStringLiteral("E_MESSAGE_INVALID"));
            return false;
        }
    }
    for (const QString& key : required) {
        if (!object.contains(key)) {
            setError(error, QStringLiteral("E_MESSAGE_INVALID"));
            return false;
        }
    }
    return true;
}

}

struct SharedFrameMemory::Impl {
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    void* view = nullptr;
#endif
    QString sessionId;
    QString name;
    int slotIndex = -1;
    qint64 capacity = 0;
    qint64 offset = 0;
    qint64 length = 0;
};

struct ReadOnlyFrameMemory::Impl {
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    const void* view = nullptr;
#endif
    QString name;
    qint64 capacity = 0;
    qint64 offset = 0;
    qint64 length = 0;
};

SharedFrameMemory::SharedFrameMemory() : m_impl(new Impl) {}
SharedFrameMemory::~SharedFrameMemory() { close(); delete m_impl; }

bool SharedFrameMemory::create(const QString& sessionId, int slotIndex,
                               qint64 capacityBytes, QString* error)
{
    setError(error, {});
    if (isOpen()) {
        setError(error, QStringLiteral("E_SHM_BUSY"));
        return false;
    }
    if (!validSession(sessionId, error) || !validSlot(slotIndex, error)
        || !validCapacity(capacityBytes, error)) return false;
#ifndef Q_OS_WIN
    Q_UNUSED(sessionId); Q_UNUSED(slotIndex); Q_UNUSED(capacityBytes);
    setError(error, QStringLiteral("E_SHM_UNSUPPORTED"));
    return false;
#else
    const QString name = mappingName(sessionId, slotIndex);
    const std::wstring wideName = name.toStdWString();
    const DWORD sizeHigh = static_cast<DWORD>(static_cast<qulonglong>(capacityBytes) >> 32);
    const DWORD sizeLow = static_cast<DWORD>(static_cast<qulonglong>(capacityBytes) & 0xffffffffULL);
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       sizeHigh, sizeLow, wideName.c_str());
    if (!handle) {
        setError(error, winError(QStringLiteral("E_SHM_OPEN")));
        return false;
    }
    const DWORD creationError = GetLastError();
    if (creationError == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        setError(error, QStringLiteral("E_SHM_EXISTS"));
        return false;
    }
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
        const QString failure = winError(QStringLiteral("E_SHM_OPEN"));
        CloseHandle(handle);
        setError(error, failure);
        return false;
    }
    void* view = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                               static_cast<SIZE_T>(capacityBytes));
    if (!view) {
        const QString failure = winError(QStringLiteral("E_SHM_OPEN"));
        CloseHandle(handle);
        setError(error, failure);
        return false;
    }
    m_impl->handle = handle;
    m_impl->view = view;
    m_impl->sessionId = sessionId;
    m_impl->name = name;
    m_impl->slotIndex = slotIndex;
    m_impl->capacity = capacityBytes;
    m_impl->offset = 0;
    m_impl->length = 0;
    return true;
#endif
}

bool SharedFrameMemory::write(const QByteArray& pixels, qint64 offset, QString* error)
{
    setError(error, {});
    if (!isOpen()) { setError(error, QStringLiteral("E_SHM_OPEN")); return false; }
    if (pixels.isEmpty() || offset < 0 || offset > m_impl->capacity
        || static_cast<qint64>(pixels.size()) > m_impl->capacity - offset) {
        setError(error, QStringLiteral("E_SHM_BOUNDS"));
        return false;
    }
#ifdef Q_OS_WIN
    memcpy(static_cast<char*>(m_impl->view) + offset, pixels.constData(),
           static_cast<size_t>(pixels.size()));
    MemoryBarrier();
    m_impl->offset = offset;
    m_impl->length = pixels.size();
    return true;
#else
    Q_UNUSED(pixels); Q_UNUSED(offset);
    setError(error, QStringLiteral("E_SHM_UNSUPPORTED"));
    return false;
#endif
}

QJsonObject SharedFrameMemory::poolEntry() const
{
    if (!isOpen()) return {};
    return QJsonObject{{QStringLiteral("slot_index"), m_impl->slotIndex},
                       {QStringLiteral("mapping_name"), m_impl->name},
                       {QStringLiteral("capacity_bytes"), m_impl->capacity}};
}

QJsonObject SharedFrameMemory::descriptor(const LeaseHandle& lease) const
{
    if (!isOpen() || lease.slotIndex != m_impl->slotIndex || lease.leaseId.isEmpty()
        || !kLeaseId.match(lease.leaseId).hasMatch() || lease.generation < 1
        || lease.generation > 9007199254740991LL
        || lease.byteLength < 1 || lease.byteLength != m_impl->length
        || m_impl->offset < 0 || m_impl->offset > m_impl->capacity
        || lease.byteLength > m_impl->capacity - m_impl->offset) return {};
    return QJsonObject{
        {QStringLiteral("transport"), QStringLiteral("win32_named_shared_memory")},
        {QStringLiteral("mapping_name"), m_impl->name},
        {QStringLiteral("lease_id"), lease.leaseId},
        {QStringLiteral("slot_index"), lease.slotIndex},
        {QStringLiteral("slot_generation"), lease.generation},
        {QStringLiteral("offset_bytes"), m_impl->offset},
        {QStringLiteral("byte_length"), lease.byteLength},
        {QStringLiteral("capacity_bytes"), m_impl->capacity}};
}

void SharedFrameMemory::close()
{
    if (!m_impl) return;
#ifdef Q_OS_WIN
    if (m_impl->view) { UnmapViewOfFile(m_impl->view); m_impl->view = nullptr; }
    if (m_impl->handle) { CloseHandle(m_impl->handle); m_impl->handle = nullptr; }
#endif
    m_impl->sessionId.clear(); m_impl->name.clear(); m_impl->slotIndex = -1;
    m_impl->capacity = 0; m_impl->offset = 0; m_impl->length = 0;
}

bool SharedFrameMemory::isOpen() const
{
#ifdef Q_OS_WIN
    return m_impl && m_impl->handle != nullptr && m_impl->view != nullptr;
#else
    return false;
#endif
}

ReadOnlyFrameMemory::ReadOnlyFrameMemory() : m_impl(new Impl) {}
ReadOnlyFrameMemory::~ReadOnlyFrameMemory() { close(); delete m_impl; }

bool ReadOnlyFrameMemory::open(const QString& sessionId, const QJsonObject& descriptor,
                               const QJsonArray& negotiatedPool, QString* error)
{
    setError(error, {});
    if (isOpen()) {
        setError(error, QStringLiteral("E_SHM_BUSY"));
        return false;
    }
    if (!validSession(sessionId, error)) return false;
    if (!exactKeys(descriptor, {QStringLiteral("transport"), QStringLiteral("mapping_name"),
                                QStringLiteral("lease_id"), QStringLiteral("slot_index"),
                                QStringLiteral("slot_generation"), QStringLiteral("offset_bytes"),
                                QStringLiteral("byte_length"), QStringLiteral("capacity_bytes")}, error)) return false;
    if (descriptor.value(QStringLiteral("transport")).toString() != QStringLiteral("win32_named_shared_memory")) {
        setError(error, QStringLiteral("E_SHM_OPEN")); return false;
    }
    qint64 slot = 0, generation = 0, offset = 0, length = 0, capacity = 0;
    if (!jsonInteger(descriptor.value(QStringLiteral("slot_index")), 0, 1, &slot, error)
        || !jsonInteger(descriptor.value(QStringLiteral("slot_generation")), 1, 9007199254740991LL, &generation, error)
        || !jsonInteger(descriptor.value(QStringLiteral("offset_bytes")), 0, kMaxCapacity, &offset, error)
        || !jsonInteger(descriptor.value(QStringLiteral("byte_length")), 1, kMaxCapacity, &length, error)
        || !jsonInteger(descriptor.value(QStringLiteral("capacity_bytes")), 4, kMaxCapacity, &capacity, error)) return false;
    const QString expectedName = mappingName(sessionId, static_cast<int>(slot));
    if (descriptor.value(QStringLiteral("mapping_name")).toString() != expectedName
        || !kLeaseId.match(descriptor.value(QStringLiteral("lease_id")).toString()).hasMatch()
        || offset > capacity || length > capacity - offset) {
        setError(error, QStringLiteral("E_SHM_BOUNDS")); return false;
    }
    if (negotiatedPool.size() != 2) { setError(error, QStringLiteral("E_SHM_OPEN")); return false; }
    QSet<int> seen;
    bool matched = false;
    for (const auto& value : negotiatedPool) {
        if (!value.isObject()) { setError(error, QStringLiteral("E_SHM_OPEN")); return false; }
        const auto entry = value.toObject();
        if (!exactKeys(entry, {QStringLiteral("slot_index"), QStringLiteral("mapping_name"),
                               QStringLiteral("capacity_bytes")}, error)) return false;
        qint64 entrySlot = 0, entryCapacity = 0;
        if (!jsonInteger(entry.value(QStringLiteral("slot_index")), 0, 1, &entrySlot, error)
            || !jsonInteger(entry.value(QStringLiteral("capacity_bytes")), 4, kMaxCapacity, &entryCapacity, error)
            || seen.contains(static_cast<int>(entrySlot))) {
            setError(error, QStringLiteral("E_SHM_OPEN")); return false;
        }
        seen.insert(static_cast<int>(entrySlot));
        const QString entryName = mappingName(sessionId, static_cast<int>(entrySlot));
        if (entry.value(QStringLiteral("mapping_name")).toString() != entryName) {
            setError(error, QStringLiteral("E_SHM_OPEN")); return false;
        }
        if (entrySlot == slot) {
            matched = true;
            if (entryCapacity != capacity) { setError(error, QStringLiteral("E_SHM_BOUNDS")); return false; }
        }
    }
    if (!matched || seen != QSet<int>{0, 1}) { setError(error, QStringLiteral("E_SHM_OPEN")); return false; }
#ifndef Q_OS_WIN
    setError(error, QStringLiteral("E_SHM_UNSUPPORTED"));
    return false;
#else
    const std::wstring wideName = expectedName.toStdWString();
    HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, wideName.c_str());
    if (!handle) { setError(error, winError(QStringLiteral("E_SHM_OPEN"))); return false; }
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
        const QString failure = winError(QStringLiteral("E_SHM_OPEN"));
        CloseHandle(handle);
        setError(error, failure);
        return false;
    }
    void* view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(capacity));
    if (!view) {
        const QString failure = winError(QStringLiteral("E_SHM_OPEN"));
        CloseHandle(handle); setError(error, failure); return false;
    }
    m_impl->handle = handle;
    m_impl->view = view;
    m_impl->name = expectedName;
    m_impl->capacity = capacity;
    m_impl->offset = offset;
    m_impl->length = length;
    Q_UNUSED(generation);
    return true;
#endif
}

QByteArray ReadOnlyFrameMemory::copyBytes(QString* error) const
{
    setError(error, {});
    if (!isOpen()) { setError(error, QStringLiteral("E_SHM_OPEN")); return {}; }
#ifdef Q_OS_WIN
    MemoryBarrier();
    return QByteArray(static_cast<const char*>(m_impl->view) + m_impl->offset,
                      static_cast<qsizetype>(m_impl->length));
#else
    setError(error, QStringLiteral("E_SHM_UNSUPPORTED"));
    return {};
#endif
}

void ReadOnlyFrameMemory::close()
{
    if (!m_impl) return;
#ifdef Q_OS_WIN
    if (m_impl->view) { UnmapViewOfFile(m_impl->view); m_impl->view = nullptr; }
    if (m_impl->handle) { CloseHandle(m_impl->handle); m_impl->handle = nullptr; }
#endif
    m_impl->name.clear(); m_impl->capacity = 0; m_impl->offset = 0; m_impl->length = 0;
}

bool ReadOnlyFrameMemory::isOpen() const
{
#ifdef Q_OS_WIN
    return m_impl && m_impl->handle != nullptr && m_impl->view != nullptr;
#else
    return false;
#endif
}

} // namespace relink::vision
