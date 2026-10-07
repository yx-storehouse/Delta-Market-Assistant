#include "domain.h"
#include "mainwindow.h"
#include "fluenttheme.h"
#include "application/startup_config.h"
#include "application/workspace/workspace_controller.h"
#include "diagnostics/ui_self_test_runner.h"
#include "ledger/storage_self_test.h"
#include "diagnostics/live_capture_check.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv) {
    // Explicit diagnostic uses QCoreApplication and never creates the main UI.
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--live-capture-check")
            return relink::diagnostics::runLiveCaptureCheck(argc, argv);
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        headless |= a == "--self-test" || a == "--snapshot-dir" || a == "--write-demo-config" || a == "--validate-config"
            || a == "--export-app-icon" || a == "--storage-self-test";
        headless |= a.startsWith("--snapshot-dir=") || a.startsWith("--write-demo-config=") || a.startsWith("--validate-config=");
    }
    if (headless) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        bool validScale = false;
        const QByteArray requestedScale = qgetenv("RELINK_TEST_SCALE");
        const double testScale = requestedScale.toDouble(&validScale);
        qputenv("QT_SCALE_FACTOR", validScale && testScale > 0.0 && testScale <= 4.0
            ? requestedScale : QByteArray("1"));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }
    QApplication app(argc, argv);
    app.setOrganizationName("RelinkStudio");
    app.setApplicationName(headless ? "RelinkStudioOffscreenTest" : "RelinkStudio");
    app.setApplicationVersion("0.6.0");
    app.setStyle("Fusion");
    // The generic offscreen font database does not enumerate Windows fonts.
    // Load installed system fonts for memory-only Chinese rendering; no redistribution.
    if (headless || app.platformName() == QStringLiteral("offscreen")) {
        const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
        for (const QString& name : {QString("msyh.ttc"), QString("msyhbd.ttc"), QString("segoeui.ttf"), QString("seguisb.ttf"),
                                    QString("SegoeIcons.ttf"), QString("segmdl2.ttf"),
                                    QString("seguisym.ttf"), QString("consola.ttf")})
            QFontDatabase::addApplicationFont(fonts + name);
    }
    app.setPalette(FluentTheme::palette());
    FluentTheme::installNativeFrames(app);
    QFile theme(":/theme.qss");
    if (theme.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(theme.readAll()));
    QCommandLineParser parser;
    parser.setApplicationDescription("Relink Studio offline frontend. No input automation. Explicit --live-capture-check provides read-only diagnostics.");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"self-test", "Run offscreen UI interaction checks and exit."});
    parser.addOption({"storage-self-test", "Verify the packaged SQLite driver, transactions, recovery and backup using temporary data."});
    parser.addOption({"snapshot-dir", "Render seven pages to PNG, offscreen, then exit.", "directory"});
    parser.addOption({"config", "Local demo configuration JSON path.", "path"});
    parser.addOption({"workspace-dir", "Directory for reviewed profiles and the persistent replay ledger.", "directory"});
    parser.addOption({"workspace-read-only", "Open an existing business workspace without changing it."});
    parser.addOption({"write-demo-config", "Write a fresh synthetic configuration and exit offscreen.", "path"});
    parser.addOption({"validate-config", "Validate a local demo configuration and exit offscreen.", "path"});
    parser.addOption({"font-report", "Print the resolved UI and icon fonts, then exit without opening a window."});
    parser.addOption({"export-app-icon", "Render the 256 px application icon to a PNG offscreen, then exit.", "path"});
    parser.process(app);
    if (parser.isSet("storage-self-test")) return relink::ledger::runStorageSelfTest();
    if (parser.isSet("font-report")) {
        const auto report = [](const char* role, const QFont& font) {
            const QFontInfo info(font);
            std::printf("FONT_%s=%s; pixel_size=%d; weight=%d\n", role, info.family().toUtf8().constData(),
                        info.pixelSize(), static_cast<int>(info.weight()));
        };
        report("UI", FluentTheme::font(14));
        report("UI_SEMIBOLD", FluentTheme::font(14, QFont::DemiBold));
        report("DISPLAY", FluentTheme::displayFont(28));
        report("ICON", FluentTheme::iconFont(16));
        for (const QString& family : QFontDatabase::families())
            if (family.startsWith(QStringLiteral("Segoe UI")))
                std::printf("FAMILY=%s; styles=%s\n", family.toUtf8().constData(),
                            QFontDatabase::styles(family).join(QStringLiteral(",")).toUtf8().constData());
        std::printf("ICON_FONT_AVAILABLE=%s; platform=%s\n", FluentTheme::hasIconFont() ? "true" : "false",
                    app.platformName().toUtf8().constData());
        std::fflush(stdout);
        return 0;
    }
    AppState state;
    if (parser.isSet("export-app-icon")) {
        QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        FluentTheme::paintAppIcon(&painter, QRectF(0, 0, 256, 256));
        painter.end();
        const bool ok = image.save(parser.value("export-app-icon"));
        std::printf("APP_ICON=%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (parser.isSet("validate-config")) {
        QString error;
        if (!state.loadFrom(parser.value("validate-config"), &error)) {
            std::printf("CONFIG_VALIDATION=FAIL\n");
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        std::printf("CONFIG_VALIDATION=PASS; skins=%lld; tasks=%lld; first_max_price=%.2f; first_quantity=%d\n",
                    static_cast<long long>(state.skins.size()), static_cast<long long>(state.tasks.size()),
                    state.tasks.isEmpty() ? 0.0 : state.tasks.first().maxPrice,
                    state.tasks.isEmpty() ? 0 : state.tasks.first().quantity);
        return 0;
    }
    if (parser.isSet("write-demo-config")) {
        const QString path = QFileInfo(parser.value("write-demo-config")).absoluteFilePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QString error;
        const bool ok = state.saveTo(path, &error);
        std::printf("DEMO_CONFIG=%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    QString config = parser.value("config");
    if (config.isEmpty()) {
        config = headless ? QDir::tempPath() + "/RelinkStudioOffscreen/config.json"
                          : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/config.json";
    }
    config = relink::application::prepareStartupConfig(state, config, !headless);
    // Headless runs get a fresh, temporary workspace unless the caller supplies
    // an explicit test directory; never open the normal user's ledger in tests.
    QTemporaryDir temporaryWorkspace;
    QString workspacePath = parser.value("workspace-dir");
    if (workspacePath.isEmpty()) {
        if (headless) {
            if (!temporaryWorkspace.isValid()) {
                std::fprintf(stderr, "WORKSPACE_TEMP_DIRECTORY=FAIL\n");
                return 1;
            }
            workspacePath = temporaryWorkspace.path();
        } else {
            workspacePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/business");
        }
    }
    relink::workspace::WorkspaceController workspace;
    if (!workspace.open(workspacePath, parser.isSet("workspace-read-only")))
        std::fprintf(stderr, "WORKSPACE_OPEN_ERROR: %s\n", workspace.lastError().toUtf8().constData());
    MainWindow window(&state, nullptr, &workspace);
    window.show(); // Offscreen platform renders to memory only when headless=true.
    if (!headless) {
        QObject::connect(&app, &QApplication::aboutToQuit, &state, [&state, &window]() {
            state.pauseSimulation();
            if (window.property("skipAutomaticConfigSave").toBool()) return;
            QString error;
            if (!state.saveTo(state.configPath, &error))
                std::fprintf(stderr, "CONFIG_SAVE_ERROR: %s\n", error.toUtf8().constData());
        });
        return app.exec();
    }
    const QString output = parser.value("snapshot-dir").isEmpty()
        ? QFileInfo(config).absolutePath() + "/snapshots" : QFileInfo(parser.value("snapshot-dir")).absoluteFilePath();
    return relink::diagnostics::runUiSelfTest(app, window, state, workspace,
        {config, output, parser.isSet("self-test")});
}
