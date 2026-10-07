#pragma once
#include <QJsonObject>
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
// Opt-in calibration aid: only three fixed, non-personal lobby button regions.
// This is a partial projection, not a page classification or action target.
QJsonObject projectLobbyAnchorDiagnostics(const QJsonObject& observation);
QJsonObject projectMarketAnchorDiagnostics(const QJsonObject& observation);
} // namespace relink::vision
