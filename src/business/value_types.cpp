#include "value_types.h"

#include <QChar>

namespace relink::business {
namespace {

Diagnostic error(const QString& code, const QString& message, const QString& field = {}) {
    return {code, field, message, 0, 0};
}

QString stripLeadingZeroes(const QString& value) {
    int i = 0;
    while (i + 1 < value.size() && value.at(i) == QLatin1Char('0')) ++i;
    return value.mid(i);
}

int compareUnsignedStrings(QString a, QString b) {
    a = stripLeadingZeroes(a);
    b = stripLeadingZeroes(b);
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

QString padRight(const QString& input, int count) {
    QString result = input;
    result += QString(count, QLatin1Char('0'));
    return result;
}

// Decimal long division remainder. The values passed here are non-negative decimal
// strings and can be longer than native integer widths.
int decimalRemainder(const QString& value, const QString& divisor) {
    QString d = stripLeadingZeroes(divisor);
    if (d == QStringLiteral("0")) return -1;
    QString remainder = QStringLiteral("0");
    for (const QChar ch : value) {
        remainder = stripLeadingZeroes(remainder + ch);
        int digit = 0;
        while (compareUnsignedStrings(remainder, d) >= 0) {
            // Subtract d using schoolbook arithmetic. This loop is bounded by 10 for
            // each digit because remainder < 10*d after the append above.
            QString out(remainder.size(), QLatin1Char('0'));
            int i = remainder.size() - 1;
            int j = d.size() - 1;
            int borrow = 0;
            while (i >= 0) {
                int lhs = remainder.at(i).unicode() - '0' - borrow;
                int rhs = j >= 0 ? d.at(j).unicode() - '0' : 0;
                if (lhs < rhs) { lhs += 10; borrow = 1; } else borrow = 0;
                out[i] = QChar('0' + lhs - rhs);
                --i; --j;
            }
            remainder = stripLeadingZeroes(out);
            ++digit;
            if (digit > 10) break;
        }
    }
    return remainder == QStringLiteral("0") ? 0 : 1;
}

} // namespace

bool DecimalValue::isStructurallyValid(int maxDigits, int maxScale) const {
    if (scale > maxScale || unscaled.isEmpty() || unscaled.size() > maxDigits) return false;
    if (unscaled == QStringLiteral("0")) return true;
    if (unscaled.at(0) == QLatin1Char('0')) return false;
    for (const QChar ch : unscaled)
        if (!ch.isDigit()) return false;
    return true;
}

bool DecimalValue::isZero() const { return unscaled == QStringLiteral("0"); }

Result<DecimalValue> parseDecimal(QStringView raw, const DecimalParsePolicy& policy) {
    if (raw.isEmpty())
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("decimal is empty") )};
    if (raw.contains(QLatin1Char('e')) || raw.contains(QLatin1Char('E')) ||
        raw.contains(QLatin1Char('+')) || raw.contains(QLatin1Char('-')))
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("signs and exponents are not accepted"))};

    const int dot = raw.indexOf(QLatin1Char('.'));
    if (dot >= 0 && raw.indexOf(QLatin1Char('.'), dot + 1) >= 0)
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("decimal has more than one point"))};
    const QString integer = (dot < 0 ? raw : raw.left(dot)).toString();
    const QString fraction = (dot < 0 ? QStringView{} : raw.mid(dot + 1)).toString();
    if (integer.isEmpty() || (dot >= 0 && fraction.isEmpty()))
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("decimal requires digits on both sides of the point"))};
    for (const QChar ch : integer)
        if (!ch.isDigit()) return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("decimal contains a non-digit"))};
    for (const QChar ch : fraction)
        if (!ch.isDigit()) return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("decimal contains a non-digit"))};

    if (integer.size() > 1 && integer.at(0) == QLatin1Char('0'))
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_INVALID"), QStringLiteral("leading zeroes are not accepted"))};
    if (fraction.size() > policy.maxScale)
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_CAPACITY_EXCEEDED"), QStringLiteral("decimal scale exceeds the contract limit"))};

    QString unscaled = stripLeadingZeroes(integer + fraction);
    if (unscaled.isEmpty()) unscaled = QStringLiteral("0");
    if (unscaled.size() > policy.maxUnscaledDigits)
        return QVector<Diagnostic>{error(QStringLiteral("DECIMAL_CAPACITY_EXCEEDED"), QStringLiteral("decimal precision exceeds the contract limit"))};
    DecimalValue result{unscaled, static_cast<quint8>(fraction.size())};
    return result;
}

Ordering compareDecimal(const DecimalValue& a, const DecimalValue& b) {
    const int scale = qMax<int>(a.scale, b.scale);
    const QString lhs = padRight(stripLeadingZeroes(a.unscaled), scale - a.scale);
    const QString rhs = padRight(stripLeadingZeroes(b.unscaled), scale - b.scale);
    const int cmp = compareUnsignedStrings(lhs, rhs);
    return cmp < 0 ? Ordering::Less : (cmp > 0 ? Ordering::Greater : Ordering::Equal);
}

bool decimalIsMultipleOf(const DecimalValue& value, const DecimalValue& quantum) {
    if (!value.isStructurallyValid() || !quantum.isStructurallyValid() || quantum.isZero()) return false;
    const int scale = qMax<int>(value.scale, quantum.scale);
    const QString v = padRight(stripLeadingZeroes(value.unscaled), scale - value.scale);
    const QString q = padRight(stripLeadingZeroes(quantum.unscaled), scale - quantum.scale);
    return decimalRemainder(v, q) == 0;
}

bool decimalIsPositive(const DecimalValue& value) {
    return value.isStructurallyValid() && !value.isZero();
}

QString orderingString(Ordering ordering) {
    switch (ordering) {
    case Ordering::Less: return QStringLiteral("less");
    case Ordering::Greater: return QStringLiteral("greater");
    case Ordering::Equal: return QStringLiteral("equal");
    }
    return {};
}

} // namespace relink::business

