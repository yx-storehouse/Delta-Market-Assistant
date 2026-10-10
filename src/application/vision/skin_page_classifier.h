#pragma once
#include <QJsonObject>
#include <QRect>
#include <QStringList>

namespace relink::vision {
enum class SkinPage {
    Unknown, Lobby, Warehouse, Mandel, SkinHome, EmptyWatchlist,
    CatalogFilter, SkinListings, WatchlistListings, GameSettings, Base,
};
enum class PageOverlay { None, ListingFilter, ResultDialog };
QString toString(SkinPage page);
QString toString(PageOverlay overlay);

struct SkinPageResult {
    bool validInput = false;
    SkinPage page = SkinPage::Unknown;
    PageOverlay overlay = PageOverlay::None;
    QString reason;
    QStringList anchors;
    QStringList candidates;
    int tokenCount = 0;
    int lowScoreTokens = 0;
    bool providerScoresAvailable = false;
    QString calibrationEvidence = QStringLiteral("historical_ocr_only");
    QJsonObject anchorChecks;
    QJsonObject toJson() const;
};

// Pure text/geometry boundary. Explicit full-client coverage is mandatory;
// partial OCR, legacy labels and navigation text alone are not page proof.
// Historical rules plus one narrowly scoped live Windows OCR lobby variant.
// A matching sample never enables actions or certifies all current game pages.
SkinPageResult classifySkinPage(const QJsonObject& observation);
// Request a real same-frame circulation label only after navigation ROI words
// have been merged. This region is not itself proof of a home page.
QRect homeCirculationRefinementRegion(const QJsonObject& observation);
// Narrow same-frame re-read of the listing filter's text, excluding its bright
// checkbox. This only requests/merges real OCR words; all original modal
// anchors remain mandatory and the returned projection never enables actions.
QRect listingFilterAllLabelRefinementRegion(const QJsonObject& observation);
QJsonObject refineListingFilterAllLabel(const QJsonObject& observation,
    const QJsonObject& regionObservation, const QRect& region, QJsonObject* evidence = nullptr);
// Startup-only repair: two new narrow reads of the SAME frame must still
// satisfy the original complete sale + empty-message rule. No OCR character
// substitution, past-frame word reuse, or partial-message acceptance.
QJsonObject refineEmptyWatchlistPage(const QJsonObject& observation,
    const QJsonObject& saleReading, const QJsonObject& emptyReading, QJsonObject* evidence = nullptr);
// The watchlist its refresh emptied (live 2026-10-09 purchase_readonly/peek01):
// "暂无信息内容" centred in the list panel, "我的关注：0/150" bottom-left. The
// same message can show on an empty market listing, so a third read of the
// count corner is required. Same contract: new same-frame reads, exact words.
inline const QRect EmptiedWatchlistContentRegion(860,840,260,70);
inline const QRect WatchlistCountRegion(100,1235,360,60);
QJsonObject refineEmptiedWatchlistPage(const QJsonObject& observation, const QJsonObject& saleReading,
    const QJsonObject& contentReading, const QJsonObject& countReading, QJsonObject* evidence = nullptr);
// Opt-in calibration aid: only three fixed, non-personal lobby button regions.
// This is a partial projection, not a page classification or action target.
QJsonObject projectLobbyAnchorDiagnostics(const QJsonObject& observation);
QJsonObject projectMarketAnchorDiagnostics(const QJsonObject& observation);
} // namespace relink::vision
