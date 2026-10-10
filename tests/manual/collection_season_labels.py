"""Evidence-bound source/game season labels; never rewrite imported task data.

Only the three individually verified S11 Black Silver products are mapped.
This is not an OCR repair rule, a fuzzy alias, or a global 风/光 replacement.
Evidence: artifacts/collection_season_labels/REPORT.md.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping


SOURCE_LABEL = '疾风魅影'
GAME_LABEL = '疾光魅影'
MAPPING_ID = 's11-black-silver-source-season-20261008-v1'
VERIFIED_PRODUCTS = (
    ('11102', 'AUG突击步枪-黑银先锋'),
    ('11103', 'AS Val突击步枪-黑银先锋'),
    ('11104', 'P90冲锋枪-黑银先锋'),
)
# The current exporter removes the single ASCII space inside AS Val. Accept
# that observed serialization only for its known product ID; no normalization.
VERIFIED_EXPORTED_PRODUCTS = VERIFIED_PRODUCTS + (
    ('11103', 'ASVal突击步枪-黑银先锋'),
)
EVIDENCE = (
    'artifacts/collection_season_labels/evidence.json#original_dictionary',
    'artifacts/collection_season_labels/evidence.json#original_game_logs',
    'https://df.qq.com/cp/a20240906main/newsdetail.html?id=9650576787262514590',
)


@dataclass(frozen=True)
class SeasonLabelResolution:
    source_label: str
    game_label: str
    mapping_id: str | None = None
    evidence: tuple[str, ...] = ()

    @property
    def corrected(self) -> bool:
        return self.mapping_id is not None

    def event_fields(self) -> dict:
        return dict(source_season_label=self.source_label,
                    game_season_label=self.game_label,
                    season_label_mapping_id=self.mapping_id,
                    season_label_evidence=list(self.evidence))


def resolve_season_label(row: Mapping) -> SeasonLabelResolution:
    """Return a navigation label for an exact verified identity, without I/O.

    An unrecognized/mismatched identity retains its literal source label, so
    existing observed-menu matching still decides whether it can be navigated.
    No product title, OCR output, snapshot field or rule fingerprint is changed.
    """
    source = row['season_label']
    if (source == SOURCE_LABEL and row.get('season_id') == 'S11'
            and row.get('dictionary_resolved') is True
            and (row.get('product_id'), row.get('product_name')) in VERIFIED_EXPORTED_PRODUCTS):
        return SeasonLabelResolution(source, GAME_LABEL, MAPPING_ID, EVIDENCE)
    return SeasonLabelResolution(source, source)
