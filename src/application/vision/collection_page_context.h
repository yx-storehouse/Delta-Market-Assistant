#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include "target_window.h"
#include <QJsonObject>
#include <QList>
#include <QPair>

namespace relink::vision {
// Server-owned continuation of a fully classified listing page. It holds
// hashes and page/control evidence only, never old pixels, card geometry,
// prices, wear, title words, star state or transient success/capacity flags.
// A miss requires full classification of the SAME newly acquired frame.
class CollectionPageContext final {
public:
    static QList<QPair<QString, QRect>> guardRegions();
    void clear();
    bool remember(const runtime::observation::FrameEnvelope& frame,
                  const TargetWindow& target, const QJsonObject& packet);
    QJsonObject check(const runtime::observation::FrameEnvelope& frame,
                      const TargetWindow& target, const QString& digest) const;
    QJsonObject page(const QJsonObject& proof) const;
    QJsonObject controls(const QJsonObject& proof) const;
private:
    bool m_valid = false;
    TargetWindow m_target;
    QString m_frameId, m_frameSha;
    qint64 m_sourceMs = -1;
    QList<QByteArray> m_hashes;
    QJsonObject m_page, m_controls;
    mutable QJsonObject m_issuedProof;
};
}
