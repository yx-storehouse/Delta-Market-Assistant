#include "matcher.h"

#include <algorithm>

namespace relink::business {
namespace {

Decision makeDecision(const TaskRule& rule, const ListingObservation& observation,
                      const QString& status, const QString& code, const QString& field = {},
                      bool evidence = false) {
    Decision result;
    result.status = status;
    result.primary_reason = code;
    result.rule_ref = {rule.task_id, rule.revision};
    result.observation_id = observation.observation_id;
    result.eligible_for_action = false;
    result.reasons.push_back({code, field, evidence ? std::optional<QString>(observation.frame_ref) : std::nullopt});
    return result;
}

const FieldEvidence* evidence(const ListingObservation& observation, const QString& field) {
    return observation.evidence(field);
}

bool isEq(const Selector& selector) { return selector.op == QStringLiteral("eq"); }

std::optional<QString> fieldValue(const ListingObservation& observation, const QString& field) {
    if (field == QStringLiteral("product_ref")) return observation.product_ref;
    if (field == QStringLiteral("condition")) return observation.condition;
    if (field == QStringLiteral("season")) return observation.season;
    if (field == QStringLiteral("ownership")) return observation.ownership;
    if (field == QStringLiteral("grade")) return observation.grade;
    if (field == QStringLiteral("rarity")) return observation.rarity;
    if (field == QStringLiteral("publicity")) return observation.publicity;
    return std::nullopt;
}

Decision reviewForField(const TaskRule& rule, const ListingObservation& observation, const QString& field) {
    const auto* e = evidence(observation, field);
    if (!e || e->status == QStringLiteral("missing")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), field, true);
    if (e->status == QStringLiteral("invalid")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_INVALID"), field, true);
    if (e->status == QStringLiteral("ambiguous")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_AMBIGUOUS"), field, true);
    const auto value = fieldValue(observation, field);
    if (!value) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), field, true);
    return {};
}

Decision reviewForRequiredPrice(const TaskRule& rule, const ListingObservation& observation) {
    const auto* e = evidence(observation, QStringLiteral("price"));
    if (!e || e->status == QStringLiteral("missing")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), QStringLiteral("price"), true);
    if (e->status == QStringLiteral("invalid")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_INVALID"), QStringLiteral("price"), true);
    if (e->status == QStringLiteral("ambiguous")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_AMBIGUOUS"), QStringLiteral("price"), true);
    if (!observation.price) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), QStringLiteral("price"), true);
    return {};
}

Decision reviewForRequiredWear(const TaskRule& rule, const ListingObservation& observation) {
    const auto* e = evidence(observation, QStringLiteral("wear"));
    if (!e || e->status == QStringLiteral("missing")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), QStringLiteral("wear"), true);
    if (e->status == QStringLiteral("invalid")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_INVALID"), QStringLiteral("wear"), true);
    if (e->status == QStringLiteral("ambiguous")) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_AMBIGUOUS"), QStringLiteral("wear"), true);
    if (!observation.wear) return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("FIELD_MISSING"), QStringLiteral("wear"), true);
    return {};
}

bool unitKnown(const EvaluationContext& context, const QString& unit) {
    return !unit.isEmpty() && context.known_units.contains(unit);
}

Decision emptyDecision() { return {}; }

} // namespace

