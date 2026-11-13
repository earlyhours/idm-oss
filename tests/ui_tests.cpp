#include <QtTest>
#include <QFile>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQmlProperty>
#include <QColor>
#include <cmath>
#include <algorithm>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "downloadmanager.h"
#include "automation.h"
#include "theme.h"
#include "portalpicker.h"
#include "translator.h"

static QStringList qmlWarnings;
static QtMessageHandler previousHandler = nullptr;

static void collectMessages(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg && (message.contains("QML") || message.contains("qrc:/Main.qml")))
        qmlWarnings << message;
    if (previousHandler) previousHandler(type, context, message);
}

static bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

static bool capture(QQuickWindow *window, const QString &path) {
    const auto image = window->grabWindow();
    const auto directory = qEnvironmentVariable("IDM_UI_SCREENSHOT_DIR");
    if (directory.isEmpty()) return image.save(path);
    QString name = QFileInfo(path).fileName();
    name.replace("idm-native-", "desktop-").replace("idm-classic-", "desktop-").replace("idm-custom-", "desktop-custom-");
    return QDir().mkpath(directory) && image.save(QDir(directory).filePath(name));
}

static double contrast(const QColor &foreground, const QColor &background) {
    auto luminance = [](const QColor &color) {
        auto channel = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
        return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) + .0722 * channel(color.blueF());
    };
    const double a = luminance(foreground), b = luminance(background);
    return (std::max(a, b) + .05) / (std::min(a, b) + .05);
}

static QObject *withProperty(QObject *root, const char *property, const QVariant &value) {
    if (root->property(property) == value) return root;
    for (auto *child : root->children())
        if (auto *found = withProperty(child, property, value)) return found;
    if (auto *item = qobject_cast<QQuickItem *>(root))
        for (auto *child : item->childItems())
            if (child->parent() != root)
                if (auto *found = withProperty(child, property, value)) return found;
    return nullptr;
}

