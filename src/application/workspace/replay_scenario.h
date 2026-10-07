#pragma once

#include "application/runtime/runtime.h"
#include "business/validator.h"
#include <QJsonObject>

namespace relink::workspace {

// Deterministic replay fixture and observation synthesis. Production capture
// adapters can replace this component without changing controller/UI contracts.
class ReplayScenario final {
public:
    static business::TaskRule fixtureRule();
    static business::TaskRule savedRule(const QJsonObject& document);
    static QJsonObject observation(int step, qint64 nowMonoMs,
                                   const runtime::RunSnapshot& runtime,
                                   const QJsonObject& runDocument);
};

} // namespace relink::workspace
