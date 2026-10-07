#pragma once
#include "application/vision/catalog_filter_reader.h"
#include <QJsonArray>
#include <QSet>

namespace relink::runtime {
// Explicit UI-state target, not a task aggregator. Callers must resolve the
// original batch grouping and dictionary before creating this snapshot.
// Ownership has two independent boxes; grade is a multi-select set.
struct CatalogFilterTarget {
    QString snapshotId;
    QString seasonLabel;
    bool owned = false;
    bool unowned = false;
    QSet<QString> grades;
};
struct CatalogFilterPlan {
    bool valid = false;
    bool alreadyMatches = false;
    QString reason;
    QString snapshotId;
    QString sourceFrameId;
    QJsonArray steps;
    QJsonObject toJson() const;
};
// Declarative read-only difference. Each change requires a fresh readback;
// this module neither owns coordinates nor executes/authorizes game input.
CatalogFilterPlan planCatalogFilter(const vision::CatalogFilterState& observed,
    const CatalogFilterTarget& target);
} // namespace relink::runtime
