#include "business/matcher.h"
#include "business/validator.h"
#include "business/value_types.h"

#include <QCoreApplication>

#include <iostream>

using namespace relink::business;

namespace {
int failures = 0;
void check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    if (!ok) ++failures;
}

FieldEvidence observed(const QString& source = QStringLiteral("frame-1")) {
    FieldEvidence e;
    e.status = QStringLiteral("observed");
    e.source_ref = source;
    e.confidence = 0.99;
    return e;
}

DecimalValue dec(const QString& raw) {
    const auto parsed = parseDecimal(raw);
    return resultValue(parsed) ? *resultValue(parsed) : DecimalValue{};
}

TaskRule makeRule() {
    TaskRule r;
    r.task_id = QStringLiteral("task-1");
    r.revision = 1;
    r.name = QStringLiteral("synthetic rule");
    r.product_ref = QStringLiteral("product-1");
    r.enabled = true;
    r.filters.condition = Selector::eq(QStringLiteral("S"));
    r.price_range = PriceRange{Money{dec(QStringLiteral("230")), QStringLiteral("synthetic-unit")},
                               Money{dec(QStringLiteral("600")), QStringLiteral("synthetic-unit")}};
    r.max_wear = dec(QStringLiteral("0.399"));
    r.quantity_semantics = QStringLiteral("none");
    return r;
}

ListingObservation makeObservation(const QString& price = QStringLiteral("340")) {
    ListingObservation o;
    o.observation_id = QStringLiteral("obs-1");
    o.product_ref = QStringLiteral("product-1");
    o.market_listing_id = QStringLiteral("listing-1");
    o.association = {QStringLiteral("assoc-1"), QStringLiteral("confirmed")};
    o.price = Money{dec(price), QStringLiteral("synthetic-unit")};
    o.wear = dec(QStringLiteral("0.399"));
    o.condition = QStringLiteral("S");
    o.session_id = QStringLiteral("session-1");
    o.clock_domain_id = QStringLiteral("clock-1");
    o.frame_ref = QStringLiteral("frame-1");
    o.field_evidence.insert(QStringLiteral("product_ref"), observed());
    o.field_evidence.insert(QStringLiteral("price"), observed());
    o.field_evidence.insert(QStringLiteral("wear"), observed());
    o.field_evidence.insert(QStringLiteral("condition"), observed());
    for (const QString& key : {QStringLiteral("season"), QStringLiteral("ownership"), QStringLiteral("grade"),
                               QStringLiteral("rarity"), QStringLiteral("publicity")}) {
        FieldEvidence e = observed();
        e.status = QStringLiteral("missing");
        e.confidence.reset();
        e.raw.reset();
        o.field_evidence.insert(key, e);
    }
    return o;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const auto one = parseDecimal(QStringLiteral("1.187788"));
    const auto upper = parseDecimal(QStringLiteral("1.249"));
    check(isOk(one) && isOk(upper) && compareDecimal(*resultValue(one), *resultValue(upper)) == Ordering::Less,
          "decimal_exact_comparison");
    check(!isOk(parseDecimal(QStringLiteral("-1"))), "decimal_reject_negative");
    check(!isOk(parseDecimal(QStringLiteral("01"))), "decimal_reject_leading_zero");

    Catalog catalog;
    SkinCatalogEntry product;
    product.product_id = QStringLiteral("product-1");
    product.name = QStringLiteral("Synthetic");
    catalog.push_back(product);
    UnitDefinition unit;
    unit.unit_id = QStringLiteral("synthetic-unit");
    unit.display_name = QStringLiteral("Synthetic");
    unit.quantum = dec(QStringLiteral("1"));
    unit.reviewed = true;
    unit.synthetic = true;
    UnitCatalog units{unit};

    const TaskRule rule = makeRule();
    const auto validRule = validateRule(rule, catalog, units);
    check(isOk(validRule), "rule_validation");
    const auto validObs = validateObservation(makeObservation());
    check(isOk(validObs), "observation_validation");

    EvaluationContext context;
    context.session_id = QStringLiteral("session-1");
    context.clock_domain_id = QStringLiteral("clock-1");
    context.viewport_generation = 0;
    context.as_of_mono_ms = 1000;
    context.max_age_ms = 1000;
    context.known_units = {QStringLiteral("synthetic-unit")};

    if (isOk(validRule) && isOk(validObs)) {
        const Decision match = evaluate(*resultValue(validRule), *resultValue(validObs), context);
        check(match.status == QStringLiteral("Match") && match.primary_reason == QStringLiteral("MATCH") &&
                  !match.eligible_for_action,
              "matcher_match_not_action_authorized");

        ListingObservation expensive = makeObservation(QStringLiteral("601"));
        const auto expensiveValidated = validateObservation(expensive);
        check(isOk(expensiveValidated), "expensive_observation_validation");
        if (isOk(expensiveValidated)) {
            const Decision noMatch = evaluate(*resultValue(validRule), *resultValue(expensiveValidated), context);
            check(noMatch.status == QStringLiteral("NoMatch") && noMatch.primary_reason == QStringLiteral("PRICE_ABOVE_MAX"),
                  "matcher_price_upper_bound");
        }

        ListingObservation missingWear = makeObservation();
        missingWear.wear.reset();
        missingWear.field_evidence[QStringLiteral("wear")].status = QStringLiteral("missing");
        const auto missingValidated = validateObservation(missingWear);
        check(isOk(missingValidated), "missing_wear_observation_validation");
        if (isOk(missingValidated)) {
            const Decision review = evaluate(*resultValue(validRule), *resultValue(missingValidated), context);
            check(review.status == QStringLiteral("NeedsReview") && review.primary_reason == QStringLiteral("FIELD_MISSING"),
                  "review_precedes_known_mismatch");
        }
    }

    return failures == 0 ? 0 : 1;
}
