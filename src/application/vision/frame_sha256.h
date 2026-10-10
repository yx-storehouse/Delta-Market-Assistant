#pragma once
#include <QByteArray>
#include <QString>

namespace relink::vision {
struct FrameDigest {
    QByteArray bytes;
    QString provider;
    qint64 elapsedNs = 0;
};
// Same SHA256 over every input byte, not a perceptual/partial hash or cache.
// Windows uses CNG; provider failure keeps the original Qt implementation.
FrameDigest frameSha256(const QByteArray& data);
}
