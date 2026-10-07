#pragma once
#include <QString>
class QApplication;
namespace relink::diagnostics { int runCollectionImportSelfTest(QApplication&, const QString& output, const QString& source); }
