#pragma once

#include "catalog/skin_catalog.h"
#include <QString>

class AppState;

namespace relink::application {

struct CatalogProjectionReport {
    int addedSkins = 0;
    int updatedSkins = 0;
    int removedDefaultSkins = 0;
    int removedDefaultTasks = 0;
    int preservedLegacySkins = 0;
    int clearedSyntheticObservations = 0;
};

QString catalogSkinId(const QString& productId);
// The 品阶 shown for a catalogue skin (its rarity field): the recorded label, or 待核对.
QString catalogGradeText(const QString& grade);
// No disk access. Caller owns config backup/save. A rejected projection leaves
// state unchanged. User tasks keep every parameter and may only get a remapped
// skinId when their existing catalog entry receives its stable catalog ID.
bool applyCatalogConfiguration(AppState& state, const catalog::Catalog& catalog,
                               CatalogProjectionReport* report = nullptr,
                               QString* error = nullptr);

} // namespace relink::application
