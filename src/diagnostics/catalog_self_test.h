#pragma once
#include <QString>
class QApplication;
namespace relink::diagnostics { int runCatalogSelfTest(QApplication&, const QString& snapshots); }
