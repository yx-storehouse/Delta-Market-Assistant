#include "validator.h"

#include <QRegularExpression>

namespace relink::business {
namespace {

Diagnostic diag(const QString& code, const QString& field, const QString& message) {
    return {code, field, message, 0, 0};
}

bool idValid(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
    return re.match(id).hasMatch();
}

bool tokenValid(const std::optional<QString>& value, int maxLength = 128) {
    return !value || (!value->isEmpty() && value->size() <= maxLength);
}

bool selectorValid(const Selector& selector, int maxLength = 128) {
    if (selector.op == QStringLiteral("any")) return selector.value.isEmpty();
    return selector.op == QStringLiteral("eq") && !selector.value.isEmpty() && selector.value.size() <= maxLength;
}

bool conditionSelectorValid(const Selector& selector) {
    if (!selectorValid(selector, 1)) return false;
    return selector.isAny() || selector.value == QStringLiteral("S") || selector.value == QStringLiteral("A") ||
           selector.value == QStringLiteral("B") || selector.value == QStringLiteral("C");
}

bool ownershipSelectorValid(const Selector& selector) {
    if (!selectorValid(selector, 8)) return false;
    return selector.isAny() || selector.value == QStringLiteral("owned") || selector.value == QStringLiteral("unowned");
}

bool publicitySelectorValid(const Selector& selector) {
    if (!selectorValid(selector, 6)) return false;
    return selector.isAny() || selector.value == QStringLiteral("active") || selector.value == QStringLiteral("ended") ||
           selector.value == QStringLiteral("none");
}

const UnitDefinition* findUnit(const UnitCatalog& units, const QString& id) {
    for (const auto& unit : units)
        if (unit.unit_id == id) return &unit;
    return nullptr;
}

bool moneyValid(const Money& money, const UnitCatalog& units, const QString& field, QVector<Diagnostic>& errors) {
    if (!money.value.isStructurallyValid()) {
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), field, QStringLiteral("invalid decimal value")));
        return false;
    }
    if (money.unit.isEmpty() || !idValid(money.unit)) {
        errors.push_back(diag(QStringLiteral("UNIT_UNKNOWN"), field, QStringLiteral("money unit is missing or invalid")));
        return false;
    }
    if (!units.isEmpty()) {
        const auto* unit = findUnit(units, money.unit);
        if (!unit) {
            errors.push_back(diag(QStringLiteral("UNIT_UNKNOWN"), field, QStringLiteral("money unit is not registered")));
            return false;
        }
        if (!unit->reviewed || !unit->quantum.isStructurallyValid() || !decimalIsPositive(unit->quantum)) {
            errors.push_back(diag(QStringLiteral("UNIT_INVALID"), field, QStringLiteral("money unit is not reviewed")));
            return false;
        }
        if (!decimalIsMultipleOf(money.value, unit->quantum))
            errors.push_back(diag(QStringLiteral("UNIT_QUANTUM_MISMATCH"), field, QStringLiteral("money is not a quantum multiple")));
    }
    return errors.isEmpty();
}

bool commonDecimal(const std::optional<DecimalValue>& value, const QString& field, QVector<Diagnostic>& errors) {
    if (value && !value->isStructurallyValid()) {
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), field, QStringLiteral("invalid decimal value")));
        return false;
    }
    return true;
}

bool evidenceStatusValid(const QString& status) {
    return status == QStringLiteral("observed") || status == QStringLiteral("missing") ||
           status == QStringLiteral("invalid") || status == QStringLiteral("ambiguous");
}

} // namespace

