#pragma once

#include "config/import_types.h"

// Schema v1 -> LegacyImportPreview bridge (PR06 / WI02).
//
// previewV1() never returns a loadable configuration: it always produces a
// schema-compliant import-preview.schema.json document with committable=false
// and candidates that are disabled / review_required. It is a pure function
// over the input bytes and never writes files or touches AppState.
//
// Preview extensions (all keys are x- prefixed because core.schema.json limits
// Extensions property names; values are metadata and never participate in
// matching or execution):
//   x-target_mode           "demo" — old synthetic data never becomes Observe/live.
//   x-catalog               complete SkinCatalogEntry[] built from /skins.
//   x-ui_run_settings       all 21 RunSettings fields after per-field default fill.
//   x-ui_run_settings_sources  field -> "default" | "v1" | "invalid".
//   x-ui_run_settings_raw_tokens  JSON Pointer -> original number token.
//   x-ui_run_settings_legacy_unknown  unrecognized run keys, redacted.
//   x-id_map                {"skins": {old: new}, "tasks": {old: new}}; a new
//                           stable ID is minted only when the old ID is not a
//                           valid new-schema Id, the mapping keeps the original.
//   x-legacy_unknown_fields unrecognized root keys, redacted, keyed by pointer.
//   x-observation_source    "synthetic_demo".
//   x-live_market_data      false — legacy price/wear/change is demo metadata,
//                           not an observation of a live market.
//
// Every candidate also carries candidate.extensions.x-product_name (catalog
// display name) and candidate.legacy_raw.unknown_json with the original number
// tokens keyed by JSON Pointer, because QJson already lost the token text by
// the time a double exists. Original tokens are therefore never re-exported as
// supposedly lossless values.
//
// previewV1 is declared in config/import_types.h so both adapters share the
// same API; this header exists for documentation and for callers that only
// include the adapter.

namespace relink::config {

// Defaults used when a field or the whole /run_settings object is missing.
QJsonObject previewV1(const QByteArray& bytes);

} // namespace relink::config
