#include <QtTest>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "downloadmanager.h"
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
        PortalPicker picker;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("picker", &picker);
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
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->isActive());
        QVERIFY(withProperty(window, "text", "Make room for what's next."));
        QCOMPARE(theme.accent(), "#F8C100");
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save("/tmp/idm-native-empty.png"));

        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        auto *input = window->findChild<QQuickItem *>("urlInput");
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
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
        QVERIFY(window->grabWindow().save("/tmp/idm-native-danish.png"));

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
        QVERIFY(withProperty(window, "color", QColor("#39C990")));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray payload("UI transfer fixture\n");
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket, payload] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
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
        auto *directory = withProperty(window, "placeholderText", "Save to folder");
        QVERIFY(directory);
        directory->setProperty("text", temporary.path() + "/output");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(manager.downloads().size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(manager.downloads().first().toMap().value("status").toString(), "complete", 10000);
        QFile downloaded(manager.downloads().first().toMap().value("path").toString());
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), payload);
        QTRY_VERIFY(withProperty(window, "text", "ui.bin"));
        QVERIFY(writeFile(colorsPath, "accent = '#FFD60A'\n"));
        QTRY_COMPARE(theme.accent(), "#FFD60A");
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save("/tmp/idm-native-download.png"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join('\n')));
        qApp->removeTranslator(&translator);
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
    QQuickStyle::setStyle("Material");
    UiTests tests;
    const int result = QTest::qExec(&tests, argc, argv);
    qInstallMessageHandler(previousHandler);
    return result;
}

#include "ui_tests.moc"