Result<ValidatedRule> validateRule(const TaskRule& rule, const Catalog& catalog, const UnitCatalog& units) {
    QVector<Diagnostic> errors;
    if (!idValid(rule.task_id)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("task_id"), QStringLiteral("invalid task id")));
    if (rule.revision < 1) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("revision"), QStringLiteral("revision must be positive")));
    if (rule.name.isEmpty() || rule.name.size() > 60) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("name"), QStringLiteral("name must be 1..60 UTF-16 units")));
    if (rule.product_ref && !idValid(*rule.product_ref)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("product_ref"), QStringLiteral("invalid product id")));
    if (!selectorValid(rule.filters.season) || !selectorValid(rule.filters.grade) || !selectorValid(rule.filters.rarity))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("filters"), QStringLiteral("invalid selector")));
    if (!ownershipSelectorValid(rule.filters.ownership) || !conditionSelectorValid(rule.filters.condition) ||
        !publicitySelectorValid(rule.filters.publicity))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("filters"), QStringLiteral("invalid constrained selector")));
    if (rule.review_required) {
        if (rule.enabled) errors.push_back(diag(QStringLiteral("RULE_REVIEW_REQUIRED"), QStringLiteral("enabled"), QStringLiteral("review rule must be disabled")));
        if (rule.review_codes.isEmpty()) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("review_codes"), QStringLiteral("review rule needs a reason code")));
    } else {
        if (!rule.review_codes.isEmpty()) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("review_codes"), QStringLiteral("non-review rule cannot carry review codes")));
        if (!rule.product_ref) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("product_ref"), QStringLiteral("active rule requires a product")));
        if (!rule.price_range) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("price_range"), QStringLiteral("active rule requires a price range")));
    }
    if (rule.quantity_candidate && (*rule.quantity_candidate < 1 || *rule.quantity_candidate > 9999))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("quantity_candidate"), QStringLiteral("quantity is outside 1..9999")));
    if (!rule.quantity_candidate && rule.quantity_semantics != QStringLiteral("none"))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("quantity_semantics"), QStringLiteral("null quantity requires none semantics")));
    if (rule.quantity_candidate && rule.quantity_semantics == QStringLiteral("none"))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("quantity_semantics"), QStringLiteral("non-null quantity needs explicit semantics")));
    if (!rule.max_wear || commonDecimal(rule.max_wear, QStringLiteral("max_wear"), errors)) {
        if (rule.max_wear && rule.max_wear->isZero()) {
            // Zero is a valid boundary; positivity is not required for wear.
        }
    }

    if (!catalog.isEmpty() && rule.product_ref) {
        bool found = false;
        for (const auto& entry : catalog) if (entry.product_id == *rule.product_ref) { found = true; break; }
        if (!found) errors.push_back(diag(QStringLiteral("PRODUCT_UNRESOLVED"), QStringLiteral("product_ref"), QStringLiteral("product is absent from catalog")));
    }

    if (rule.price_range) {
        const auto& range = *rule.price_range;
        const int old = errors.size();
        moneyValid(range.min, units, QStringLiteral("price_range.min"), errors);
        moneyValid(range.max, units, QStringLiteral("price_range.max"), errors);
        if (range.min.unit != range.max.unit)
            errors.push_back(diag(QStringLiteral("UNIT_MISMATCH"), QStringLiteral("price_range"), QStringLiteral("price bounds require one unit")));
        if (range.min.unit == range.max.unit && compareDecimal(range.min.value, range.max.value) == Ordering::Greater)
            errors.push_back(diag(QStringLiteral("PRICE_RANGE_REVERSED"), QStringLiteral("price_range"), QStringLiteral("minimum exceeds maximum")));
        Q_UNUSED(old);
    }
    if (!errors.isEmpty()) return errors;
    return ValidatedRule{rule};
}

