#pragma once

#include "value_types.h"

#include <QHash>
#include <QMap>
#include <QVariantMap>
#include <optional>

namespace relink::business {

using Id = QString;

struct Money {
    DecimalValue value;
    // Empty QString represents JSON null. IDs are non-empty when present.
    QString unit;
    bool hasUnit() const { return !unit.isEmpty(); }
};

struct RuleRef {
    QString task_id;
    qint32 revision = 0;
    bool isValid() const { return !task_id.isEmpty() && revision > 0; }
};

struct Selector {
    QString op = QStringLiteral("any");
    QString value;

    static Selector any() { return {}; }
    static Selector eq(const QString& v) { return {QStringLiteral("eq"), v}; }
    bool isAny() const { return op == QStringLiteral("any"); }
    bool isEq() const { return op == QStringLiteral("eq"); }
};

using ConditionSelector = Selector;
using OwnershipSelector = Selector;
using PublicitySelector = Selector;

struct FieldEvidence {
    QString status = QStringLiteral("missing");
    std::optional<QString> raw;
    std::optional<double> confidence;
    QString source_ref;
};

struct Association {
    QString association_ref;
    QString status = QStringLiteral("unknown");
};

struct SkinCatalogEntry {
    QString product_id;
    QString name;
    std::optional<QString> series;
    std::optional<QString> season;
    std::optional<QString> grade;
    std::optional<QString> rarity;
    QVector<QString> aliases;
    QString source_kind;
    QVariantMap extensions;
};

struct ListingObservation {
    QString observation_id;
    std::optional<QString> product_ref;
    std::optional<QString> market_listing_id;
    Association association;
    std::optional<Money> price;
    std::optional<DecimalValue> wear;
    std::optional<QString> condition;
    std::optional<QString> season;
    std::optional<QString> ownership;
    std::optional<QString> grade;
    std::optional<QString> rarity;
    std::optional<QString> publicity;
    QString frame_ref;
    QString session_id;
    QString clock_domain_id;
    quint64 observed_mono_ms = 0;
    quint64 viewport_generation = 0;
    QMap<QString, FieldEvidence> field_evidence;
    QVariantMap extensions;

    const FieldEvidence* evidence(const QString& field) const {
        auto it = field_evidence.constFind(field);
        return it == field_evidence.constEnd() ? nullptr : &it.value();
    }
};

struct PriceRange {
    Money min;
    Money max;
};

struct TaskFilters {
    Selector season;
    Selector ownership;
    Selector grade;
    Selector condition;
    Selector publicity;
    Selector rarity;
};

struct LegacyRaw {
    QString source_kind;
    QString source_sha256;
    std::optional<int> source_line;
    QString raw_line;
    QVector<QString> columns;
    QVariantMap unknown_json;
};

struct TaskRule {
    QString task_id;
    qint32 revision = 0;
    QString name;
    std::optional<QString> product_ref;
    bool enabled = false;
    bool review_required = false;
    QVector<QString> review_codes;
    TaskFilters filters;
    std::optional<PriceRange> price_range;
    std::optional<DecimalValue> max_wear;
    QString requested_sort = QStringLiteral("default");
    std::optional<int> quantity_candidate;
    QString quantity_semantics = QStringLiteral("none");
    std::optional<LegacyRaw> legacy_raw;
    QVariantMap extensions;
};

struct UnitDefinition {
    QString unit_id;
    QString display_name;
    DecimalValue quantum;
    bool reviewed = false;
    bool synthetic = false;
};

using Catalog = QVector<SkinCatalogEntry>;
using UnitCatalog = QVector<UnitDefinition>;

struct EvaluationContext {
    QString session_id;
    QString clock_domain_id;
    quint64 viewport_generation = 0;
    quint64 as_of_mono_ms = 0;
    quint64 max_age_ms = 0;
    QString purpose = QStringLiteral("filter_only");
    QVector<QString> known_units;
};

struct ValidatedRule { TaskRule rule; };
struct ValidatedObservation { ListingObservation observation; };

struct DecisionReason {
    QString code;
    QString field;
    std::optional<QString> evidence_ref;
};

struct Decision {
    QString status;
    QString primary_reason;
    QVector<DecisionReason> reasons;
    RuleRef rule_ref;
    QString observation_id;
    bool eligible_for_action = false;
};

} // namespace relink::business

