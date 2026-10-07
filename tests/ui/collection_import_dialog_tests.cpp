// Offscreen preview/confirmation tests. All .savedValue inputs are parsed as
// data, and every persistence path is an isolated temporary directory.
#include "application/catalog_configuration.h"
#include "application/collection_task_import.h"
#include "catalog/skin_catalog.h"
#include "domain.h"
#include "fluenttheme.h"
#include "ui/dialogs/collection_import_dialog.h"
#include "ui/dialogs/task_editor_dialog.h"
#include "ui/pages/task_page.h"
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace {
int assertions = 0, failures = 0;
void check(bool passed, const char* name) {
    ++assertions;
    if (!passed) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
template<class T> T* child(QObject& parent, const char* name) {
    auto* value = parent.findChild<T*>(QString::fromLatin1(name));
    if (!value) { std::fprintf(stderr, "MISSING: %s\n", name); std::exit(2); }
    return value;
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray config(const AppState& state) {
    return QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson(QJsonDocument::Compact);
}
QByteArray header(int kind, quint64 size) {
    int exponent = 0;
    while (exponent < 3 && size >= (quint64(1) << (8 * (1 << exponent)))) ++exponent;
    QByteArray out(1, char(kind | (exponent << 6)));
    for (int i = 0; i < (1 << exponent); ++i) out.append(char((size >> (8 * i)) & 255));
    return out;
}
QByteArray encode(const QJsonValue& value) {
    if (value.isBool()) return QByteArray(1, char(value.toBool()));
    if (value.isDouble()) return header(4, quint64(value.toInteger()));
    if (value.isString()) { const auto b = value.toString().toUtf8(); return header(7, b.size()) + b; }
    if (value.isArray()) {
        auto out = header(3, value.toArray().size());
        for (const auto& item : value.toArray()) out += encode(item);
        return out;
    }
    const auto object = value.toObject();
    auto out = header(2, object.size());
    for (auto it = object.begin(); it != object.end(); ++it) out += encode(it.key()) + encode(it.value());
    return out;
}
QJsonArray row(int sourceRow, int product, int condition, bool enabled, int quantity) {
    QJsonArray out;
    const QJsonObject fields{{QStringLiteral("启用"), enabled}, {QStringLiteral("成色设置栏"), condition},
        {QStringLiteral("最低价格设置栏"), 10}, {QStringLiteral("最高价格设置栏"), 600},
        {QStringLiteral("枪名设置栏"), product}, {QStringLiteral("磨损度设置栏"), 5},
        {QStringLiteral("限量设置栏"), quantity}};
    for (auto it = fields.begin(); it != fields.end(); ++it)
        out.append(QJsonArray{it.key() + QStringLiteral("_") + QString::number(sourceRow), it.value()});
    return out;
}
QByteArray sample() {
    QJsonArray rows = row(0, 10602, 0, true, 0);
    for (const auto& item : row(3, 10100, 1, false, 9)) rows.append(item);
    return encode(QJsonObject{{QStringLiteral("自动收藏-任务配置"), rows}});
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    const auto fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + QStringLiteral("/Fonts/");
    for (const auto& name : {QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc"),
         QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"), QStringLiteral("SegoeIcons.ttf"),
         QStringLiteral("segmdl2.ttf")}) QFontDatabase::addApplicationFont(fonts + name);
#endif
    app.setStyle(QStringLiteral("Fusion"));
    app.setPalette(FluentTheme::palette());
    app.setFont(FluentTheme::font(13));
    const QString sourceDirectory = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
    app.setStyleSheet(QString::fromUtf8(read(QDir(sourceDirectory).filePath(QStringLiteral("../../src/theme.qss")))));
    using namespace relink::application;
    using namespace relink::ui;
    const QJsonObject dictionary = QJsonDocument::fromJson(argc > 1 ? read(QString::fromLocal8Bit(argv[1])) : QByteArray()).object();
    relink::catalog::Catalog catalog;
    QString error;
    check(!dictionary.isEmpty(), "reference dictionary loads");
    check(relink::catalog::parseCatalog(argc > 2 ? read(QString::fromLocal8Bit(argv[2])) : QByteArray(), &catalog, &error), "verified skin catalog loads");
    if (dictionary.isEmpty() || catalog.skins.isEmpty()) return 2;
    AppState state;
    check(applyCatalogConfiguration(state, catalog, nullptr, &error), "real catalog projection");
    state.skins[0].followed = true;
    Task existing;
    existing.id = QStringLiteral("manual-existing"); existing.name = QStringLiteral("保留原任务");
    existing.skinId = state.skins[0].id; existing.quantity = 2;
    state.tasks.append(existing);
    const QByteArray before = config(state);
    int modelChanges = 0;
    QObject::connect(&state, &AppState::changed, &app, [&] { ++modelChanges; });
    const QByteArray bytes = sample();
    CollectionImportDialog dialog(dictionary, &catalog, &state);
    auto* table = child<QTableWidget>(dialog, "collectionImportTable");
    auto* commit = child<QPushButton>(dialog, "collectionImportCommitButton");
    check(!dialog.isVisible() && table->rowCount() == 0 && !commit->isEnabled(), "constructor stays hidden and empty");
    check(dialog.loadSavedValue(bytes, QStringLiteral("<tasks>.savedValue"), &error), "valid source preview loads");
    check(dialog.preview().rowCount == 2 && dialog.preview().enabledCount == 1
          && dialog.preview().distinctProducts == 1 && dialog.preview().totalDistinctProducts == 2,
          "enabled disabled and product counts preserved");
    check(dialog.preview().addedCount == 2 && dialog.preview().duplicateCount == 0 && table->rowCount() == 2,
          "all source rows previewed including disabled");
    check(config(state) == before && modelChanges == 0, "preview leaves model and original follows unchanged");
    auto* fileName = child<QLabel>(dialog, "collectionImportFile");
    check(fileName->text() == QStringLiteral("<tasks>.savedValue") && fileName->textFormat() == Qt::PlainText,
          "source filename rendered as plain text");
    check(child<QLabel>(dialog, "collectionImportHash")->text().contains(dialog.preview().sourceSha256)
          && dialog.preview().sourceSha256.size() == 64, "full source hash visible");
    check(table->item(0, 0)->text() == "1" && table->item(1, 0)->text() == "4", "original source row numbers retained");
    check(table->item(0, 1)->text() == "S6" && table->item(0, 2)->data(Qt::UserRole).toString() == "10602",
          "season and stable product identity visible");
    check(table->item(1, 3)->text().contains(QStringLiteral("极品")) && table->item(1, 3)->text().contains(QStringLiteral("红色")),
          "menu color and variant remain separate from condition");
    check(table->item(0, 4)->text() == QStringLiteral("成色S") && table->item(1, 4)->text() == QStringLiteral("成色A"),
          "condition IDs resolve without quality inference");
    check(table->item(0, 5)->text() == "10" && table->item(0, 6)->text() == "600" && table->item(0, 7)->text() == "5",
          "raw numeric conditions rendered without invented values");
    check(table->item(0, 8)->text() == QStringLiteral("不限") && table->item(1, 8)->text() == "9"
          && table->item(1, 9)->text() == QStringLiteral("停用"), "zero unlimited and disabled rows remain intact");
    check(table->editTriggers() == QAbstractItemView::NoEditTriggers, "preview table is read only");
    dialog.reject();
    check(config(state) == before && modelChanges == 0, "cancelled import changes nothing");

    CollectionImportDialog confirm(dictionary, &catalog, &state);
    check(confirm.loadSavedValue(bytes, QStringLiteral("tasks.savedValue"), &error), "confirmation source loads");
    int commits = 0;
    QObject::connect(&confirm, &CollectionImportDialog::commitRequested, &app, [&] { ++commits; });
    auto* confirmButton = child<QPushButton>(confirm, "collectionImportCommitButton");
    confirmButton->click();
    check(commits == 1 && confirm.result() != QDialog::Accepted && config(state) == before && modelChanges == 0,
          "confirmation requests durable commit without premature success or mutation");
    check(!confirmButton->isEnabled() && !child<QPushButton>(confirm, "collectionImportChooseButton")->isEnabled(),
          "commit in flight prevents double submission or replacing source");
    confirmButton->click();
    check(commits == 1, "in flight second click ignored");
    confirm.showCommitError(QStringLiteral("测试保存失败"));
    check(confirmButton->isEnabled() && confirm.sourceBytes() == bytes
          && child<QLabel>(confirm, "collectionImportMessage")->text() == QStringLiteral("测试保存失败"),
          "save failure retains preview and permits retry");
    QObject::connect(&confirm, &CollectionImportDialog::commitRequested, &app, [&] { confirm.accept(); });
    confirmButton->click();
    check(commits == 2 && confirm.result() == QDialog::Accepted && config(state) == before,
          "only owning commit callback closes accepted dialog");

    auto prepared = confirm.preview();
    const Task imported = prepared.rows[0].task;
    TaskEditorDialog editor(&state, &imported);
    auto* quantity = child<QSpinBox>(editor, "taskQuantity");
    check(quantity->minimum() == 0 && quantity->value() == 0 && quantity->text() == QStringLiteral("不限"),
          "task editor supports imported unlimited quantity");
    child<QLineEdit>(editor, "taskName")->setText(QStringLiteral("编辑导入任务"));
    child<QDoubleSpinBox>(editor, "taskMaxPrice")->setValue(567);
    child<QPushButton>(editor, "taskSaveButton")->click();
    check(editor.result() == QDialog::Accepted && editor.task().quantity == 0 && editor.task().maxPrice == 567,
          "editing preserves unlimited quantity and changed threshold");
    check(editor.task().importSource.sourceSha256 == imported.importSource.sourceSha256
          && editor.task().importSource.dictionarySha256 == imported.importSource.dictionarySha256
          && editor.task().importSource.fields == imported.importSource.fields
          && editor.task().importSource.row == imported.importSource.row,
          "task editing retains immutable source parameters and provenance");
    check(config(state) == before && modelChanges == 0, "task dialog draft does not mutate source model");

    CollectionImportDialog stale(dictionary, &catalog, &state);
    check(stale.loadSavedValue(bytes, QStringLiteral("tasks.savedValue"), &error), "stale preview starts ready");
    int staleCommits = 0;
    QObject::connect(&stale, &CollectionImportDialog::commitRequested, &app, [&] { ++staleCommits; });
    state.tasks = prepared.mergedTasks;
    state.tasks[1].maxPrice = 567;
    child<QPushButton>(stale, "collectionImportCommitButton")->click();
    check(staleCommits == 0 && stale.preview().addedCount == 0 && stale.preview().duplicateCount == 2,
          "confirmation revalidates and prevents duplicate append after state changes");
    check(!child<QPushButton>(stale, "collectionImportCommitButton")->isEnabled()
          && child<QLabel>(stale, "collectionImportMessage")->text().contains(QStringLiteral("没有新增")),
          "repeat import clearly reports no new rows");
    check(state.tasks[1].maxPrice == 567 && state.skins[0].followed, "reimport keeps edited task and original follow");
    TaskPage page(&state);
    page.refresh();
    check(child<QTableWidget>(page, "taskTable")->item(1, 5)->text() == QStringLiteral("不限"),
          "task list displays unlimited quantity consistently");
    int importIntents = 0;
    QObject::connect(&page, &TaskPage::importCollectionRequested, &app, [&] { ++importIntents; });
    child<QPushButton>(page, "taskImportCollectionButton")->click();
    check(importIntents == 1 && !state.simulationRunning, "task page import is an intent not a runner action");

    const QByteArray beforeFailure = config(state);
    check(!stale.loadSavedValue(QByteArray("broken"), QStringLiteral("bad.savedValue"), &error), "invalid source rejected");
    check(stale.sourceBytes().isEmpty() && stale.preview().rows.isEmpty()
          && child<QTableWidget>(stale, "collectionImportTable")->rowCount() == 0
          && !child<QPushButton>(stale, "collectionImportCommitButton")->isEnabled(),
          "invalid replacement clears stale preview and confirm state");
    check(config(state) == beforeFailure && modelChanges == 0, "all failure paths leave model unchanged");
    check(!stale.loadSavedValueFile(QStringLiteral("not-existing.savedValue"), &error) && !error.isEmpty(),
          "missing file returns visible diagnostic");
    CollectionImportDialog noModel(dictionary, nullptr, nullptr);
    check(!noModel.loadSavedValue(bytes, QStringLiteral("tasks.savedValue"), &error), "missing catalog fails without dereference");
    QTemporaryDir directory;
    check(directory.isValid(), "isolated test directory");
    const auto sourcePath = directory.filePath(QStringLiteral("任务.savedValue"));
    { QFile file(sourcePath); check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write isolated source"); }
    check(dialog.loadSavedValueFile(sourcePath, &error) && dialog.sourceName() == QStringLiteral("任务.savedValue"),
          "file picker backend accepts Unicode path and preserves basename");
    check(read(sourcePath) == bytes, "source input bytes never modified");

    if (argc > 3) {
        AppState actualState;
        check(applyCatalogConfiguration(actualState, catalog, nullptr, &error), "actual source catalog projection");
        CollectionImportDialog actual(dictionary, &catalog, &actualState);
        check(actual.loadSavedValueFile(QString::fromLocal8Bit(argv[3]), &error), "user supplied data previews offscreen");
        check(actual.preview().rowCount == 50 && actual.preview().enabledCount == 21
              && actual.preview().distinctProducts == 9 && actual.preview().totalDistinctProducts == 13
              && actual.preview().addedCount == 50,
              "user data exact 50 rows 21 enabled 9 active 13 total products");
        check(actualState.tasks.isEmpty(), "real source preview does not auto import or run");
        if (argc > 4) {
            const QString snapshot = QString::fromLocal8Bit(argv[4]);
            QDir().mkpath(QFileInfo(snapshot).absolutePath());
            actual.show(); app.processEvents();
            check(actual.grab().save(snapshot), "actual import offscreen screenshot");
            actual.hide();
        }
    }
    std::printf("COLLECTION_IMPORT_DIALOG_TESTS=%s; assertions=%d; failures=%d; offscreen=true; game_connected=false\n",
                failures == 0 ? "PASS" : "FAIL", assertions, failures);
    return failures == 0 ? 0 : 1;
}
