#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>
#include <utility>

namespace relink::catalog {

struct Season {
    QString id;
    QString label;
};

struct Skin {
    QString productId;
    QString seasonId;
    QString seasonLabel;
    QString weapon;
    QString skinSeries;
    QString variantLabel;
    QString menuColor;
    QString gameQualityName;
    QString displayName;
    // Reserved for a future thumbnail feature. Current documents require empty.
    QString thumbnailPath;
};

struct Catalog {
    QVector<Season> seasons;
    QVector<Skin> skins;
    const Skin* findSkin(const QString& productId) const;
    const Season* findSeason(const QString& id) const;
};

// Strict bounded JSON only; no images, prices, positions, or runtime actions.
bool parseCatalog(const QByteArray& bytes, Catalog* result, QString* error = nullptr);
QByteArray serializeCatalog(const Catalog& catalog);
QString nextUserProductId();

class CatalogStore {
public:
    // On failure existing memory and files remain unchanged. Missing extension
    // is an empty user catalog; an existing malformed extension is an error.
    bool load(const QByteArray& builtinBytes, const QString& extensionPath,
              QString* error = nullptr);
    const Catalog& catalog() const { return m_catalog; }
    const QString& extensionPath() const { return m_extensionPath; }
    // Application-level projection checks run before disk or memory mutation.
    // The callback must not mutate this store; a standalone store needs none.
    void setCommitValidator(std::function<bool(const Catalog&, QString*)> validator) {
        m_commitValidator = std::move(validator);
    }
    bool appendSeason(const Season& season, const QVector<Skin>& skins,
                      QString* error = nullptr);
    bool appendSkins(const QVector<Skin>& skins, QString* error = nullptr);
    bool importJson(const QByteArray& bytes, QString* error = nullptr);
    QByteArray exportJson() const;
    QByteArray extensionJson() const;

private:
    bool commitAddition(const Catalog& addition, QString* error);
    Catalog m_builtin;
    Catalog m_extension;
    Catalog m_catalog;
    QString m_extensionPath;
    QByteArray m_extensionBytes;
    bool m_loaded = false;
    bool m_extensionExisted = false;
    std::function<bool(const Catalog&, QString*)> m_commitValidator;
};

} // namespace relink::catalog
