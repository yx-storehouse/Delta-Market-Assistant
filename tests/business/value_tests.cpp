#include "../../src/business/value_types.h"

#include <QCoreApplication>
#include <QDebug>

using namespace relink::business;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto a = parseDecimal(QStringView{QStringLiteral("0.399000")});
    Q_ASSERT(isOk(a));
    Q_ASSERT(std::get<DecimalValue>(a).unscaled == QStringLiteral("399000"));
    Q_ASSERT(std::get<DecimalValue>(a).scale == 6);
    const auto b = parseDecimal(QStringView{QStringLiteral("0.399")});
    Q_ASSERT(isOk(b));
    Q_ASSERT(compareDecimal(std::get<DecimalValue>(a), std::get<DecimalValue>(b)) == Ordering::Equal);
    const auto c = parseDecimal(QStringView{QStringLiteral("229.999999999999")});
    Q_ASSERT(isOk(c));
    const auto d = parseDecimal(QStringView{QStringLiteral("229.999999999999")});
    Q_ASSERT(compareDecimal(std::get<DecimalValue>(c), std::get<DecimalValue>(d)) == Ordering::Equal);
    Q_ASSERT(!isOk(parseDecimal(QStringView{QStringLiteral("1.0000000000000")})));
    qInfo() << "value_tests: PASS";
    return 0;
}