static QStringList columnKeys(QObject *window) {
    QStringList keys;
    const auto columns = window->property("columns").value<QJSValue>().toVariant().toList();
    for (const auto &column : columns) keys << column.toMap().value("key").toString();
    return keys;
}

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void realWindowAndDownload() {
        qmlWarnings.clear();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(QDir().mkpath(temporary.path() + "/theme"));
        const auto languagePath = temporary.path() + "/language";
        const auto colorsPath = temporary.path() + "/theme/colors.toml";
        QVERIFY(writeFile(languagePath, "en\n"));
        QVERIFY(writeFile(colorsPath, "accent = '#F8C100'\n"));
        Theme theme(temporary.path());
        Translator translator(languagePath);
        qApp->installTranslator(&translator);
        DownloadManager manager(temporary.path() + "/state");
        QList<QStringList> commands;
        Automation automation(&manager, temporary.path() + "/state", nullptr,
                              [&](const QStringList &command, std::function<void(int,const QString &)> done) {
            commands.append(command); done(0, {});
        });
        QVERIFY(automation.configure({{"directory",temporary.path()+"/output"},{"notifications",false},{"sound",false}}));
        PortalPicker picker;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("picker", &picker);
        engine.rootContext()->setContextProperty("automation", &automation);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &manager);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        const QList<QPair<QString, QString>> sourceHosts{
            {"https://user:secret@downloads.example:8443/private/file.zip?token=hidden#fragment", "downloads.example"},
            {"http://127.0.0.1:8080/file", "127.0.0.1"},
            {"https://user:secret@[2001:db8::1]:8443/private?token=hidden", "[2001:db8::1]"},
            {"invalid URL", "Other"}
        };
        for (const auto &[url, host] : sourceHosts) {
            QVariant result;
            const QVariant input = QVariantMap{{"url", url}, {"category", "Other"}};
            QVERIFY(QMetaObject::invokeMethod(window, "sourceHost", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, input)));
            QCOMPARE(result.toString(), host);
        }
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QVERIFY(window->findChild<QObject *>("downloadTable"));
        auto *fileTypesGroup = qobject_cast<QQuickItem *>(withProperty(window, "objectName", "navigation-types"));
        auto *queuesGroup = qobject_cast<QQuickItem *>(withProperty(window, "objectName", "navigation-queues"));
        QVERIFY(fileTypesGroup && queuesGroup);
        QVERIFY(!window->property("filesExpanded").toBool());
        QVERIFY(!window->property("queuesExpanded").toBool());
        fileTypesGroup->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(window->property("filesExpanded").toBool());
        QCOMPARE(window->property("filter").toString(), "all");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->property("filesExpanded").toBool());
        queuesGroup->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Enter);
        QTRY_VERIFY(window->property("queuesExpanded").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!window->property("queuesExpanded").toBool());
        QCOMPARE(window->property("filter").toString(), "all");
        QCOMPARE(window->title(), QString("IDM Native"));
        QCOMPARE(columnKeys(window), QStringList({"name", "status", "total", "speed"}));
        auto *advancedColumns = window->findChild<QObject *>("advancedColumnsAction");
        QVERIFY(advancedColumns);
        QVERIFY(!advancedColumns->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(advancedColumns, "trigger", Q_ARG(QObject *, nullptr)));
        QTRY_VERIFY(advancedColumns->property("checked").toBool());
        const auto expandedColumns = columnKeys(window);
        QCOMPARE(expandedColumns.size(), 7);
        QVERIFY(expandedColumns.contains("eta"));
        QVERIFY(expandedColumns.contains("created"));
        QVERIFY(expandedColumns.contains("description"));
        QVERIFY(QMetaObject::invokeMethod(advancedColumns, "trigger", Q_ARG(QObject *, nullptr)));
        QTRY_VERIFY(!advancedColumns->property("checked").toBool());
        QCOMPARE(columnKeys(window), QStringList({"name", "status", "total", "speed"}));
        QCOMPARE(theme.accent(), "#F8C100");
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-empty.png"));

        fileTypesGroup->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(window->property("filesExpanded").toBool());
        auto *documentsItem = qobject_cast<QQuickItem *>(withProperty(window, "objectName", "navigation-Documents"));
        QVERIFY(documentsItem);
        QTRY_VERIFY(documentsItem->isVisible());
        documentsItem->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(window->property("filter").toString(), QString("Documents"));
        window->setProperty("filter", "all");
        fileTypesGroup->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->property("filesExpanded").toBool());
        window->contentItem()->forceActiveFocus();

        auto *emptyState = window->findChild<QQuickItem *>("emptyState");
        auto *addDownload = window->findChild<QQuickItem *>("addDownloadButton");
        QVERIFY(emptyState && addDownload);
        QTRY_VERIFY(emptyState->isVisible());
        QTRY_VERIFY(addDownload->isVisible() && addDownload->isEnabled());
        auto *primaryText = addDownload->property("contentItem").value<QQuickItem *>();
        auto *primaryBackground = addDownload->property("background").value<QQuickItem *>();
        QVERIFY(primaryText && primaryBackground);
        QVERIFY(contrast(primaryText->property("color").value<QColor>(), primaryBackground->property("color").value<QColor>()) >= 4.5);
        addDownload->setProperty("down", true);
        QVERIFY(contrast(primaryText->property("color").value<QColor>(), primaryBackground->property("color").value<QColor>()) >= 4.5);
        addDownload->setProperty("down", false);
        QVERIFY(contrast(window->property("accent").value<QColor>(), window->property("selection").value<QColor>()) >= 4.5);
        const QRectF headerButton = addDownload->mapRectToScene(addDownload->boundingRect());
        QVERIFY(headerButton.width() > 0 && headerButton.height() > 0);
        QVERIFY2(QRectF(0, 0, window->width(), window->height()).contains(headerButton), "The header Add URL button must be visible inside the window.");
        auto *toolbar = window->findChild<QQuickItem *>("downloadToolbar");
        auto *moreButton = window->findChild<QQuickItem *>("moreActionsButton");
        QVERIFY(toolbar && moreButton);
        for (const int width : {820, 1220}) {
            window->setWidth(width);
            QTest::qWait(50);
            QRectF previous;
            for (auto *child : toolbar->childItems()) {
                if (!child->isVisible() || child->height() <= 0) continue;
                const auto rect = child->mapRectToScene(child->boundingRect());
                QVERIFY2(QRectF(0, 0, window->width(), window->height()).contains(rect), qPrintable(QString("Toolbar child %1 %2: x=%3 y=%4 width=%5 height=%6 window=%7x%8").arg(child->metaObject()->className()).arg(child->objectName()).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(window->width()).arg(window->height())));
                QVERIFY2(previous.isNull() || previous.right() <= rect.left(), "Toolbar actions must not overlap.");
                previous = rect;
            }
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, moreButton->mapToScene(moreButton->boundingRect().center()).toPoint());
        auto *moreMenu = window->findChild<QObject *>("moreActionsMenu");
        QVERIFY(moreMenu);
        QTRY_VERIFY(moreMenu->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QVERIFY(moreMenu->property("currentIndex").toInt() >= 0);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-more-menu.png"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!moreMenu->property("visible").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, headerButton.center().toPoint());
        auto *input = window->findChild<QQuickItem *>("urlInput");
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
        auto *urlOptions = window->findChild<QQuickItem *>("urlOptionsDisclosure");
        auto *authorization = window->findChild<QQuickItem *>("useAuthorization");
        QVERIFY(urlOptions && authorization);
        QVERIFY(!urlOptions->property("checked").toBool());
        QVERIFY(!authorization->isVisible());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-url-quiet.png"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, urlOptions->mapToScene(urlOptions->boundingRect().center()).toPoint());
        QTRY_VERIFY(urlOptions->property("checked").toBool());
        QTRY_VERIFY(authorization->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, urlOptions->mapToScene(urlOptions->boundingRect().center()).toPoint());
        QTRY_VERIFY(!authorization->isVisible());
        input->forceActiveFocus();
        for (const char character : QByteArray("https://example.test/q?query=q q"))
            QTest::keyClick(window, character);
        QCOMPARE(input->property("text").toString(), "https://example.test/q?query=q q");
        QVERIFY(window->isVisible());
        QVERIFY(!window->property("helpVisible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);

        QSignalSpy languageChanges(&translator, &Translator::changed);
        QVERIFY(writeFile(languagePath, "da\n"));
        QTRY_COMPARE(translator.language(), "da");
        QVERIFY(languageChanges.count() > 0);
        QTRY_VERIFY(withProperty(window, "text", QString::fromUtf8("Overførsler")));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-danish.png"));

        QVERIFY(writeFile(languagePath, "ar\n"));
        QTRY_COMPARE(translator.language(), "ar");
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::RightToLeft);
        QTRY_VERIFY(withProperty(window, "text", "Downloads"));
        QVERIFY(writeFile(languagePath, "en\n"));
        QTRY_COMPARE(translator.language(), "en");
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);

        QSignalSpy themeChanges(&theme, &Theme::changed);
        QVERIFY(writeFile(colorsPath, "accent = '#39C990'\n"));
        QTRY_COMPARE(theme.accent(), "#39C990");
        QVERIFY(themeChanges.count() > 0);
        QCOMPARE(window->property("accent").value<QColor>(), QColor("#ef5a00"));
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        auto *fieldBackground = input->property("background").value<QQuickItem *>();
        QVERIFY(fieldBackground);
        QTRY_COMPARE(QQmlProperty(fieldBackground, "border.color").read().value<QColor>(), QColor("#39C990"));
        QTest::keyClick(window, Qt::Key_Escape);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray payload("UI transfer fixture\n");
        int requests = 0;
        int authorizationChallenges = 0;
        int basicAttempts = 0;
        QVariantMap siteRequestHeaders, jobRequestHeaders;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket, payload, &requests, &authorizationChallenges, &basicAttempts, &siteRequestHeaders, &jobRequestHeaders] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    ++requests;
                    if (request.toLower().contains("authorization:")) ++basicAttempts;
                    if (request.startsWith("GET /auth.bin ") && !request.contains("Basic dXNlcjpwYXNz")) {
                        ++authorizationChallenges;
                        socket->write("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    const bool perSite = request.startsWith("GET /site-headers.bin ");
                    const bool perJob = request.startsWith("GET /job-headers.bin ");
                    if (perSite || perJob) {
                        QVariantMap headers;
                        for (const auto &line : request.split('\n')) {
                            const int colon = line.indexOf(':');
                            if (colon > 0) headers[QString::fromLatin1(line.left(colon).toLower())] = QString::fromLatin1(line.mid(colon+1).trimmed());
                        }
                        if (perSite) siteRequestHeaders = headers; else jobRequestHeaders = headers;
                        if (headers.value("x-api-key").toString() != (perSite ? "fixture-site-key" : "fixture-job-key")) {
                            socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            socket->disconnectFromHost();
                            return;
                        }
                    }
                    const bool head = request.startsWith("HEAD ");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(payload.size())
                                  + "\r\nConnection: close\r\n\r\n" + (head ? QByteArray() : payload));
                    socket->disconnectFromHost();
                });
            }
        });
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        const auto url = QString("http://127.0.0.1:%1/ui.bin").arg(server.serverPort());
        input->setProperty("text", url);
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-url.png"));
        QTest::keyClick(window, Qt::Key_Return);
        auto *information = window->findChild<QObject *>("infoDialog");
        QVERIFY(information);
        QTRY_VERIFY(information->property("visible").toBool());
        auto *infoOptions = window->findChild<QQuickItem *>("infoOptionsDisclosure");
        auto *address = window->findChild<QQuickItem *>("addressInput");
        QVERIFY(infoOptions && address);
        QVERIFY(!infoOptions->property("checked").toBool());
        QVERIFY(!address->isVisible());
        auto *directory = window->findChild<QObject *>("directoryInput");
        QVERIFY(directory);
        directory->setProperty("text", temporary.path() + "/output");
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-info.png"));
        auto *infoMedia = window->findChild<QObject *>("infoMedia");
        auto *mediaFormat = window->findChild<QQuickItem *>("mediaFormat");
        QVERIFY(infoMedia && mediaFormat);
        infoMedia->setProperty("checked", true);
        QTRY_VERIFY(infoOptions->property("checked").toBool());
        QTRY_VERIFY(address->isVisible());
        auto *qualityScroll = window->findChild<QObject *>("infoScroll");
        QVERIFY(qualityScroll);
        auto *qualityContent = qualityScroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(qualityContent);
        QTest::qWait(50);
        const qreal qualityY = mediaFormat->mapToItem(qualityContent, QPointF()).y() + qualityContent->property("contentY").toReal();
        qualityContent->setProperty("contentY", std::clamp(qualityY, 0.0, std::max(0.0, qualityContent->property("contentHeight").toReal() - qualityContent->height())));
        QTRY_VERIFY(mediaFormat->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, mediaFormat->mapToScene(mediaFormat->boundingRect().center()).toPoint());
        auto *qualityPopup = mediaFormat->property("popup").value<QObject *>();
        QVERIFY(qualityPopup);
        QTRY_VERIFY(qualityPopup->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-quality-menu.png"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!qualityPopup->property("visible").toBool());
        QCOMPARE(mediaFormat->property("currentIndex").toInt(), 1);
        QCOMPARE(mediaFormat->property("currentText").toString(), "1080p");
        infoMedia->setProperty("checked", false);
        auto *start = window->findChild<QObject *>("startDownloadButton");
        QVERIFY(start);
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_COMPARE(manager.downloads().size(), 1);
        QTRY_VERIFY(!emptyState->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(manager.downloads().first().toMap().value("status").toString(), "complete", 10000);
        QFile downloaded(manager.downloads().first().toMap().value("path").toString());
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), payload);
        QTRY_VERIFY(withProperty(window, "text", "ui.bin"));
        auto *transfer = window->findChild<QObject *>("transferDialog");
        QVERIFY(transfer);
        QTRY_VERIFY(transfer->property("visible").toBool());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-transfer.png"));
        auto *transferDetails = window->findChild<QQuickItem *>("transferDetailsDisclosure");
        auto *transferConnections = window->findChild<QQuickItem *>("transferConnections");
        QVERIFY(transferDetails && transferConnections);
        QVERIFY(!transferDetails->property("checked").toBool());
        QVERIFY(!transferConnections->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, transferDetails->mapToScene(transferDetails->boundingRect().center()).toPoint());
        QTRY_VERIFY(transferDetails->property("checked").toBool());
        QTRY_VERIFY(transferConnections->isVisible());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-transfer-details.png"));
        QVERIFY(QMetaObject::invokeMethod(transfer, "close"));
        QVERIFY(writeFile(colorsPath, "accent = '#FFD60A'\n"));
        QTRY_COMPARE(theme.accent(), "#FFD60A");
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-download.png"));
        auto *options = window->findChild<QObject *>("optionsDialog");
        QVERIFY(options);
        QVERIFY(QMetaObject::invokeMethod(options, "open"));
        auto *darkToggle = window->findChild<QQuickItem *>("darkModeToggle");
        QVERIFY(darkToggle);
        QTRY_VERIFY(darkToggle->isVisible());
        window->findChild<QObject *>("optionsTabs")->setProperty("currentIndex", 6);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-browser-integration.png"));
        auto *browserSetup = window->findChild<QQuickItem *>("browserSetupDisclosure");
        auto *browserInstall = window->findChild<QQuickItem *>("browserInstallButton");
        auto *browserInstructions = window->findChild<QQuickItem *>("browserSetupInstructions");
        QVERIFY(browserSetup && browserInstall && browserInstructions);
        QVERIFY(!browserSetup->property("checked").toBool());
        QVERIFY(browserInstall->isVisible());
        QVERIFY(!browserInstructions->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, browserSetup->mapToScene(browserSetup->boundingRect().center()).toPoint());
        QTRY_VERIFY(browserSetup->property("checked").toBool());
        QTRY_VERIFY(browserInstall->isVisible());
        QTRY_VERIFY(browserInstructions->isVisible());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-browser-setup.png"));
        window->findChild<QObject *>("optionsTabs")->setProperty("currentIndex", 0);
        const bool initialDarkMode = window->property("darkMode").toBool();
        QVERIFY(initialDarkMode);
        const QColor initialCanvas = window->property("canvas").value<QColor>();
        QCOMPARE(window->color(), initialCanvas);
        QVERIFY(contrast(window->property("ink").value<QColor>(), initialCanvas) >= 4.5);
        const auto togglePoint = darkToggle->mapToScene(QPointF(darkToggle->width()/2,darkToggle->height()/2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, togglePoint);
        QTRY_COMPARE(window->property("darkMode").toBool(), !initialDarkMode);
        QTRY_COMPARE(window->color(), window->property("canvas").value<QColor>());
        QVERIFY(window->color() != initialCanvas);
        QVERIFY(contrast(window->property("ink").value<QColor>(), window->color()) >= 4.5);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, togglePoint);
        QTRY_COMPARE(window->property("darkMode").toBool(), initialDarkMode);
        QTRY_COMPARE(window->color(), initialCanvas);
        const bool storedDarkMode = automation.preferences().value("darkMode").toBool();
        QCOMPARE(initialDarkMode, storedDarkMode);
        for (const bool escape : {true, false}) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, togglePoint);
            QTRY_COMPARE(window->property("darkMode").toBool(), !storedDarkMode);
            QCOMPARE(automation.preferences().value("darkMode").toBool(), storedDarkMode);
            if (escape) {
                QTest::keyClick(window, Qt::Key_Escape);
            } else {
                auto *header = options->property("header").value<QQuickItem *>();
                QVERIFY(header);
                auto *close = qobject_cast<QQuickItem *>(withProperty(header, "text", "×"));
                QVERIFY(close);
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, close->mapToScene(close->boundingRect().center()).toPoint());
            }
            QTRY_VERIFY(!options->property("visible").toBool());
            QCOMPARE(window->property("darkMode").toBool(), storedDarkMode);
            QCOMPARE(automation.preferences().value("darkMode").toBool(), storedDarkMode);
            QTRY_COMPARE(window->color(), initialCanvas);
            QVERIFY(QMetaObject::invokeMethod(options, "open"));
            QTRY_VERIFY(darkToggle->isVisible());
        }
        QVERIFY(QMetaObject::invokeMethod(options, "close"));
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-dark.png"));
        window->setProperty("darkMode", false);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-light.png"));
        QMimeData mime;
        mime.setUrls({QUrl("https://example.test/one.zip"),QUrl("file:///etc/passwd"),QUrl("https://example.test/two.zip")});
        QDragEnterEvent enter(QPointF(400,300), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &enter);
        QDropEvent drop(QPointF(400,300), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &drop);
        auto *urlDialog = window->findChild<QObject *>("urlDialog");
        QVERIFY(urlDialog);
        QTRY_VERIFY(urlDialog->property("visible").toBool());
        QCOMPARE(input->property("text").toString(), "https://example.test/one.zip");
        QCOMPARE(window->property("droppedUrls").value<QJSValue>().toVariant().toList().size(), 1);
        QCOMPARE(manager.downloads().size(), 1);
        QTest::keyClick(window, Qt::Key_Escape);
        const int beforeLater = requests;
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        input->setProperty("text", QString("http://127.0.0.1:%1/later.zip").arg(server.serverPort()));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(information->property("visible").toBool());
        directory->setProperty("text", temporary.path() + "/output");
        window->findChild<QObject *>("descriptionInput")->setProperty("text", "Queued from file information dialog");
        auto *later = window->findChild<QObject *>("downloadLaterButton");
        QVERIFY(later);
        QVERIFY(QMetaObject::invokeMethod(later, "clicked"));
        QTRY_COMPARE(manager.downloads().size(), 2);
        QCOMPARE(manager.downloads().last().toMap().value("status").toString(), "paused");
        QCOMPARE(manager.downloads().last().toMap().value("description").toString(), "Queued from file information dialog");
        QTest::qWait(50);
        QCOMPARE(requests, beforeLater);
        QTRY_VERIFY(withProperty(window, "text", "later.zip"));
        auto *table = window->findChild<QQuickItem *>("downloadTable");
        QVERIFY(table);
        auto *label = qobject_cast<QQuickItem *>(withProperty(table, "text", "later.zip"));
        QVERIFY(label);
        const QPoint rowPoint = label->mapToScene(QPointF(label->width()/2,label->height()/2)).toPoint();
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, rowPoint);
        auto *rowMenu = window->findChild<QObject *>("rowContextMenu");
        QVERIFY(rowMenu);
        QTRY_VERIFY(rowMenu->property("visible").toBool());
        QCOMPARE(window->property("selectedId").toString(), manager.downloads().last().toMap().value("id").toString());
        auto *resumeAction = withProperty(rowMenu, "text", "Resume");
        auto *stopAction = withProperty(rowMenu, "text", "Stop");
        QVERIFY(resumeAction && stopAction);
        QVERIFY(resumeAction->property("enabled").toBool());
        QVERIFY(!stopAction->property("enabled").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QVERIFY(rowMenu->property("currentIndex").toInt() >= 0);
        QCOMPARE(window->property("selectedId").toString(), manager.downloads().last().toMap().value("id").toString());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-context-menu.png"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!rowMenu->property("visible").toBool());
        QCOMPARE(manager.downloads().size(), 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, rowPoint);
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, rowPoint);
        QTRY_VERIFY(transfer->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(transfer, "close"));
        window->setProperty("filter", "queue:Main download queue");
        auto *startQueue = window->findChild<QQuickItem *>("startQueueButton");
        QVERIFY(startQueue);
        QTRY_VERIFY(startQueue->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, startQueue->mapToScene(startQueue->boundingRect().center()).toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(manager.downloads().last().toMap().value("status").toString(), "complete", 10000);
        window->setProperty("filter", "all");
        const auto queuedPath = manager.downloads().last().toMap().value("path").toString();
        QTest::keyClick(window, Qt::Key_Delete);
        auto *remove = window->findChild<QObject *>("removeDialog");
        QVERIFY(remove);
        QTRY_VERIFY(remove->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(remove, "accept"));
        QTRY_COMPARE(manager.downloads().size(), 1);
        QVERIFY(QFile::exists(queuedPath));
        auto *deleteCompleted = window->findChild<QObject *>("deleteCompletedButton");
        QVERIFY(deleteCompleted);
        QVERIFY(QMetaObject::invokeMethod(deleteCompleted, "triggered"));
        QTRY_VERIFY(manager.downloads().isEmpty());
        QVERIFY(QFile::exists(downloaded.fileName()));
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        input->setProperty("text",QString("http://127.0.0.1:%1/auth.bin").arg(server.serverPort()));
        window->findChild<QObject *>("useAuthorization")->setProperty("checked",true);
        window->findChild<QObject *>("loginInput")->setProperty("text","user");
        window->findChild<QObject *>("passwordInput")->setProperty("text","pass");
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_VERIFY(information->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(start,"clicked"));
        QTRY_COMPARE(manager.downloads().size(),1);
        QFile savedQueue(temporary.path()+"/state/queue.json");
        QVERIFY(savedQueue.open(QIODevice::ReadOnly));
        const auto saved=QJsonDocument::fromJson(savedQueue.readAll()).object().value("downloads").toArray().first().toObject();
        QCOMPARE(saved.value("username").toString(),"user");
        QCOMPARE(saved.value("password").toString(),"pass");
        QTRY_VERIFY_WITH_TIMEOUT(manager.downloads().first().toMap().value("status").toString()=="complete" || manager.downloads().first().toMap().value("status").toString()=="error",10000);
        QVERIFY2(manager.downloads().first().toMap().value("status").toString()=="complete",qPrintable(manager.downloads().first().toMap().value("error").toString()+QString("; challenges=%1, credential attempts=%2").arg(authorizationChallenges).arg(basicAttempts)));
        QVERIFY(authorizationChallenges > 0);
        QFile authenticated(manager.downloads().first().toMap().value("path").toString());
        QVERIFY(authenticated.open(QIODevice::ReadOnly));
        QCOMPARE(authenticated.readAll(),payload);
        QVERIFY(window->findChild<QObject *>("passwordInput")->property("text").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(transfer,"close"));
        QVERIFY(QMetaObject::invokeMethod(options,"open"));
        window->findChild<QObject *>("optionsTabs")->setProperty("currentIndex",4);
        auto *siteHost=window->findChild<QObject *>("siteHost");
        auto *siteHeaders=window->findChild<QObject *>("siteHeaders");
        auto *siteAgent=window->findChild<QObject *>("siteAgent");
        auto *siteReferer=window->findChild<QObject *>("siteReferer");
        auto *saveSite=window->findChild<QObject *>("saveSiteButton");
        auto *loadSite=window->findChild<QObject *>("loadSiteButton");
        QVERIFY(siteHost && siteHeaders && siteAgent && siteReferer && saveSite && loadSite);
        siteHost->setProperty("text","127.0.0.1");
        for (const auto &invalid : QStringList{"not JSON", "[]", "null", "{\"X-Key\": 7}", "{\"X-Key\": \"unsafe\\r\\nvalue\"}"}) {
            siteHeaders->setProperty("text",invalid);
            QVERIFY(QMetaObject::invokeMethod(saveSite,"clicked"));
            QVERIFY(!window->property("lastError").toString().isEmpty());
            QVERIFY(!automation.preferences().value("siteSettings").toMap().contains("127.0.0.1"));
        }
        siteHeaders->setProperty("text","{\"X-Api-Key\":\"fixture-site-key\",\"X-Trace\":\"preserved\",\"uSeR-aGeNt\":\"overridden\"}");
        siteAgent->setProperty("text","CustomHeadersFixture/1.0");
        const auto referrer=QString("http://127.0.0.1:%1/source-page").arg(server.serverPort());
        siteReferer->setProperty("text",referrer);
        QVERIFY(QMetaObject::invokeMethod(saveSite,"clicked"));
        QVERIFY2(window->property("lastError").toString().isEmpty(),qPrintable(window->property("lastError").toString()));
        QVERIFY(QMetaObject::invokeMethod(loadSite,"clicked"));
        const auto custom=QJsonDocument::fromJson(siteHeaders->property("text").toByteArray()).object();
        QCOMPARE(custom.value("X-Api-Key").toString(),"fixture-site-key");
        QVERIFY(!siteHeaders->property("text").toString().toLower().contains("user-agent"));
        QCOMPARE(siteAgent->property("text").toString(),"CustomHeadersFixture/1.0");
        QCOMPARE(siteReferer->property("text").toString(),referrer);
        siteAgent->setProperty("text","");siteReferer->setProperty("text","");
        QVERIFY(QMetaObject::invokeMethod(saveSite,"clicked"));
        const auto cleared=automation.preferences().value("siteSettings").toMap().value("127.0.0.1").toMap().value("headers").toMap();
        QVERIFY(!cleared.contains("User-Agent"));QVERIFY(!cleared.contains("Referer"));
        QCOMPARE(cleared.value("X-Trace").toString(),"preserved");
        siteAgent->setProperty("text","CustomHeadersFixture/1.0");siteReferer->setProperty("text",referrer);
        QVERIFY(QMetaObject::invokeMethod(saveSite,"clicked"));
        auto *siteScroll=window->findChild<QObject *>("siteScroll");
        QVERIFY(siteScroll);
        auto *siteContent=siteScroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(siteContent);
        QVERIFY(options->property("height").toReal() < window->height());
        QTRY_VERIFY(siteContent->property("contentHeight").toReal() > siteContent->height());
        siteContent->setProperty("contentY",siteContent->property("contentHeight").toReal()-siteContent->height());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-custom-headers.png"));
        auto *hostItem=qobject_cast<QQuickItem *>(siteHost);
        QVERIFY(hostItem);
        const auto hostPosition=hostItem->mapToScene(QPointF());
        const auto scrollPosition=siteContent->mapToScene(QPointF());
        QVERIFY(hostPosition.y()>=options->property("y").toReal());
        QVERIFY2(hostPosition.y()+hostItem->height() <= scrollPosition.y(),
                 "The site hostname must stay above the scrolling request settings.");
        QVERIFY(QMetaObject::invokeMethod(options,"close"));
        QVERIFY(siteHeaders->property("text").toString().isEmpty());

        QTest::keyClick(window,Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        input->setProperty("text",QString("http://127.0.0.1:%1/site-headers.bin").arg(server.serverPort()));
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_VERIFY(information->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(start,"clicked"));
        QTRY_COMPARE(manager.downloads().size(),2);
        QTRY_VERIFY(manager.downloads().last().toMap().value("status").toString()=="complete" || manager.downloads().last().toMap().value("status").toString()=="error");
        QVERIFY2(manager.downloads().last().toMap().value("status").toString()=="complete",qPrintable(manager.downloads().last().toMap().value("error").toString()));
        QCOMPARE(siteRequestHeaders.value("x-api-key").toString(),"fixture-site-key");
        QCOMPARE(siteRequestHeaders.value("x-trace").toString(),"preserved");
        QCOMPARE(siteRequestHeaders.value("user-agent").toString(),"CustomHeadersFixture/1.0");
        QCOMPARE(siteRequestHeaders.value("referer").toString(),referrer);
        QFile siteDownload(manager.downloads().last().toMap().value("path").toString());
        QVERIFY(siteDownload.open(QIODevice::ReadOnly));QCOMPARE(siteDownload.readAll(),payload);
        QVERIFY(QMetaObject::invokeMethod(transfer,"close"));

        QTest::keyClick(window,Qt::Key_N,Qt::ControlModifier);
        QTRY_VERIFY(input->hasActiveFocus());
        input->setProperty("text",QString("http://127.0.0.1:%1/job-headers.bin").arg(server.serverPort()));
        QTest::keyClick(window,Qt::Key_Return);
        QTRY_VERIFY(information->property("visible").toBool());
        auto *requestHeaders=window->findChild<QObject *>("requestHeadersInput");
        auto *useHeaders=window->findChild<QObject *>("useRequestHeaders");
        QVERIFY(requestHeaders && useHeaders);
        useHeaders->setProperty("checked",true);
        requestHeaders->setProperty("text","{\"X-Api-Key\": 7}");
        QVERIFY(QMetaObject::invokeMethod(start,"clicked"));
        QCOMPARE(manager.downloads().size(),2);
        QVERIFY(information->property("visible").toBool());
        QVERIFY(!window->property("lastError").toString().isEmpty());
        requestHeaders->setProperty("text","{\"x-api-key\":\"fixture-job-key\",\"X-Request\":\"job-only\"}");
        QVERIFY(QMetaObject::invokeMethod(start,"clicked"));
        QTRY_COMPARE(manager.downloads().size(),3);
        QTRY_VERIFY(manager.downloads().last().toMap().value("status").toString()=="complete" || manager.downloads().last().toMap().value("status").toString()=="error");
        QVERIFY2(manager.downloads().last().toMap().value("status").toString()=="complete",qPrintable(manager.downloads().last().toMap().value("error").toString()));
        QCOMPARE(jobRequestHeaders.value("x-api-key").toString(),"fixture-job-key");
        QCOMPARE(jobRequestHeaders.value("x-trace").toString(),"preserved");
        QCOMPARE(jobRequestHeaders.value("x-request").toString(),"job-only");
        QVERIFY(requestHeaders->property("text").toString().isEmpty());
        QVERIFY(!useHeaders->property("checked").toBool());
        QVERIFY(!QJsonDocument::fromVariant(manager.downloads()).toJson().contains("fixture-site-key"));
        QVERIFY(!QJsonDocument::fromVariant(manager.downloads()).toJson().contains("fixture-job-key"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
        qApp->removeTranslator(&translator);
    }

    void brandedMenusAndTransferScreenshots() {
        qmlWarnings.clear();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(writeFile(temporary.filePath("language"), "en\n"));
        Theme theme(temporary.path());
        Translator translator(temporary.filePath("language"));
        DownloadManager manager(temporary.filePath("state"));
        Automation automation(&manager, temporary.filePath("state"), nullptr,
                              [](const QStringList &, std::function<void(int,const QString &)> done) { done(0, {}); });
        QVERIFY(automation.configure({{"notifications",false},{"sound",false}}));
        PortalPicker picker;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("picker", &picker);
        engine.rootContext()->setContextProperty("automation", &automation);
        engine.rootContext()->setContextProperty("backend", &manager);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (socket->property("responded").toBool() || !request.contains("\r\n\r\n")) return;
                    socket->setProperty("responded", true);
                    const bool slow = request.contains("/slow-");
                    const int total = slow ? 8 * 1024 * 1024 : 85732;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(total) + "\r\nConnection: close\r\n\r\n");
                    if (request.startsWith("HEAD ")) { socket->disconnectFromHost(); return; }
                    if (!slow) { socket->write(QByteArray(total, 'd')); socket->disconnectFromHost(); return; }
                    auto *timer = new QTimer(socket);
                    timer->setInterval(80);
                    connect(timer, &QTimer::timeout, socket, [socket, timer, total] {
                        const int sent = socket->property("sent").toInt();
                        const int count = std::min(131072, total - sent);
                        socket->write(QByteArray(count, 'd'));
                        socket->setProperty("sent", sent + count);
                        if (sent + count == total) { timer->stop(); socket->disconnectFromHost(); }
                    });
                    timer->start();
                });
            }
        });
        const auto origin = QString("http://127.0.0.1:%1/").arg(server.serverPort());
        QStringList ids;
        const QStringList filenames{"Design resources.zip", "Field notes.pdf", "Studio session.flac", "Linux installer.iso", "Documentary - chapter 04.mp4", "Typography reference.zip"};
        for (int i = 0; i < filenames.size(); ++i) {
            const auto id = manager.addRequest({{"url", origin + (i >= 4 ? "slow-" : "file-") + QString::number(i)},
                                               {"filename", filenames[i]}, {"directory", temporary.filePath("downloads")},
                                               {"description", i >= 4 ? "Captured from the browser" : ""}, {"start", i < 2 || i >= 4}});
            QVERIFY2(!id.isEmpty(), qPrintable(manager.error()));
            ids << id;
        }
        QTRY_COMPARE(manager.downloads().first().toMap().value("status").toString(), "complete");
        QTRY_VERIFY(manager.downloads().last().toMap().value("received").toLongLong() > 0);
        window->setProperty("selectedId", ids.last());
        QTest::qWait(1600);
        QVERIFY(capture(window, "/tmp/idm-native-overview.png"));
        auto *menuBar = window->findChild<QQuickItem *>("mainMenuBar");
        QVERIFY(menuBar);
        auto *tasksItem = qobject_cast<QQuickItem *>(withProperty(menuBar, "text", "Tasks"));
        auto *tasksMenu = window->findChild<QObject *>("tasksMenu");
        QVERIFY(tasksItem && tasksMenu);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, tasksItem->mapToScene(tasksItem->boundingRect().center()).toPoint());
        QTRY_VERIFY(tasksMenu->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QVERIFY(tasksMenu->property("currentIndex").toInt() >= 0);
        QCOMPARE(window->property("selectedId").toString(), ids.last());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-top-menu.png"));
        QSignalSpy tasksClosed(tasksMenu, SIGNAL(closed()));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!tasksClosed.isEmpty());
        QVERIFY(!tasksMenu->property("visible").toBool());
        auto *table = window->findChild<QQuickItem *>("downloadTable");
        auto *label = qobject_cast<QQuickItem *>(withProperty(table, "text", filenames.last()));
        QVERIFY(label);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, label->mapToScene(label->boundingRect().center()).toPoint());
        auto *rowMenu = window->findChild<QObject *>("rowContextMenu");
        QVERIFY(rowMenu);
        QTRY_VERIFY(rowMenu->property("opened").toBool());
        QSignalSpy updatesWhileMenuOpen(&manager, &DownloadManager::changed);
        QTRY_VERIFY(updatesWhileMenuOpen.count() > 0);
        QVERIFY2(rowMenu->property("opened").toBool(), "Refreshing active transfers must keep their context menu open.");
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-context-menu.png"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!rowMenu->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        auto *transfer = window->findChild<QObject *>("transferDialog");
        QVERIFY(transfer);
        QTRY_VERIFY(transfer->property("visible").toBool());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-active-transfer.png"));
        QVERIFY(QMetaObject::invokeMethod(transfer, "close"));
        manager.pauseAll();
        QVERIFY(automation.configure({{"darkMode", false}}));
        QTRY_VERIFY(!window->property("darkMode").toBool());
        QTest::qWait(200);
        QVERIFY(capture(window, "/tmp/idm-native-light.png"));
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
    }

    void minimumWindowAndLargeQueue() {
        qmlWarnings.clear();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(writeFile(temporary.filePath("language"), "en\n"));
        Theme theme(temporary.path());
        Translator translator(temporary.filePath("language"));
        DownloadManager manager(temporary.filePath("state"));
        Automation automation(&manager, temporary.filePath("state"), nullptr,
                              [](const QStringList &, std::function<void(int,const QString &)> done) { done(0, {}); });
        QVERIFY(automation.configure({{"notifications",false},{"sound",false}}));
        PortalPicker picker;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("picker", &picker);
        engine.rootContext()->setContextProperty("automation", &automation);
        engine.rootContext()->setContextProperty("backend", &manager);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(820,500);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QStringList ids;
        QString lastName;
        for (int i=0;i<80;++i) {
            lastName=QString("Archive %1 - ").arg(i,2,10,QChar('0'))+QString(140,'x')+".zip";
            const auto id=manager.addRequest({{"url",QString("https://downloads.example/%1.zip").arg(i)},
                                             {"filename",lastName},{"description",QString(200,'d')},
                                             {"directory",temporary.filePath("files")},{"start",false}});
            QVERIFY2(!id.isEmpty(),qPrintable(manager.error()));
            ids<<id;
        }
        window->setProperty("sortKey","name");
        window->setProperty("descending",false);
        window->setProperty("selectedId",ids.first());
        auto *table=window->findChild<QQuickItem *>("downloadTable");
        QVERIFY(table);
        QTRY_COMPARE(table->property("count").toInt(),80);
        for (int i=1;i<80;++i) QTest::keyClick(window,Qt::Key_Down);
        QTRY_COMPARE(window->property("selectedId").toString(),ids.last());
        QTRY_VERIFY(table->property("contentY").toReal()>0);
        auto *name=qobject_cast<QQuickItem *>(withProperty(table,"text",lastName));
        QVERIFY(name);
        QVERIFY(name->property("truncated").toBool());
        const auto rowPosition=name->mapToItem(table,QPointF());
        QVERIFY(rowPosition.y()>=0 && rowPosition.y()+name->height()<=table->height());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-native-minimum.png"));
        QTest::mouseMove(window,name->mapToScene(QPointF(name->width()/2,name->height()/2)).toPoint());
        auto *tip=name->findChild<QObject *>("downloadCellTip");
        QVERIFY(tip);
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("visible").toBool(),2000);
        QCOMPARE(tip->property("text").toString(),lastName);
        QVERIFY(capture(window, "/tmp/idm-native-long-name-tooltip.png"));
        QTest::mouseMove(window,QPoint(5,5));
        auto *horizontal=window->findChild<QQuickItem *>("tableScroll");
        auto *advancedColumns=window->findChild<QObject *>("advancedColumnsAction");
        QVERIFY(horizontal && advancedColumns);
        QVERIFY(horizontal->property("contentWidth").toReal() <= horizontal->width() + 1);
        QVERIFY(QMetaObject::invokeMethod(advancedColumns, "trigger", Q_ARG(QObject *, nullptr)));
        QTRY_VERIFY(advancedColumns->property("checked").toBool());
        QTRY_VERIFY(horizontal->property("contentWidth").toReal()>horizontal->width());
        horizontal->setProperty("contentX",horizontal->property("contentWidth").toReal()-horizontal->width());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-native-minimum-scrolled.png"));
        horizontal->setProperty("contentX",0);
        QVERIFY(QMetaObject::invokeMethod(advancedColumns, "trigger", Q_ARG(QObject *, nullptr)));
        QTRY_VERIFY(!advancedColumns->property("checked").toBool());
        QTest::keyClick(window,Qt::Key_Return);
        auto *transfer=window->findChild<QObject *>("transferDialog");
        QVERIFY(transfer);
        QTRY_VERIFY(transfer->property("visible").toBool());
        QVERIFY(transfer->property("width").toReal()<=window->width());
        QVERIFY(transfer->property("height").toReal()<=window->height());
        auto *closeButton=qobject_cast<QQuickItem *>(withProperty(transfer,"text","Close"));
        QVERIFY(closeButton);
        QTRY_VERIFY(closeButton->mapToScene(QPointF(0,closeButton->height())).y()<=window->height());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-native-long-name.png"));
        QVERIFY(QMetaObject::invokeMethod(transfer,"close"));
        auto *information = window->findChild<QObject *>("infoDialog");
        auto *infoScroll = window->findChild<QObject *>("infoScroll");
        auto *startDownload = window->findChild<QQuickItem *>("startDownloadButton");
        auto *downloadLater = window->findChild<QQuickItem *>("downloadLaterButton");
        QVERIFY(information && infoScroll && startDownload && downloadLater);
        window->findChild<QObject *>("addressInput")->setProperty("text", "https://downloads.example/design-resources.zip");
        window->findChild<QObject *>("filenameInput")->setProperty("text", "Design resources.zip");
        window->findChild<QObject *>("directoryInput")->setProperty("text", temporary.filePath("files"));
        window->findChild<QObject *>("infoMedia")->setProperty("checked", true);
        window->findChild<QObject *>("useRequestHeaders")->setProperty("checked", true);
        window->findChild<QObject *>("requestHeadersInput")->setProperty("text", "{\"X-Project\": \"Design resources\"}");
        QVERIFY(QMetaObject::invokeMethod(information, "open"));
        QTRY_VERIFY(information->property("visible").toBool());
        auto *infoContent = infoScroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(infoContent);
        QTRY_VERIFY(infoContent->property("contentHeight").toReal() > infoContent->height());
        QTest::qWait(100);
        const QRectF windowBounds(0, 0, window->width(), window->height());
        for (auto *button : {startDownload, downloadLater}) {
            QTRY_VERIFY(button->isVisible());
            QVERIFY(windowBounds.contains(button->mapRectToScene(button->boundingRect())));
        }
        const auto actionPosition = startDownload->mapRectToScene(startDownload->boundingRect());
        infoContent->setProperty("contentY", infoContent->property("contentHeight").toReal() - infoContent->height());
        QTRY_VERIFY(infoContent->property("contentY").toReal() > 0);
        auto *headersEditor = window->findChild<QQuickItem *>("requestHeadersInput");
        QVERIFY(headersEditor);
        QTRY_VERIFY(headersEditor->mapToItem(infoContent, QPointF(0, headersEditor->height())).y() <= infoContent->height());
        QCOMPARE(startDownload->mapRectToScene(startDownload->boundingRect()), actionPosition);
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-minimum-info.png"));
        QVERIFY(QMetaObject::invokeMethod(information, "close"));
        auto *options=window->findChild<QObject *>("optionsDialog");
        QVERIFY(options);
        QVERIFY(QMetaObject::invokeMethod(options,"open"));
        window->findChild<QObject *>("optionsTabs")->setProperty("currentIndex",4);
        window->findChild<QObject *>("siteHost")->setProperty("text","downloads.example");
        for (const auto *name : {"siteAuthenticationDisclosure", "siteHeadersDisclosure"}) {
            auto *disclosure = window->findChild<QQuickItem *>(name);
            QVERIFY(disclosure);
            if (!disclosure->property("checked").toBool())
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, disclosure->mapToScene(disclosure->boundingRect().center()).toPoint());
            QTRY_VERIFY(disclosure->property("checked").toBool());
        }
        auto *scroll=window->findChild<QObject *>("siteScroll");
        QVERIFY(scroll);
        auto *content=scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QTest::qWait(50);
        content->setProperty("contentY",content->property("contentHeight").toReal()-content->height());
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-native-minimum-sites.png"));
        QVERIFY(options->property("height").toReal()<=window->height());
        auto *host=qobject_cast<QQuickItem *>(window->findChild<QObject *>("siteHost"));
        auto *save=qobject_cast<QQuickItem *>(window->findChild<QObject *>("saveSiteButton"));
        QVERIFY(host && save);
        QVERIFY(host->mapToScene(QPointF()).y()>=0);
        QVERIFY(host->mapToScene(QPointF(0,host->height())).y()<=content->mapToScene(QPointF()).y());
        QVERIFY(save->mapToScene(QPointF(0,save->height())).y()<=window->height());
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }

    void automationDialogsAndPolicies() {
        qmlWarnings.clear();
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(writeFile(temporary.filePath("language"), "en\n"));
        Theme theme(temporary.path());
        Translator translator(temporary.filePath("language"));
        DownloadManager manager(temporary.filePath("state"));
        QList<QStringList> commands;
        Automation automation(&manager, temporary.filePath("state"), nullptr,
                              [&](const QStringList &command, std::function<void(int,const QString &)> done) {
            commands.append(command); done(0, {});
        });
        QVERIFY(automation.configure({{"notifications",false},{"sound",false}}));
        PortalPicker picker;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("picker", &picker);
        engine.rootContext()->setContextProperty("automation", &automation);
        engine.rootContext()->setContextProperty("backend", &manager);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        auto *options=window->findChild<QObject *>("optionsDialog");
        QVERIFY(options);
        QVERIFY(QMetaObject::invokeMethod(options,"open"));
        auto *directory=window->findChild<QObject *>("defaultsDirectory");
        auto *concurrency=window->findChild<QObject *>("concurrentInput");
        auto *rate=window->findChild<QObject *>("rateInput");
        auto *saveOptions=window->findChild<QObject *>("saveOptionsButton");
        QVERIFY(directory && concurrency && rate && saveOptions);
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-options.png"));
        directory->setProperty("text",temporary.filePath("downloads"));
        window->findChild<QObject *>("optionsTabs")->setProperty("currentIndex", 1);
        auto *concurrencyEditor = concurrency->property("contentItem").value<QQuickItem *>();
        QVERIFY(concurrencyEditor);
        QTRY_VERIFY(concurrencyEditor->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, concurrencyEditor->mapToScene(concurrencyEditor->boundingRect().center()).toPoint());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_7);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(concurrency->property("value").toInt(), 7);
        rate->setProperty("text","1024");
        QVERIFY(QMetaObject::invokeMethod(saveOptions,"clicked"));
        QTRY_VERIFY(!options->property("visible").toBool());
        QCOMPARE(automation.preferences().value("directory").toString(),temporary.filePath("downloads"));
        QCOMPARE(automation.preferences().value("maxConcurrent").toInt(),7);
        QCOMPARE(automation.preferences().value("rateLimit").toInt(),1024);
        QVERIFY(QFile::exists(temporary.filePath("state/automation.json")));

        auto *scheduler=window->findChild<QObject *>("schedulerDialog");
        QVERIFY(scheduler);
        QVERIFY(QMetaObject::invokeMethod(scheduler,"open"));
        auto *queueName=window->findChild<QObject *>("queueName");
        auto *queueConcurrency=window->findChild<QObject *>("queueConcurrent");
        auto *queueStart=window->findChild<QObject *>("queueStart");
        auto *saveQueue=window->findChild<QObject *>("saveQueueButton");
        QVERIFY(queueName && queueConcurrency && queueStart && saveQueue);
        auto *timing = window->findChild<QQuickItem *>("scheduleTimingDisclosure");
        QVERIFY(timing);
        QVERIFY(!timing->property("checked").toBool());
        QVERIFY(!qobject_cast<QQuickItem *>(queueStart)->isVisible());
        QTest::qWait(100);
        QVERIFY(capture(window, "/tmp/idm-native-scheduler-quiet.png"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, timing->mapToScene(timing->boundingRect().center()).toPoint());
        QTRY_VERIFY(timing->property("checked").toBool());
        QTRY_VERIFY(qobject_cast<QQuickItem *>(queueStart)->isVisible());
        QTest::qWait(100);
        auto *queueEditor = queueName->property("contentItem").value<QQuickItem *>();
        QVERIFY(queueEditor);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, queueEditor->mapToScene(queueEditor->boundingRect().center()).toPoint());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const char character : QByteArray("Nightly")) QTest::keyClick(window, character);
        QTRY_COMPARE(queueName->property("editText").toString(), "Nightly");
        queueConcurrency->setProperty("value",2);
        const auto nextYear=QDateTime::currentDateTime().addYears(1).toString(Qt::ISODate);
        queueStart->setProperty("text",nextYear);
        QVERIFY(QMetaObject::invokeMethod(saveQueue,"clicked"));
        QTRY_VERIFY(!scheduler->property("visible").toBool());
        QVariantMap nightly;
        for(const auto &item:automation.queues())if(item.toMap().value("name")=="Nightly")nightly=item.toMap();
        QCOMPARE(nightly.value("concurrent").toInt(),2);
        QCOMPARE(QDateTime::fromString(nightly.value("startAt").toString(),Qt::ISODate).toSecsSinceEpoch(),QDateTime::fromString(nextYear,Qt::ISODate).toSecsSinceEpoch());
        QVERIFY(QMetaObject::invokeMethod(scheduler,"open"));
        QTest::qWait(50);
        QVERIFY(capture(window, "/tmp/idm-classic-scheduler.png"));
        QVERIFY(QMetaObject::invokeMethod(scheduler,"close"));

        const auto id=automation.enqueue({{"url","http://127.0.0.1:9/queued.bin"},{"start",false}});
        QVERIFY(!id.isEmpty());
        window->setProperty("selectedId",id);
        auto *policy=window->findChild<QObject *>("policyDialog");
        QVERIFY(policy);
        QVERIFY(QMetaObject::invokeMethod(policy,"open"));
        window->findChild<QObject *>("jobQueue")->setProperty("editText","Nightly");
        window->findChild<QObject *>("jobPriority")->setProperty("currentIndex",1);
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>("savePolicyButton"),"clicked"));
        QTRY_VERIFY(!policy->property("visible").toBool());
        QCOMPARE(automation.jobPolicies().value(id).toMap().value("queue").toString(),"Nightly");
        QCOMPARE(automation.jobPolicies().value(id).toMap().value("priority").toInt(),10);
        auto *edit=window->findChild<QObject *>("editDialog");
        QVERIFY(edit);
        edit->setProperty("renaming",true);
        QVERIFY(QMetaObject::invokeMethod(edit,"open"));
        window->findChild<QObject *>("editValue")->setProperty("text","renamed.bin");
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>("saveEditButton"),"clicked"));
        QTRY_VERIFY(!edit->property("visible").toBool());
        QCOMPARE(manager.downloads().first().toMap().value("name").toString(),"renamed.bin");
        edit->setProperty("renaming",false);
        QVERIFY(QMetaObject::invokeMethod(edit,"open"));
        window->findChild<QObject *>("editValue")->setProperty("text","http://127.0.0.1:9/refreshed.bin");
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>("saveEditButton"),"clicked"));
        QTRY_VERIFY(!edit->property("visible").toBool());
        QCOMPARE(manager.downloads().first().toMap().value("url").toString(),"http://127.0.0.1:9/refreshed.bin");
        automation.pauseAll();
        QCOMPARE(manager.downloads().first().toMap().value("status").toString(),"paused");
        window->setProperty("filter","queue:Nightly");
        QTRY_VERIFY(withProperty(window->findChild<QObject *>("downloadTable"),"text","renamed.bin"));

        auto *list=window->findChild<QObject *>("listDialog");
        QVERIFY(list);
        list->setProperty("importing",false);
        QVERIFY(QMetaObject::invokeMethod(list,"open"));
        const auto exportPath=temporary.filePath("list.json");
        window->findChild<QObject *>("listPath")->setProperty("text",exportPath);
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>("listSubmitButton"),"clicked"));
        QTRY_VERIFY(!list->property("visible").toBool());
        QFile exported(exportPath);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        const auto content=exported.readAll();
        QVERIFY(content.contains("renamed.bin"));
        QVERIFY(!content.contains("Cookie"));
        const auto importPath=temporary.filePath("imports.txt");
        QVERIFY(writeFile(importPath,"http://127.0.0.1:9/imported.zip\n"));
        list->setProperty("importing",true);
        QVERIFY(QMetaObject::invokeMethod(list,"open"));
        window->findChild<QObject *>("listPath")->setProperty("text",importPath);
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>("listSubmitButton"),"clicked"));
        QTRY_COMPARE(manager.downloads().size(),2);
        automation.pauseAll();
        QTcpServer website;
        QVERIFY(website.listen(QHostAddress::LocalHost));
        const QByteArray html("<html><head><title>UI site fixture</title></head><body>Website copied through the dialog</body></html>");
        connect(&website,&QTcpServer::newConnection,this,[&] {
            while(auto *socket=website.nextPendingConnection()) {
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[socket,html] {
                    const auto request=socket->property("request").toByteArray()+socket->readAll();
                    socket->setProperty("request",request);
                    if(!request.contains("\r\n\r\n"))return;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "+QByteArray::number(html.size())+"\r\nConnection: close\r\n\r\n"+html);
                    socket->disconnectFromHost();
                });
            }
        });
        auto *grabber=window->findChild<QObject *>("grabberDialog");
        QVERIFY(grabber);
        QVERIFY(QMetaObject::invokeMethod(grabber,"open"));
        auto *grabberUrl=window->findChild<QObject *>("grabberUrl");
        auto *grabberDirectory=window->findChild<QObject *>("grabberDirectory");
        auto *grabberStart=window->findChild<QObject *>("grabberStartButton");
        QVERIFY(grabberUrl && grabberDirectory && grabberStart);
        grabberDirectory->setProperty("text",temporary.filePath("website"));
        grabberUrl->setProperty("text","file:///etc/passwd");
        QVERIFY(QMetaObject::invokeMethod(grabberStart,"clicked"));
        QVERIFY(!automation.error().isEmpty());
        grabberUrl->setProperty("text",QString("http://127.0.0.1:%1/").arg(website.serverPort()));
        QVERIFY(QMetaObject::invokeMethod(grabberStart,"clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(automation.grabber().value("status").toString(),"complete",15000);
        QFile copied(temporary.filePath("website/index.html"));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QVERIFY(copied.readAll().contains("Website copied through the dialog"));
        QVERIFY(QMetaObject::invokeMethod(grabber,"close"));
        QVERIFY(QMetaObject::invokeMethod(&automation,"clipboardUrl",Q_ARG(QString,QString("https://example.test/from-clipboard.zip"))));
        auto *urlDialog=window->findChild<QObject *>("urlDialog");
        QTRY_VERIFY(urlDialog->property("visible").toBool());
        QCOMPARE(window->findChild<QObject *>("urlInput")->property("text").toString(),"https://example.test/from-clipboard.zip");
        QTest::keyClick(window,Qt::Key_Escape);
        QVERIFY(commands.isEmpty());
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }

    void portalResponsesValidateLocalFolders() {
        PortalPicker picker;
        QSignalSpy selected(&picker, &PortalPicker::selected);
        QSignalSpy errors(&picker, &PortalPicker::error);
        auto respond = [&](uint code, const QStringList &uris) {
            const QVariantMap results{{"uris", uris}};
            QVERIFY(QMetaObject::invokeMethod(&picker, "response", Qt::DirectConnection,
                    Q_ARG(uint, code), Q_ARG(QVariantMap, results)));
        };
        respond(0, {"file:///tmp/a%20folder"});
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().first().toString(), "/tmp/a folder");
        respond(1, {});
        QCOMPARE(errors.size(), 0);
        respond(0, {"https://example.com/not-local"});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(selected.size(), 1);
        respond(2, {});
        QCOMPARE(errors.size(), 2);
    }

    void themeAndLanguageFallbacks() {
        QCOMPARE(contrast(Qt::white, Qt::black), 21.0);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QCOMPARE(Theme::readAccent(temporary.filePath("absent")), "#FFD60A");
        QVERIFY(writeFile(temporary.filePath("colors"), "accent = 'not-a-color'\n"));
        QCOMPARE(Theme::readAccent(temporary.filePath("colors")), "#FFD60A");
        QCOMPARE(Translator::readLanguage(temporary.filePath("absent")), "en");
        QVERIFY(writeFile(temporary.filePath("language"), "../bad\n"));
        QCOMPARE(Translator::readLanguage(temporary.filePath("language")), "en");
        Translator translator(temporary.filePath("language"));
        QVERIFY(translator.translate("", "Uncatalogued string").isNull());
        QCOMPARE(Translator::pluralCategory("da", 1), "one");
        QCOMPARE(Translator::pluralCategory("da", 2), "other");
    }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    previousHandler = qInstallMessageHandler(collectMessages);
    QQuickStyle::setStyle("Basic");
    UiTests tests;
    const int result = QTest::qExec(&tests, argc, argv);
    qInstallMessageHandler(previousHandler);
    return result;
}

#include "ui_tests.moc"
