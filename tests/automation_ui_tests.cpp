#include <QtTest>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "automation.h"
#include "downloadmanager.h"
#include "portalpicker.h"
#include "theme.h"
#include "translator.h"

class AutomationUiTests : public QObject {
    Q_OBJECT
private slots:
    void actualControlsSaveProjectsAndDownloadAFile() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto directory = temporary.path()+"/output";
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray payload("synchronized through the real UI");
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket,payload] {
                    auto request = socket->property("request").toByteArray()+socket->readAll();
                    socket->setProperty("request",request);
                    if (!request.contains("\r\n\r\n")) return;
                    if (request.startsWith("GET /auth.bin ") && !request.contains("Authorization: Basic dXNlcjpwYXNz") && !request.contains("authorization: Basic dXNlcjpwYXNz")) {
                        socket->write("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (!request.toLower().contains("x-sync-key: fixture-secret")) {
                        socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nETag: \"fixture\"\r\nConnection: close\r\n\r\n"+payload);
                    socket->disconnectFromHost();
                });
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
        DownloadManager manager(temporary.path()+"/state");
        Automation automation(&manager,temporary.path()+"/state",nullptr,[](const QStringList &,auto done){done(0,{});});
        const auto origin=QString("http://127.0.0.1:%1").arg(server.serverPort());
        QVERIFY(automation.configure({{"directory",directory},{"notifications",false},
            {"siteSettings",QVariantMap{{origin,QVariantMap{{"headers",QVariantMap{{"X-Sync-Key","fixture-secret"}}}}}}}}));
        Theme theme(temporary.path());
        Translator translator(temporary.path()+"/language");
        PortalPicker picker;
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError> &errors){for(const auto &e:errors)warnings<<e.toString();});
        engine.rootContext()->setContextProperty("backend",&manager);
        engine.rootContext()->setContextProperty("automation",&automation);
        engine.rootContext()->setContextProperty("theme",&theme);
        engine.rootContext()->setContextProperty("translator",&translator);
        engine.rootContext()->setContextProperty("picker",&picker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(),qPrintable(warnings.join('\n')));
        auto *window=engine.rootObjects().first();
        auto find=[&](const char *name){return window->findChild<QObject *>(name);};
        auto click=[&](const char *name){
            auto *item=qobject_cast<QQuickItem *>(find(name));
            if (!item || !item->isVisible()) return false;
            QTest::mouseClick(qobject_cast<QQuickWindow *>(window),Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
            return true;
        };
        auto *action=find("automationProjectsAction");QVERIFY(action);
        QVERIFY(QMetaObject::invokeMethod(action,"triggered"));
        auto *dialog=find("automationProjectsDialog");QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        find("quotaMB")->setProperty("value",2);
        find("quotaPeriodMinutes")->setProperty("value",60);
        QVERIFY(click("saveQuotaButton"));
        QCOMPARE(automation.preferences().value("quotaMB").toInt(),2);
        QCOMPARE(automation.preferences().value("quotaPeriodMinutes").toInt(),60);
        find("automationProjectTabs")->setProperty("currentIndex",1);
        find("syncProjectUrl")->setProperty("text",QString("http://127.0.0.1:%1/file.bin").arg(server.serverPort()));
        find("syncProjectDirectory")->setProperty("text",directory);
        find("syncProjectFilename")->setProperty("text","synced.bin");
        find("syncProjectInterval")->setProperty("value",5);
        QTest::qWait(30);
        QVERIFY(click("saveSyncProjectButton"));
        QCOMPARE(automation.preferences().value("fileSyncJobs").toList().size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(automation.syncQueue().first().toMap().value("status").toString(),"complete",15000);
        QFile output(directory+"/synced.bin");QVERIFY(output.open(QIODevice::ReadOnly));QCOMPARE(output.readAll(),payload);
        find("automationProjectTabs")->setProperty("currentIndex",2);
        find("syncProjectUrl")->setProperty("text","https://example.com/gallery/");
        find("syncProjectDirectory")->setProperty("text",directory+"/site");
        find("syncProjectStart")->setProperty("text","2099-01-01T09:00:00Z");
        find("syncProjectInclude")->setProperty("text","*.png, *.jpg");
        find("syncProjectExclude")->setProperty("text","private/*");
        find("syncProjectInterval")->setProperty("value",0);
        QTest::qWait(30);
        QVERIFY(click("saveSyncProjectButton"));
        const auto screenshotDirectory=qEnvironmentVariable("IDM_UI_SCREENSHOT_DIR");
        if (!screenshotDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshotDirectory));
            QTest::qWait(100);
            QVERIFY(qobject_cast<QQuickWindow *>(window)->grabWindow().save(screenshotDirectory+"/automation-projects.png"));
        }
        const auto projects=automation.preferences().value("syncJobs").toList();
        QCOMPARE(projects.size(),1);
        QCOMPARE(projects.first().toMap().value("include").toStringList(),QStringList({"*.png","*.jpg"}));
        QCOMPARE(projects.first().toMap().value("intervalMinutes").toInt(),0);
        find("syncProjectStart")->setProperty("text","invalid date");
        QTest::qWait(30);
        QVERIFY(click("saveSyncProjectButton"));
        QVERIFY(!dialog->property("failure").toString().isEmpty());
        QCOMPARE(automation.preferences().value("syncJobs").toList(),projects);
        QVERIFY(QMetaObject::invokeMethod(dialog,"removeProject"));
        QVERIFY(automation.preferences().value("syncJobs").toList().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(dialog,"close"));
        QVERIFY(QMetaObject::invokeMethod(window,"presentUrl",Q_ARG(QVariant,origin+"/auth.bin")));
        find("useAuthorization")->setProperty("checked",true);
        find("loginInput")->setProperty("text","user");
        find("passwordInput")->setProperty("text","pass");
        find("authMethodChoice")->setProperty("currentIndex",1);
        QVERIFY(QMetaObject::invokeMethod(window,"prepareInformation"));
        QVERIFY(QMetaObject::invokeMethod(window,"submit",Q_ARG(QVariant,true)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.downloads().last().toMap().value("status").toString(),"complete",15000);
        QCOMPARE(manager.downloads().last().toMap().value("authMethod").toString(),"basic");
        QFile authenticated(directory+"/auth.bin"); QVERIFY(authenticated.open(QIODevice::ReadOnly)); QCOMPARE(authenticated.readAll(),payload);
        auto sites=automation.preferences().value("siteSettings").toMap();
        auto profile=sites.value(automation.siteOrigin(origin)).toMap();
        profile["authMethod"]="digest";sites[automation.siteOrigin(origin)]=profile;
        QVERIFY(automation.configure({{"siteSettings",sites}}));
        auto id=automation.enqueue({{"url",origin+"/inherited.bin"},{"start",false}});QVERIFY(!id.isEmpty());
        QCOMPARE(manager.downloads().last().toMap().value("authMethod").toString(),"digest");
        auto other=automation.enqueue({{"url","http://127.0.0.1:1/other.bin"},{"start",false}});QVERIFY(!other.isEmpty());
        QCOMPARE(manager.downloads().last().toMap().value("authMethod").toString(),"auto");
        auto proxied=automation.enqueue({{"url",origin+"/proxy.bin"},{"proxy","https://127.0.0.1"},{"start",false}});QVERIFY2(!proxied.isEmpty(),qPrintable(automation.error()));
        const auto saved=QJsonDocument::fromJson([&]{QFile f(temporary.path()+"/state/queue.json");f.open(QIODevice::ReadOnly);return f.readAll();}()).object();
        const auto proxy=saved.value("downloads").toArray().last().toObject().value("proxy").toObject();
        QCOMPARE(proxy.value("type").toString(),"https");QCOMPARE(proxy.value("port").toInt(),443);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};
int main(int argc,char **argv){QGuiApplication app(argc,argv);QQuickStyle::setStyle("Basic");AutomationUiTests tests;return QTest::qExec(&tests,argc,argv);}
#include "automation_ui_tests.moc"
