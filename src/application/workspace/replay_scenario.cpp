#include "replay_scenario.h"

#include "business/matcher.h"
#include "business/value_types.h"
#include <QJsonArray>

namespace relink::workspace {
namespace {
QString s(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
business::DecimalValue decimal(const QJsonObject& o) { return {s(o, "unscaled"), quint8(o.value(QStringLiteral("scale")).toInt())}; }
business::Money money(const QJsonObject& o) { return {decimal(o.value(QStringLiteral("value")).toObject()), s(o, "unit")}; }
QString priceText(const business::DecimalValue& value)
{
    QString result = value.unscaled;
    while (result.size() <= value.scale) result.prepend(QLatin1Char('0'));
    if (value.scale) result.insert(result.size() - value.scale, QLatin1Char('.'));
    return result;
}
business::Selector selector(const QJsonObject& o)
{
    return {o.value(QStringLiteral("op")).toString(QStringLiteral("any")), s(o, "value")};
}
}

business::TaskRule ReplayScenario::fixtureRule()
{
    business::TaskRule rule;
    rule.task_id = QStringLiteral("fixture-rule"); rule.revision = 1;
    rule.name = QStringLiteral("Synthetic price range"); rule.product_ref = QStringLiteral("synthetic-product-1");
    rule.enabled = true;
    rule.price_range = business::PriceRange{{{QStringLiteral("100"), 0}, QStringLiteral("synthetic-unit")},
                                          {{QStringLiteral("200"), 0}, QStringLiteral("synthetic-unit")}};
    return rule;
}

business::TaskRule ReplayScenario::savedRule(const QJsonObject& document)
{
    const auto rules = document.value(QStringLiteral("rules")).toArray();
    const auto o = rules.isEmpty() ? QJsonObject{} : rules.first().toObject();
    business::TaskRule r;
    r.task_id = s(o, "task_id"); r.revision = o.value(QStringLiteral("revision")).toInt(1);
    r.name = s(o, "name"); r.enabled = false;
    r.review_required = o.value(QStringLiteral("review_required")).toBool();
    if (!s(o, "product_ref").isEmpty()) r.product_ref = s(o, "product_ref");
    if (o.value(QStringLiteral("price_range")).isObject()) {
        const auto p = o.value(QStringLiteral("price_range")).toObject();
        r.price_range = business::PriceRange{money(p.value(QStringLiteral("min")).toObject()), money(p.value(QStringLiteral("max")).toObject())};
    }
    if (o.value(QStringLiteral("max_wear")).isObject()) r.max_wear = decimal(o.value(QStringLiteral("max_wear")).toObject());
    const auto f = o.value(QStringLiteral("filters")).toObject();
    r.filters = {selector(f.value(QStringLiteral("season")).toObject()), selector(f.value(QStringLiteral("ownership")).toObject()),
        selector(f.value(QStringLiteral("grade")).toObject()), selector(f.value(QStringLiteral("condition")).toObject()),
        selector(f.value(QStringLiteral("publicity")).toObject()), selector(f.value(QStringLiteral("rarity")).toObject())};
    return r;
}

QJsonObject ReplayScenario::observation(int n, qint64 now, const runtime::RunSnapshot& runtime,
                                        const QJsonObject& runDocument)
{
    const QChar suffix = QChar('A' + (n == 1 ? 0 : n == 4 ? 1 : n == 5 ? 2 : n == 6 ? 3 : n == 7 ? 4 : 5));
    business::ListingObservation o;
    o.observation_id = QStringLiteral("observation-") + suffix; o.market_listing_id = QStringLiteral("listing-") + suffix;
    o.product_ref = QStringLiteral("synthetic-product-1"); o.association = {QStringLiteral("fixture-association"),QStringLiteral("confirmed")};
    o.frame_ref = QStringLiteral("memory-frame-") + suffix; o.session_id = runtime.sessionId; o.clock_domain_id = runtime.clockDomainId;
    o.viewport_generation = 1; o.observed_mono_ms = n == 5 ? 0 : now;
    if (n != 6) o.price = business::Money{{QString::number(n == 7 ? 250 : n == 4 ? 149 : n == 8 ? 155 : 150),0},QStringLiteral("synthetic-unit")};
    for (const auto* field : {"product_ref","price","wear","condition","season","ownership","grade","rarity","publicity"}) {
        business::FieldEvidence evidence; evidence.source_ref = o.frame_ref;
        if (QLatin1String(field) == QStringLiteral("product_ref") || (QLatin1String(field) == QStringLiteral("price") && o.price)) evidence.status = QStringLiteral("observed");
        o.field_evidence.insert(QLatin1String(field),evidence);
    }
    const bool builtin = s(runDocument.value(QStringLiteral("profile")).toObject(),"id") == QStringLiteral("builtin-synthetic-fixture");
    const auto rule = builtin ? fixtureRule() : savedRule(runDocument);
    business::EvaluationContext context{runtime.sessionId,runtime.clockDomainId,1,quint64(now),250,QStringLiteral("filter_only"),{QStringLiteral("synthetic-unit")}};
    const auto vr = business::validateRule(rule);
    const auto vo = business::validateObservation(o);
    QString decision = QStringLiteral("NeedsReview"), reason = QStringLiteral("RULE_INVALID");
    if (const auto* validatedRule = std::get_if<business::ValidatedRule>(&vr)) {
        if (const auto* validatedObservation = std::get_if<business::ValidatedObservation>(&vo)) {
            const auto result = business::evaluate(*validatedRule,*validatedObservation,context);
            decision = result.status; reason = result.primary_reason;
        } else reason = QStringLiteral("OBSERVATION_INVALID");
    }
    QJsonArray missing; if (!o.price) missing.append(QStringLiteral("price"));
    return {{QStringLiteral("listing_id"),*o.market_listing_id},{QStringLiteral("observation_id"),o.observation_id},
        {QStringLiteral("product_id"),*o.product_ref},{QStringLiteral("product_name"),QStringLiteral("合成商品 · 非市场数据")},
        {QStringLiteral("observed_price"),o.price ? priceText(o.price->value) : QString()},
        {QStringLiteral("observed_mono_ms"),qint64(o.observed_mono_ms)}, {QStringLiteral("stale"),quint64(now)-o.observed_mono_ms>250},
        {QStringLiteral("missing_fields"),missing},{QStringLiteral("decision"),decision},{QStringLiteral("reason"),reason}};
}

} // namespace relink::workspace
