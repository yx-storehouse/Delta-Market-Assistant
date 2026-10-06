#pragma once

#include "validator.h"

namespace relink::business {

Decision evaluate(const ValidatedRule& rule,
                  const ValidatedObservation& observation,
                  const EvaluationContext& context);

} // namespace relink::business

