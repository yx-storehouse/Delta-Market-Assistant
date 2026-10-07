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
    QJsonObject toJson() const;
};

// Pure text/geometry boundary. Explicit full-client coverage is mandatory;
// partial OCR, legacy labels and navigation text alone are not page proof.
// These rules are calibrated to historical OCR records, not yet current live UI.
SkinPageResult classifySkinPage(const QJsonObject& observation);
} // namespace relink::vision
