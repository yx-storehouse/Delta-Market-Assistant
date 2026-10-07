#pragma once

#include "application/vision/lease_pool.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace relink::vision {

// Owner-side RAII view over a paging-file-backed named shared-memory section.
// This class owns only the mapping and bytes; LeasePool remains the lifecycle
// authority for slot/generation state. No filesystem/image path transport is
// involved.
class SharedFrameMemory final {
public:
    SharedFrameMemory();
    ~SharedFrameMemory();
    SharedFrameMemory(const SharedFrameMemory&) = delete;
    SharedFrameMemory& operator=(const SharedFrameMemory&) = delete;

    // An open mapping requires explicit close after the caller drains leases.
    bool create(const QString& sessionId, int slotIndex, qint64 capacityBytes,
                QString* error = nullptr);
    bool write(const QByteArray& pixels, qint64 offset = 0,
               QString* error = nullptr);
    QJsonObject poolEntry() const;
    QJsonObject descriptor(const LeaseHandle& lease) const;
    void close();
    bool isOpen() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// Reader-side read-only RAII view. It validates the descriptor against the
// negotiated two-slot pool before opening any mapping and copies only the
// declared byte range into a QByteArray.
class ReadOnlyFrameMemory final {
public:
    ReadOnlyFrameMemory();
    ~ReadOnlyFrameMemory();
    ReadOnlyFrameMemory(const ReadOnlyFrameMemory&) = delete;
    ReadOnlyFrameMemory& operator=(const ReadOnlyFrameMemory&) = delete;

    // An open reader requires explicit close before it can open another lease.
    bool open(const QString& sessionId, const QJsonObject& descriptor,
              const QJsonArray& negotiatedPool, QString* error = nullptr);
    QByteArray copyBytes(QString* error = nullptr) const;
    void close();
    bool isOpen() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace relink::vision