Decision evaluate(const ValidatedRule& validatedRule,
                  const ValidatedObservation& validatedObservation,
                  const EvaluationContext& context) {
    const TaskRule& rule = validatedRule.rule;
    const ListingObservation& observation = validatedObservation.observation;

    // V0 has already run at the validation boundary. The matcher retains a defensive
    // invalid result for callers that manufacture a Validated* object by hand.
    if (rule.task_id.isEmpty() || observation.observation_id.isEmpty())
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("RULE_INVALID"));

    // P1 / P2 are intentionally before all observation checks.
    if (rule.review_required)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("RULE_REVIEW_REQUIRED"));
    if (!rule.enabled)
        return makeDecision(rule, observation, QStringLiteral("NoMatch"), QStringLiteral("RULE_DISABLED"));

    // P3: context and freshness. Do not repair a mismatch using wall-clock time.
    if (observation.session_id != context.session_id)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("CONTEXT_MISMATCH"), QStringLiteral("session_id"), true);
    if (observation.clock_domain_id != context.clock_domain_id)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("CONTEXT_MISMATCH"), QStringLiteral("clock_domain_id"), true);
    if (observation.viewport_generation != context.viewport_generation)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("CONTEXT_MISMATCH"), QStringLiteral("viewport_generation"), true);
    if (observation.observed_mono_ms > context.as_of_mono_ms)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("OBSERVATION_FUTURE"), QStringLiteral("observed_mono_ms"), true);
    if (context.as_of_mono_ms - observation.observed_mono_ms > context.max_age_ms)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("OBSERVATION_STALE"), QStringLiteral("observed_mono_ms"), true);

    // P4: association and product identity are not guessed from a label or position.
    if (!observation.product_ref)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("PRODUCT_UNRESOLVED"), QStringLiteral("product_ref"), true);
    if (observation.association.status == QStringLiteral("unknown"))
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("ASSOCIATION_UNKNOWN"), QStringLiteral("association"), true);
    if (observation.association.status == QStringLiteral("ambiguous"))
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("ASSOCIATION_AMBIGUOUS"), QStringLiteral("association"), true);
    if (context.purpose == QStringLiteral("intent_candidate") && observation.association.status == QStringLiteral("provisional"))
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("ASSOCIATION_PROVISIONAL"), QStringLiteral("association"), true);

    // P5: required evidence is checked before any known mismatch. An `any` selector
    // deliberately permits an unknown optional field.
    if (auto d = reviewForRequiredPrice(rule, observation); !d.primary_reason.isEmpty()) return d;
    if (rule.max_wear) {
        auto d = reviewForRequiredWear(rule, observation);
        if (!d.primary_reason.isEmpty()) return d;
    }
    const QVector<QPair<QString, const Selector*>> selectors = {
        {QStringLiteral("season"), &rule.filters.season},
        {QStringLiteral("ownership"), &rule.filters.ownership},
        {QStringLiteral("grade"), &rule.filters.grade},
        {QStringLiteral("condition"), &rule.filters.condition},
        {QStringLiteral("publicity"), &rule.filters.publicity},
        {QStringLiteral("rarity"), &rule.filters.rarity}
    };
    for (const auto& pair : selectors) {
        if (!isEq(*pair.second)) continue;
        if (auto d = reviewForField(rule, observation, pair.first); !d.primary_reason.isEmpty()) return d;
    }

    const auto& price = *observation.price;
    const auto& range = *rule.price_range;
    if (!unitKnown(context, price.unit) || !unitKnown(context, range.min.unit))
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("UNIT_UNKNOWN"), QStringLiteral("price"), true);
    if (price.unit != range.min.unit || price.unit != range.max.unit)
        return makeDecision(rule, observation, QStringLiteral("NeedsReview"), QStringLiteral("UNIT_MISMATCH"), QStringLiteral("price"), true);

    // P6: deterministic mismatch order follows the rule contract.
    if (rule.product_ref && *rule.product_ref != *observation.product_ref)
        return makeDecision(rule, observation, QStringLiteral("NoMatch"), QStringLiteral("PRODUCT_MISMATCH"), QStringLiteral("product_ref"), true);
    const QPair<QString, std::optional<QString>> values[] = {
        {QStringLiteral("season"), observation.season}, {QStringLiteral("ownership"), observation.ownership},
        {QStringLiteral("grade"), observation.grade}, {QStringLiteral("condition"), observation.condition},
        {QStringLiteral("publicity"), observation.publicity}, {QStringLiteral("rarity"), observation.rarity}
    };
    const QString mismatchCodes[] = {QStringLiteral("SEASON_MISMATCH"), QStringLiteral("OWNERSHIP_MISMATCH"), QStringLiteral("GRADE_MISMATCH"), QStringLiteral("CONDITION_MISMATCH"), QStringLiteral("PUBLICITY_MISMATCH"), QStringLiteral("RARITY_MISMATCH")};
    const Selector* selectorValues[] = {&rule.filters.season, &rule.filters.ownership, &rule.filters.grade, &rule.filters.condition, &rule.filters.publicity, &rule.filters.rarity};
    for (int i = 0; i < 6; ++i) {
        if (selectorValues[i]->isEq() && values[i].second && *values[i].second != selectorValues[i]->value)
            return makeDecision(rule, observation, QStringLiteral("NoMatch"), mismatchCodes[i], values[i].first, true);
    }
    const auto minCmp = compareDecimal(price.value, range.min.value);
    if (minCmp == Ordering::Less)
        return makeDecision(rule, observation, QStringLiteral("NoMatch"), QStringLiteral("PRICE_BELOW_MIN"), QStringLiteral("price"), true);
    const auto maxCmp = compareDecimal(price.value, range.max.value);
    if (maxCmp == Ordering::Greater)
        return makeDecision(rule, observation, QStringLiteral("NoMatch"), QStringLiteral("PRICE_ABOVE_MAX"), QStringLiteral("price"), true);
    if (rule.max_wear && compareDecimal(*observation.wear, *rule.max_wear) == Ordering::Greater)
        return makeDecision(rule, observation, QStringLiteral("NoMatch"), QStringLiteral("WEAR_ABOVE_MAX"), QStringLiteral("wear"), true);
    return makeDecision(rule, observation, QStringLiteral("Match"), QStringLiteral("MATCH"));
}

} // namespace relink::business
