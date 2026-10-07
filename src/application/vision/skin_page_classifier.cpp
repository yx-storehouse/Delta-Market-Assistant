#include "skin_page_classifier.h"
#include <QJsonArray>
#include <QRectF>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <functional>

namespace relink::vision {
namespace {
struct Token { QString text; QRectF bounds; };
QString normalized(QString text) {
    text = text.normalized(QString::NormalizationForm_KC);
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    // Only known checkbox/diamond decorations, not arbitrary Chinese letters.
    while (!text.isEmpty() && QStringLiteral("□■△◇◆").contains(text.front())) text.remove(0, 1);
    return text;
}
bool inBand(const QRectF& rect, const QRectF& band) { return band.contains(rect.center()); }
class Anchors {
public:
    explicit Anchors(QVector<Token> tokens, double aspect) : m_tokens(std::move(tokens)), m_aspect(aspect) {
        // Windows OCR can split a Chinese label into adjacent words. Combine
        // only nearby same-line tokens, never concatenate the entire screen.
        std::sort(m_tokens.begin(), m_tokens.end(), [](const Token& a, const Token& b) {
            return a.bounds.top() == b.bounds.top() ? a.bounds.left() < b.bounds.left() : a.bounds.top() < b.bounds.top();
        });
        QVector<QVector<Token>> rows;
        for (const auto& token : m_tokens) {
            int match = -1;
            for (int i = rows.size() - 1; i >= 0; --i) {
                const auto& first = rows[i].first();
                if (token.bounds.top() - first.bounds.bottom() > 0.05) break;
                if (std::abs(token.bounds.center().y() - first.bounds.center().y()) <= 0.35 * std::min(token.bounds.height(), first.bounds.height())) {
                    match = i; break;
                }
            }
            if (match < 0) rows.push_back({token}); else rows[match].push_back(token);
        }
        for (auto& row : rows) {
            std::sort(row.begin(), row.end(), [](const Token& a, const Token& b) { return a.bounds.left() < b.bounds.left(); });
            for (int i = 0; i < row.size(); ++i) {
                Token joined = row[i];
                for (int j = i + 1; j < row.size() && j < i + 8; ++j) {
                    const double gap = row[j].bounds.left() - row[j-1].bounds.right();
                    if (gap < -0.002 || gap > std::max(row[j].bounds.height(), row[j-1].bounds.height()) * 0.9) break;
                    joined.text += row[j].text;
                    joined.bounds = joined.bounds.united(row[j].bounds);
                    if (joined.text.size() > 100) break;
                    m_joined.push_back(joined);
                }
            }
        }
    }
    bool exact(const QString& text, const QRectF& band) const {
        for (const auto& token : m_tokens) if (token.text == text && inBand(token.bounds, band)) return true;
        for (const auto& token : m_joined) if (token.text == text && inBand(token.bounds, band)) return true;
        return localSequence(text,band);
    }
    bool contains(const QString& text, const QRectF& band) const {
        for (const auto& token : m_tokens) if (token.text.contains(text) && inBand(token.bounds, band)) return true;
        for (const auto& token : m_joined) if (token.text.contains(text) && inBand(token.bounds, band)) return true;
        return localSequence(text,band);
    }
    bool receipt(const QString& text) const {
        // Collection toasts may be nine individual Chinese OCR tokens. Keep
        // the longer bound local to the central toast, not every page anchor.
        const QRectF toast(.35, .12, .30, .08);
        for (const auto& token : m_tokens)
            if (token.text == text && inBand(token.bounds, toast)) return true;
        return localSequence(text, toast, 16);
    }
    int countExact(const QString& text, const QRectF& band) const {
        int count = 0;
        // Joined windows do not count as extra independent supporting anchors.
        for (const auto& token : m_tokens) if (token.text == text && inBand(token.bounds, band)) ++count;
        return count;
    }
    bool pageNumber(const QRectF& band) const {
        static const QRegularExpression expression(QStringLiteral("^第[1-9][0-9]{0,4}页$"));
        for (const auto& token : m_tokens)
            if (inBand(token.bounds, band) && expression.match(token.text).hasMatch()) return true;
        for (const auto& token : m_joined)
            if (inBand(token.bounds, band) && expression.match(token.text).hasMatch()) return true;
        return false;
    }
private:
    bool localSequence(const QString& label,const QRectF& band,int maxTokens=8) const {
        // Whole-screen row grouping can be disrupted by unrelated scene text.
        // Match a bounded exact label locally, with physical-pixel adjacency;
        // no arbitrary character substitution or whole-screen concatenation.
        int comparisons=8192;
        std::function<bool(int,int,int)> extend=[&](int last,int used,int depth){
            if(used==label.size())return true;
            if(depth>=maxTokens)return false;
            const auto& previous=m_tokens[last];
            for(int j=0;j<m_tokens.size();++j){
                if(--comparisons<0)return false;
                const auto& next=m_tokens[j];
                if(next.bounds.left()<=previous.bounds.left() || !inBand(next.bounds,band))continue;
                const double font=std::min(previous.bounds.height(),next.bounds.height());
                const double gap=next.bounds.left()-previous.bounds.right();
                if(gap < -.35*font/m_aspect || gap > .60*font/m_aspect
                    || std::abs(next.bounds.center().y()-previous.bounds.center().y())>.5*font)continue;
                if(label.mid(used).startsWith(next.text) && extend(j,used+next.text.size(),depth+1))return true;
            }
            return false;
        };
        for(int i=0;i<m_tokens.size() && comparisons>0;++i)
            if(inBand(m_tokens[i].bounds,band) && label.startsWith(m_tokens[i].text)
                && extend(i,m_tokens[i].text.size(),1))return true;
        return false;
    }
    QVector<Token> m_tokens;
    QVector<Token> m_joined;
    double m_aspect;
};
}

QString toString(SkinPage page) {
    switch (page) {
    case SkinPage::Lobby: return QStringLiteral("lobby");
    case SkinPage::Warehouse: return QStringLiteral("warehouse");
    case SkinPage::Mandel: return QStringLiteral("mandel");
    case SkinPage::SkinHome: return QStringLiteral("skin_home");
    case SkinPage::EmptyWatchlist: return QStringLiteral("empty_watchlist");
    case SkinPage::CatalogFilter: return QStringLiteral("catalog_filter");
    case SkinPage::SkinListings: return QStringLiteral("skin_listings");
    case SkinPage::WatchlistListings: return QStringLiteral("watchlist_listings");
    case SkinPage::GameSettings: return QStringLiteral("game_settings");
    case SkinPage::Base: return QStringLiteral("base");
    case SkinPage::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}
QString toString(PageOverlay overlay) {
    switch (overlay) {
    case PageOverlay::ListingFilter: return QStringLiteral("listing_filter");
    case PageOverlay::ResultDialog: return QStringLiteral("result_dialog");
    case PageOverlay::None: return QStringLiteral("none");
    }
    return QStringLiteral("none");
}
QJsonObject SkinPageResult::toJson() const {
    return {{"valid_input", validInput}, {"page", toString(page)}, {"overlay", toString(overlay)},
        {"reason", reason}, {"anchors", QJsonArray::fromStringList(anchors)},
        {"candidate_pages", QJsonArray::fromStringList(candidates)}, {"token_count", tokenCount},
        {"low_score_tokens", lowScoreTokens}, {"provider_scores_available", providerScoresAvailable},
        {"calibration", calibrationEvidence}, {"anchor_checks", anchorChecks}, {"live_calibrated", false}, {"actions_enabled", false}};
}

SkinPageResult classifySkinPage(const QJsonObject& observation) {
    SkinPageResult result;
    const auto fail = [&](const QString& reason) { result.reason = reason; return result; };
    const auto width = observation.value("width"), height = observation.value("height");
    const auto dimension = [](const QJsonValue& value) {
        return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 1
            && value.toDouble() <= 8192 && std::floor(value.toDouble()) == value.toDouble();
    };
    if (!dimension(width) || !dimension(height)) return fail(QStringLiteral("E_PAGE_DIMENSIONS"));
    if (observation.value("coverage") != "full_client") return fail(QStringLiteral("E_PAGE_COVERAGE"));
    if (!observation.value("words").isArray() || observation.value("words").toArray().size() > 2000)
        return fail(QStringLiteral("E_PAGE_TOKENS"));
    QVector<Token> tokens;
    int bytes = 0;
    for (const auto& value : observation.value("words").toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("E_PAGE_TOKEN"));
        const auto word = value.toObject();
        if (!word.value("text").isString() || word.value("text").toString().isEmpty()
            || word.value("text").toString().size() > 1024) return fail(QStringLiteral("E_PAGE_TEXT"));
        bytes += word.value("text").toString().toUtf8().size();
        if (bytes > 65536) return fail(QStringLiteral("E_PAGE_TEXT_LIMIT"));
        const auto x = word.value("x"), y = word.value("y"), w = word.value("width"), h = word.value("height");
        for (const auto& number : {x, y, w, h})
            if (!number.isDouble() || !std::isfinite(number.toDouble())) return fail(QStringLiteral("E_PAGE_BOUNDS"));
        if (x.toDouble() < 0 || y.toDouble() < 0 || w.toDouble() <= 0 || h.toDouble() <= 0
            || x.toDouble() + w.toDouble() > width.toDouble() || y.toDouble() + h.toDouble() > height.toDouble())
            return fail(QStringLiteral("E_PAGE_BOUNDS"));
        ++result.tokenCount;
        if (word.contains("score")) {
            const auto score = word.value("score");
            if (!score.isDouble() || !std::isfinite(score.toDouble()) || score.toDouble() < 0 || score.toDouble() > 1)
                return fail(QStringLiteral("E_PAGE_SCORE"));
            result.providerScoresAvailable = true;
            if (score.toDouble() < 0.70) { ++result.lowScoreTokens; continue; }
        }
        tokens.push_back({normalized(word.value("text").toString()),
            QRectF(x.toDouble()/width.toDouble(), y.toDouble()/height.toDouble(), w.toDouble()/width.toDouble(), h.toDouble()/height.toDouble())});
    }
    result.validInput = true;
    const Anchors a(std::move(tokens),width.toDouble()/height.toDouble());
    const QRectF top(0, 0, 1, .25), body(0, .12, 1, .82), leftBody(0, .12, .5, .82);
    const QRectF bottom(0, .5, 1, .5), center(.1, .12, .8, .7), right(.65, 0, .35, 1);
    const auto exact = [&](const char16_t* text, const QRectF& band) { return a.exact(QString::fromUtf16(text), band); };
    const auto has = [&](const char16_t* text, const QRectF& band) { return a.contains(QString::fromUtf16(text), band); };
    const auto candidate = [&](SkinPage page, const QStringList& anchors) {
        result.candidates.append(toString(page)); result.anchors.append(anchors);
    };
    if (exact(u"分辨率", body) && exact(u"显示模式", body) && exact(u"局内帧数上限", body))
        candidate(SkinPage::GameSettings, {"settings.resolution", "settings.display_mode", "settings.frame_limit"});
    if (exact(u"赛季", center) && exact(u"品阶", center) && exact(u"枪种", center) && exact(u"确认", center))
        candidate(SkinPage::CatalogFilter, {"filter.season", "filter.quality", "filter.weapon_type", "filter.confirm"});
    const bool sale = exact(u"在售", top);
    if (sale && has(u"暂未添加任何关注", body)) {
        QStringList evidence{"list.sale", "watch.empty"};
        if (has(u"返回", bottom)) evidence.append("navigation.back");
        candidate(SkinPage::EmptyWatchlist, evidence);
    }
    // The open listing-filter panel covers "similar skins" and dims the
    // footer. Its three independent right-panel anchors can prove that layout
    // instead; sale/sort/condition context is still mandatory.
    const bool listingFilterPanel=exact(u"价格区间",right) && exact(u"所有成色",right) && exact(u"确定",right);
    const bool listings = sale && (exact(u"默认排序", top) || has(u"按稀有度", top) || has(u"按价格", top) || has(u"按成色", top))
        && has(u"成色", body) && (a.pageNumber(bottom) || has(u"相似皮肤", body) || listingFilterPanel);
    if (listings) {
        const bool watched = has(u"我的关注:", bottom);
        candidate(watched ? SkinPage::WatchlistListings : SkinPage::SkinListings,
            watched ? QStringList{"list.sale", "list.sort", "list.condition", "list.layout", "watch.count"}
                    : QStringList{"list.sale", "list.sort", "list.condition", "list.layout"});
        if (listingFilterPanel)
            result.overlay = PageOverlay::ListingFilter;
    }
    result.anchorChecks={{"listing.sale",sale},{"listing.default_sort",exact(u"默认排序",top)},
        {"listing.condition",has(u"成色",body)},{"listing.page_number",a.pageNumber(bottom)},
        {"listing.similar",has(u"相似皮肤",body)},
        {"listing.filter_price",exact(u"价格区间",right)},{"listing.filter_all",exact(u"所有成色",right)},
        {"listing.filter_confirm",exact(u"确定",right)},
        {"collection.added",a.receipt(QStringLiteral("成功添加至我的关注"))},
        {"collection.full",has(u"关注数量已达上限",body) || has(u"已达关注上限",body)},
        {"catalog.skin_tab",exact(u"典藏外观",top)},{"catalog.circulation",has(u"流通量",body)},
        {"catalog.watch",exact(u"我的关注",top)},{"catalog.rows",a.countExact(QStringLiteral("典藏"),leftBody)>=2},
        {"mandel.tab",exact(u"曼德尔砖",top)},{"mandel.current",exact(u"当季产出",body)},{"mandel.past",exact(u"往季产出",body)}};
    if (exact(u"典藏外观", top) && has(u"流通量", body)
        && (exact(u"我的关注", top) || a.countExact(QStringLiteral("典藏"), leftBody) >= 2))
        candidate(SkinPage::SkinHome, {"catalog.skin_tab", "catalog.circulation", "catalog.watch_or_rows"});
    if (exact(u"曼德尔砖", top) && exact(u"当季产出", body) && exact(u"往季产出", body))
        candidate(SkinPage::Mandel, {"mandel.tab", "mandel.current_season", "mandel.past_seasons"});
    if (exact(u"装备价值", QRectF(0, .08, 1, .85)) && (has(u"口袋", body) || has(u"安全箱", body)))
        candidate(SkinPage::Warehouse, {"warehouse.equipment", "warehouse.storage"});
    if (exact(u"开始游戏", top) && exact(u"仓库", top) && exact(u"行前备战", body))
        candidate(SkinPage::Lobby, {"lobby.start", "lobby.warehouse_nav", "lobby.prepare"});
    else if (observation.value("protocol") == "windows-ocr-once-v1"
        && observation.value("language") == "zh-Hans-CN"
        && exact(u"开始游观", QRectF(.065, .025, .07, .05))
        && exact(u"仓库", QRectF(.14, .025, .065, .05))
        && exact(u"行前备战", QRectF(.79, .87, .15, .055))) {
        // Live 2026-10-07, 2560x1440/144 DPI: Windows OCR read 戏 as 观.
        // Do not replace characters globally or loosen other page rules. This
        // one variant requires its provider AND both independent local anchors.
        candidate(SkinPage::Lobby, {"lobby.start_scoped_ocr_variant", "lobby.warehouse_nav", "lobby.prepare"});
        result.calibrationEvidence = QStringLiteral("lobby_live_sample_20261007");
    }
    int baseAnchors = 0;
    for (const auto& label : {u"技术中心", u"工作台", u"指挥中心", u"训练中心", u"靶场", u"净水中心", u"收藏室"})
        if (exact(label, body)) ++baseAnchors;
    if (baseAnchors >= 3) candidate(SkinPage::Base, {"base.multiple_facilities"});
    if ((has(u"获得枪械外观", center) || has(u"该商品已被购买或已下架", center)) && (has(u"确认", body) || has(u"返回", body)))
        result.overlay = PageOverlay::ResultDialog;
    if (result.candidates.size() > 1) return fail(QStringLiteral("E_PAGE_AMBIGUOUS"));
    if (result.candidates.isEmpty()) return fail(QStringLiteral("E_PAGE_INSUFFICIENT_ANCHORS"));
    for (SkinPage page : {SkinPage::Lobby, SkinPage::Warehouse, SkinPage::Mandel, SkinPage::SkinHome,
            SkinPage::EmptyWatchlist, SkinPage::CatalogFilter, SkinPage::SkinListings, SkinPage::WatchlistListings,
            SkinPage::GameSettings, SkinPage::Base})
        if (toString(page) == result.candidates.first()) result.page = page;
    result.reason = result.overlay == PageOverlay::None ? QStringLiteral("MULTI_ANCHOR_MATCH") : QStringLiteral("OVERLAY_REQUIRES_RECHECK");
    return result;
}
QJsonObject projectLobbyAnchorDiagnostics(const QJsonObject& observation) {
    const auto validated = classifySkinPage(observation);
    QJsonObject output{{"schema", "lobby-anchor-diagnostics-v1"}, {"coverage", "anchor_projection"},
        {"valid_input", validated.validInput}, {"actions_enabled", false}};
    if (!validated.validInput) { output["error"] = validated.reason; return output; }
    const double width = observation.value("width").toDouble(), height = observation.value("height").toDouble();
    output["width"] = width; output["height"] = height;
    struct Region { const char* id; QRectF area; };
    const Region regions[] = {{"start_navigation", QRectF(.065, .025, .07, .05)},
        {"warehouse_navigation", QRectF(.14, .025, .065, .05)},
        {"prepare_button", QRectF(.79, .87, .15, .055)}};
    QJsonArray projected;
    for (const auto& region : regions) {
        QJsonArray words;
        int excluded = 0;
        bool truncated = false;
        for (const auto& value : observation.value("words").toArray()) {
            const auto word = value.toObject();
            const QRectF rect(word.value("x").toDouble()/width, word.value("y").toDouble()/height,
                word.value("width").toDouble()/width, word.value("height").toDouble()/height);
            if (!region.area.contains(rect)) continue;
            static const QRegularExpression knownCharacters(QStringLiteral("^[开始游戏观仓库行前备战\\s□■△◇◆]+$"));
            if (!knownCharacters.match(word.value("text").toString()).hasMatch()) { ++excluded; continue; }
            if (words.size() == 32) { truncated = true; continue; }
            QJsonObject minimal;
            for (const auto* field : {"text", "x", "y", "width", "height", "score"})
                if (word.contains(field)) minimal[field] = word.value(field);
            words.append(minimal);
        }
        projected.append(QJsonObject{{"id", region.id}, {"words", words},
            {"excluded_tokens", excluded}, {"truncated", truncated}});
    }
    output["regions"] = projected;
    return output;
}
QJsonObject projectMarketAnchorDiagnostics(const QJsonObject& observation) {
    const auto validated = classifySkinPage(observation);
    QJsonObject output{{"schema", "market-anchor-diagnostics-v1"}, {"coverage", "anchor_projection"},
        {"valid_input", validated.validInput}, {"actions_enabled", false}};
    if (!validated.validInput) { output["error"] = validated.reason; return output; }
    const double width = observation.value("width").toDouble(), height = observation.value("height").toDouble();
    output["width"] = width; output["height"] = height;
    QJsonArray words;
    int excluded = 0;
    bool truncated = false;
    // Fixed page-label areas only; no UID strip or wallet at the top right.
    const QRectF areas[] = {QRectF(.035,.04,.50,.16), QRectF(.035,.20,.20,.67),
        QRectF(.10,.12,.70,.70), QRectF(.84,.15,.115,.13), QRectF(.025,.90,.85,.08)};
    static const QRegularExpression characters(QStringLiteral("^[曼德尔砖典藏外观挂饰当季产出往我的关注流通量在售默认排序成色第页价格区间所有确定赛品阶枪种确认暂未添加任何皮肤未拥有全部搜索筛选返回取消成交均价\\s□■△◇◆:：/0-9]+$"));
    for (const auto& value : observation.value("words").toArray()) {
        const auto word = value.toObject();
        const QRectF rect(word.value("x").toDouble()/width,word.value("y").toDouble()/height,
            word.value("width").toDouble()/width,word.value("height").toDouble()/height);
        bool inside = false; for (const auto& area : areas) inside |= area.contains(rect);
        if (!inside) continue;
        if (word.value("text").toString().size()>32 || !characters.match(word.value("text").toString()).hasMatch()) { ++excluded; continue; }
        if (words.size()==256) { truncated=true; continue; }
        QJsonObject minimal;
        for (const auto* field : {"text","x","y","width","height","score"})
            if (word.contains(field)) minimal[field]=word.value(field);
        words.append(minimal);
    }
    output["words"]=words; output["excluded_tokens"]=excluded; output["truncated"]=truncated;
    return output;
}
} // namespace relink::vision
