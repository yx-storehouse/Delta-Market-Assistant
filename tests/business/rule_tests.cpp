#include "../../src/business/matcher.h"

#include <QCoreApplication>
#include <QDebug>

using namespace relink::business;

namespace {
FieldEvidence observed(const QString& source) { return {QStringLiteral("observed"), std::nullopt, std::nullopt, source}; }
FieldEvidence missing(const QString& source) { return {QStringLiteral("missing"), std::nullopt, std::nullopt, source}; }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    TaskRule rule;
    rule.task_id = QStringLiteral("task-01"); rule.revision = 1; rule.name = QStringLiteral("demo");
    rule.product_ref = QStringLiteral("product-01"); rule.enabled = true;
    rule.filters.condition = Selector::eq(QStringLiteral("S"));
    rule.price_range = PriceRange{{DecimalValue{"230", 0}, QStringLiteral("CREDIT")}, {DecimalValue{"600", 0}, QStringLiteral("CREDIT")}};
    rule.max_wear = DecimalValue{"399", 3};
    rule.quantity_semantics = QStringLiteral("none");
    UnitCatalog units{{QStringLiteral("CREDIT"), QStringLiteral("credit"), DecimalValue{"1", 0}, true, true}};
    auto vr = validateRule(rule, {}, units);
    Q_ASSERT(isOk(vr));
    ListingObservation obs;
    obs.observation_id = QStringLiteral("obs-01"); obs.product_ref = QStringLiteral("product-01");
    obs.association = {QStringLiteral("assoc-01"), QStringLiteral("confirmed")};
    obs.price = Money{DecimalValue{"60000", 2}, QStringLiteral("CREDIT")};
    obs.wear = DecimalValue{"3", 1}; obs.condition = QStringLiteral("S");
    obs.frame_ref = QStringLiteral("frame-01"); obs.session_id = QStringLiteral("session-01"); obs.clock_domain_id = QStringLiteral("clock-01");
    obs.observed_mono_ms = 1000; obs.viewport_generation = 1;
    for (const auto& field : {QStringLiteral("product_ref"), QStringLiteral("price"), QStringLiteral("wear"), QStringLiteral("condition")}) obs.field_evidence.insert(field, observed(obs.frame_ref));
    for (const auto& field : {QStringLiteral("season"), QStringLiteral("ownership"), QStringLiteral("grade"), QStringLiteral("rarity"), QStringLiteral("publicity")}) obs.field_evidence.insert(field, missing(obs.frame_ref));
    auto vo = validateObservation(obs);
    Q_ASSERT(isOk(vo));
    EvaluationContext context{QStringLiteral("session-01"), QStringLiteral("clock-01"), 1, 1100, 1000, QStringLiteral("filter_only"), {QStringLiteral("CREDIT")}};
    const auto decision = evaluate(std::get<ValidatedRule>(vr), std::get<ValidatedObservation>(vo), context);
    Q_ASSERT(decision.status == QStringLiteral("Match"));
    Q_ASSERT(!decision.eligible_for_action);
    qInfo() << "rule_tests: PASS";
    return 0;
}