Result<ValidatedObservation> validateObservation(const ListingObservation& observation) {
    QVector<Diagnostic> errors;
    if (!idValid(observation.observation_id) || !idValid(observation.frame_ref) ||
        !idValid(observation.session_id) || !idValid(observation.clock_domain_id))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("observation_id"), QStringLiteral("invalid observation identity")));
    if (observation.product_ref && !idValid(*observation.product_ref)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("product_ref"), QStringLiteral("invalid product id")));
    if (observation.market_listing_id && observation.market_listing_id->isEmpty()) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("market_listing_id"), QStringLiteral("listing id cannot be empty")));
    if (observation.association.status != QStringLiteral("confirmed") && observation.association.status != QStringLiteral("provisional") &&
        observation.association.status != QStringLiteral("ambiguous") && observation.association.status != QStringLiteral("unknown"))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("association.status"), QStringLiteral("invalid association status")));
    if ((observation.association.status == QStringLiteral("confirmed") || observation.association.status == QStringLiteral("provisional")) &&
        !idValid(observation.association.association_ref))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("association.association_ref"), QStringLiteral("confirmed association needs an id")));
    if (observation.association.status == QStringLiteral("unknown") && !observation.association.association_ref.isEmpty())
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("association.association_ref"), QStringLiteral("unknown association cannot carry an id")));
    if (observation.price && !observation.price->value.isStructurallyValid()) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("price"), QStringLiteral("invalid price decimal")));
    if (observation.price && !observation.price->unit.isEmpty() && !idValid(observation.price->unit)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("price.unit"), QStringLiteral("invalid unit id")));
    commonDecimal(observation.wear, QStringLiteral("wear"), errors);
    const QStringList optionalFields = {QStringLiteral("condition"), QStringLiteral("ownership"), QStringLiteral("publicity"), QStringLiteral("season"), QStringLiteral("grade"), QStringLiteral("rarity")};
    if (observation.condition && (*observation.condition != QStringLiteral("S") && *observation.condition != QStringLiteral("A") && *observation.condition != QStringLiteral("B") && *observation.condition != QStringLiteral("C")))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("condition"), QStringLiteral("invalid condition")));
    if (observation.ownership && (*observation.ownership != QStringLiteral("owned") && *observation.ownership != QStringLiteral("unowned")))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("ownership"), QStringLiteral("invalid ownership")));
    if (observation.publicity && (*observation.publicity != QStringLiteral("active") && *observation.publicity != QStringLiteral("ended") && *observation.publicity != QStringLiteral("none")))
        errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("publicity"), QStringLiteral("invalid publicity")));
    for (const QString& field : {QStringLiteral("product_ref"), QStringLiteral("price"), QStringLiteral("wear"), QStringLiteral("condition"), QStringLiteral("season"), QStringLiteral("ownership"), QStringLiteral("grade"), QStringLiteral("rarity"), QStringLiteral("publicity")}) {
        const auto* e = observation.evidence(field);
        if (!e || !evidenceStatusValid(e->status) || !idValid(e ? e->source_ref : QString{})) {
            errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), field, QStringLiteral("missing or invalid field evidence")));
            continue;
        }
        if (e->confidence && (*e->confidence < 0.0 || *e->confidence > 1.0)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), field, QStringLiteral("confidence is outside 0..1")));
        if (e->status == QStringLiteral("observed")) {
            bool present = false;
            if (field == QStringLiteral("product_ref")) present = observation.product_ref.has_value();
            else if (field == QStringLiteral("price")) present = observation.price.has_value();
            else if (field == QStringLiteral("wear")) present = observation.wear.has_value();
            else if (field == QStringLiteral("condition")) present = observation.condition.has_value();
            else if (field == QStringLiteral("season")) present = observation.season.has_value();
            else if (field == QStringLiteral("ownership")) present = observation.ownership.has_value();
            else if (field == QStringLiteral("grade")) present = observation.grade.has_value();
            else if (field == QStringLiteral("rarity")) present = observation.rarity.has_value();
            else if (field == QStringLiteral("publicity")) present = observation.publicity.has_value();
            if (!present) errors.push_back(diag(QStringLiteral("EVIDENCE_VALUE_CONFLICT"), field, QStringLiteral("observed evidence has no value")));
        }
        if (e->status == QStringLiteral("missing") && e->raw) errors.push_back(diag(QStringLiteral("EVIDENCE_VALUE_CONFLICT"), field, QStringLiteral("missing evidence cannot carry raw text")));
    }
    Q_UNUSED(optionalFields);
    if (!errors.isEmpty()) return errors;
    return ValidatedObservation{observation};
}

Result<EvaluationContext> validateEvaluationContext(const EvaluationContext& context) {
    QVector<Diagnostic> errors;
    if (!idValid(context.session_id) || !idValid(context.clock_domain_id)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("context"), QStringLiteral("invalid context identity")));
    if (context.max_age_ms > 60000) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("max_age_ms"), QStringLiteral("max age exceeds 60000ms")));
    if (context.purpose != QStringLiteral("filter_only") && context.purpose != QStringLiteral("intent_candidate")) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("purpose"), QStringLiteral("invalid evaluation purpose")));
    for (const auto& unit : context.known_units) if (!idValid(unit)) errors.push_back(diag(QStringLiteral("SCHEMA_INVALID"), QStringLiteral("known_units"), QStringLiteral("invalid unit id")));
    if (!errors.isEmpty()) return errors;
    return context;
}

} // namespace relink::business

