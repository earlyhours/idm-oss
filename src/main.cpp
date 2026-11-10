#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include "downloadmanager.h"
#include "browserbridge.h"
#include "automation.h"
#include "theme.h"
#include "portalpicker.h"
#include "translator.h"

static void printJson(const QVariantMap &value, bool error = false) {
    const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(value)).toJson(QJsonDocument::Compact);
    fprintf(error ? stderr : stdout, "%s\n", bytes.constData());
}

int main(int argc, char **argv) {
    const bool headless = argc > 1 && (QString(argv[1]) == "download" || QString(argv[1]) == "skill" || QString(argv[1]) == "browser-host" || QString(argv[1]) == "browser-install");
    if (headless) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setApplicationName("idm-native");
    app.setApplicationVersion("0.2.1");
    app.setOrganizationName("idm-native");
    app.setDesktopFileName("idm-native");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native HTTP download manager for Omarchy");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "open, download URL, browser-host, browser-install, or skill install", "[command] [URL]");
    parser.addOptions({{{"d", "directory"}, "Download directory", "path"},
                       {{"c", "connections"}, "Parallel connections (1-16)", "count", "4"},
                       {"json", "Print machine-readable results"},
                       {"state-directory", "Queue state directory", "path"},
                       {"smoke-test", "Render GUI to a PNG and quit", "path"}});
    parser.process(app);
    const auto args = parser.positionalArguments();
    const QString command = args.value(0, "open");
    if (command == "skill" && args.value(1) == "install") {
        const QString path = QDir::homePath() + "/.agents/skills/idm-native/SKILL.md";
        QFile source(":/agent-skill.md");
        if (QFile::exists(path) || !QDir().mkpath(QFileInfo(path).absolutePath()) || !source.copy(path)) {
            printJson({{"error", "Could not install skill; an existing skill is never overwritten"}}, true);
            return 1;
        }
        printJson({{"path", path}});
        return 0;
    }
    if (command != "open" && command != "download" && command != "browser-host" && command != "browser-install") { parser.showHelp(1); }
    QString state = parser.value("state-directory");
    if (state.isEmpty()) state = qEnvironmentVariable("IDM_NATIVE_STATE_DIRECTORY");
    if (state.isEmpty()) state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(state)) { printJson({{"error", "Cannot create queue directory"}}, true); return 1; }
    if (command == "browser-host") return BrowserBridge::nativeHost(state, QCoreApplication::applicationFilePath());
    if (command == "browser-install") {
        BrowserBridge installer(state, {}, false);
        const auto path = installer.install();
        printJson(path.isEmpty() ? QVariantMap{{"error",installer.error()}} : QVariantMap{{"extensionDirectory",path}}, path.isEmpty());
        return path.isEmpty() ? 1 : 0;
    }
    DownloadManager manager(state);
    if (command == "download" && !manager.error().isEmpty()) { printJson({{"error", manager.error()}}, true); return 1; }
    if (command == "download") {
        bool valid;
        const int connections = parser.value("connections").toInt(&valid);
        if (!valid || connections < 1 || connections > 16 || args.size() != 2) {
            printJson({{"error", "Use download URL with 1-16 connections"}}, true); return 1;
        }
        QString id;
        auto check = [&] {
            for (const auto &item : manager.downloads()) {
                const auto row = item.toMap();
                if (row.value("id").toString() != id) continue;
                const auto status = row.value("status").toString();
                if (status == "complete" || status == "error") {
                    printJson(row, status == "error");
                    app.exit(status == "complete" ? 0 : 1);
                }
            }
        };
        QObject::connect(&manager, &DownloadManager::changed, &app, check);
        QTimer::singleShot(0, &app, [&] {
            id = manager.add(args[1], parser.value("directory"), connections);
            if (id.isEmpty()) { printJson({{"error", manager.error()}}, true); app.exit(1); }
            else check();
        });
        return app.exec();
    }
    Automation automation(&manager,state);
    auto acceptRequest = [&](const QVariantMap &message) -> QVariantMap {
        auto add = [&](QVariantMap request) {
            if (!request.contains("directory") && !qEnvironmentVariableIsEmpty("IDM_NATIVE_DOWNLOAD_DIRECTORY"))
                request.insert("directory",qEnvironmentVariable("IDM_NATIVE_DOWNLOAD_DIRECTORY"));
            return automation.enqueue(request);
        };
        if (message.value("command")=="download") {
            const auto id=add(message.value("request").toMap());
            return id.isEmpty() ? QVariantMap{{"ok",false},{"error",automation.error()}} : QVariantMap{{"ok",true},{"id",id}};
        }
        const auto requests=message.value("requests").toList();
        if (requests.isEmpty() || requests.size()>100) return {{"ok",false},{"error","Batch must contain 1-100 requests."}};
        QVariantList ids,errors;
        for (const auto &request:requests) {
            const auto id=add(request.toMap());
            if (id.isEmpty()) errors.append(automation.error()); else ids.append(id);
        }
        return {{"ok",!ids.isEmpty()},{"ids",ids},{"errors",errors}};
    };
    BrowserBridge browserBridge(state,acceptRequest,manager.error().isEmpty());
    QQuickStyle::setStyle("Basic");
    Translator translator;
    app.installTranslator(&translator);
    Theme theme;
    PortalPicker picker;
    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("picker", &picker);
    engine.rootContext()->setContextProperty("browserBridge", &browserBridge);
    engine.rootContext()->setContextProperty("automation", &automation);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &manager);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    if (parser.isSet("smoke-test")) {
        QTimer::singleShot(1200, &app, [&] {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            app.exit(window && window->grabWindow().save(parser.value("smoke-test")) ? 0 : 1);
        });
    }
    return app.exec();
}
