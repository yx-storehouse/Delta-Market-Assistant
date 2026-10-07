#pragma once
#include <QByteArray>
#include <QJsonObject>

namespace relink::config {
// Data-only bounded decoder for the observed .savedValue subset. No object
// construction hooks, eval, plugins, specimen runtime or source-file writes.
QJsonObject decodeSavedValue(const QByteArray& bytes);
// Resolves explicit UI IDs against a versioned, source-evidenced dictionary.
// Disabled rows stay disabled, unknown IDs block readiness, and purchase
// settings remain raw metadata rather than triggering the collection runner.
QJsonObject savedValueCollectionSnapshot(const QByteArray& bytes,const QJsonObject& dictionary);
} // namespace relink::config
