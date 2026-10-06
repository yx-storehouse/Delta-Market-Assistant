#pragma once

#include "models.h"

namespace relink::business {

Result<ValidatedRule> validateRule(const TaskRule& rule,
                                   const Catalog& catalog = {},
                                   const UnitCatalog& units = {});
Result<ValidatedObservation> validateObservation(const ListingObservation& observation);
Result<EvaluationContext> validateEvaluationContext(const EvaluationContext& context);

} // namespace relink::business

