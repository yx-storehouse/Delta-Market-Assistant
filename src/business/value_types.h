#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <QVector>
#include <variant>

namespace relink::business {

struct Diagnostic {
    QString code;
    QString field;
    QString message;
    int line = 0;
    int column = 0;
};

template <typename T>
using Result = std::variant<T, QVector<Diagnostic>>;

template <typename T>
inline bool isOk(const Result<T>& value) { return std::holds_alternative<T>(value); }

template <typename T>
inline const T* resultValue(const Result<T>& value) {
    return std::get_if<T>(&value);
}

template <typename T>
inline const QVector<Diagnostic>* resultDiagnostics(const Result<T>& value) {
    return std::get_if<QVector<Diagnostic>>(&value);
}

struct DecimalParsePolicy {
    int maxUnscaledDigits = 24;
    int maxScale = 12;
};

struct DecimalValue {
    QString unscaled;
    quint8 scale = 0;

    bool isStructurallyValid(int maxDigits = 24, int maxScale = 12) const;
    bool isZero() const;
};

enum class Ordering { Less = -1, Equal = 0, Greater = 1 };

Result<DecimalValue> parseDecimal(QStringView raw,
                                  const DecimalParsePolicy& policy = DecimalParsePolicy{});
Ordering compareDecimal(const DecimalValue& a, const DecimalValue& b);

// Internal helpers deliberately remain allocation-free with respect to floating-point
// arithmetic. They are useful to the model validators and are public for deterministic
// tests of the decimal contract.
bool decimalIsMultipleOf(const DecimalValue& value, const DecimalValue& quantum);
bool decimalIsPositive(const DecimalValue& value);

QString orderingString(Ordering ordering);

} // namespace relink::business
