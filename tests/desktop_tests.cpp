#include <QtTest>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include "desktopintegration.h"
#include "downloadmanager.h"
#include "portalpicker.h"
#include "theme.h"
#include "translator.h"

static bool writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
static QByteArray readFile(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
static QString configPath() { return qEnvironmentVariable("XDG_CONFIG_HOME"); }
static QString dataPath() { return qEnvironmentVariable("XDG_DATA_HOME"); }
static QByteArray releaseJson(const QString &tag = "v0.3.0", const QString &url = "https://github.com/earlyhours/idm-rust/releases/tag/v0.3.0") {
    return QJsonDocument(QJsonObject{{"tag_name",tag},{"html_url",url},{"name","<b>Release</b>"},{"body","<img src=\"https://untrusted.example/pixel\">\nRun nothing."},{"published_at","2026-01-01T00:00:00Z"}}).toJson();
}

class Reply : public QNetworkReply {
    QByteArray body;
    qint64 offset = 0;
public:
    Reply(const QNetworkRequest &request, int status, QByteArray bytes, QObject *parent)
        : QNetworkReply(parent), body(std::move(bytes)) {
        setRequest(request); setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute,status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0,this,[this] { emit readyRead(); if (!isFinished()) { setFinished(true); emit finished(); } });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError,"aborted"); setFinished(true); emit finished(); } }
    qint64 bytesAvailable() const override { return body.size()-offset+QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override {
        const auto count = std::min(size,static_cast<qint64>(body.size())-offset);
        if (count <= 0) return -1;
        memcpy(data,body.constData()+offset,count); offset+=count; return count;
    }
};
class Network : public QNetworkAccessManager {
public:
    int status = 200, calls = 0;
    QByteArray body = releaseJson();
    QNetworkRequest last;
protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *) override {
        Q_ASSERT(op == GetOperation);
        ++calls; last=request;
        return new Reply(request,status,body,this);
    }
};

static QObject *named(QObject *root, const QString &name) {
    if (root->objectName() == name) return root;
    for (auto *child : root->children()) if (auto *found = named(child,name)) return found;
    if (auto *item = qobject_cast<QQuickItem *>(root))
        for (auto *child : item->childItems()) if (child->parent() != root) if (auto *found = named(child,name)) return found;
    return nullptr;
}
static QStringList keys(QObject *root) {
    QStringList result;
    for (const auto &column : root->property("columns").value<QJSValue>().toVariant().toList()) result << column.toMap().value("key").toString();
    return result;
}

class UrlSink : public QObject {
    Q_OBJECT
public:
    QUrl url;
public slots:
    void receive(const QUrl &value) { url=value; }
};

class DesktopTests : public QObject {
    Q_OBJECT
private slots:
    void init() {
        QDir(configPath()).removeRecursively(); QDir().mkpath(configPath());
        QDir(dataPath()).removeRecursively(); QDir().mkpath(dataPath());
    }
    void layoutPersistsAndValidates() {
        QTemporaryDir temp;
        DesktopIntegration desktop(temp.path());
        QCOMPARE(desktop.layout().value("columnOrder").toStringList(), QStringList({"name","status","total","speed","eta","created","description"}));
        QVERIFY(!desktop.autostart());
        QVERIFY(!QFileInfo::exists(configPath()+"/autostart/idm-native.desktop"));
        QVERIFY(desktop.setLayout("column",{"total","name","status","speed","eta","created","description"},{"speed","eta","created","description"}));
        QVERIFY(desktop.setLayout("toolbar",{"add","resume"},{"pause"}));
        DesktopIntegration reopened(temp.path());
        QCOMPARE(reopened.layout(),desktop.layout());
        const auto before=desktop.layout();
        QVERIFY(!desktop.setLayout("other",{},{}));
        QVERIFY(!desktop.setLayout("column",{"name","name"},{}));
        QVERIFY(!desktop.setLayout("column",{"arbitrary"},{}));
        QVERIFY(!desktop.setLayout("column",{}, {"name","status","total","speed","eta","created","description"}));
        QCOMPARE(desktop.layout(),before);
        QVERIFY(desktop.resetLayout());
        QCOMPARE(desktop.layout().value("toolbarHidden").toStringList(),QStringList({"stopAll","stopQueue","scheduler","options"}));
    }
    void autostartLaunchesExactExecutable() {
        QTemporaryDir temp;
        const auto executable = temp.filePath("app with $ ` \\\" quotes");
        const auto log = temp.filePath("arguments");
        qputenv("IDM_DESKTOP_LAUNCH_LOG",log.toUtf8());
        QVERIFY(writeFile(executable,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$IDM_DESKTOP_LAUNCH_LOG\"\n"));
        QVERIFY(QFile::setPermissions(executable,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        DesktopIntegration desktop(temp.path(),executable);
        QVERIFY2(desktop.setAutostart(true),qPrintable(desktop.error()));
        QVERIFY(desktop.autostart());
        const auto entry = configPath()+"/autostart/idm-native.desktop";
        QCOMPARE(QProcess::execute("desktop-file-validate",{entry}),0);
        QCOMPARE(QProcess::execute("gio",{"launch",entry}),0);
        QTRY_COMPARE(readFile(log),QByteArray("background\n--state-directory\n")+temp.path().toUtf8()+"\n");
        DesktopIntegration reopened(temp.path(),executable);
        QVERIFY(reopened.autostart());
        QVERIFY(reopened.setAutostart(false));
        QVERIFY(!QFileInfo::exists(entry));
        QVERIFY(writeFile(entry,"[Desktop Entry]\nType=Application\nName=Other\nExec=/bin/true\n"));
        QVERIFY(!desktop.setAutostart(true));
        QVERIFY(!desktop.setAutostart(false));
        QVERIFY(readFile(entry).contains("Name=Other"));
        QVERIFY(QFile::remove(entry));
        const auto victim=temp.filePath("victim");
        QVERIFY(writeFile(victim,"unchanged")); QVERIFY(QFile::link(victim,entry));
        QVERIFY(!desktop.setAutostart(true)); QVERIFY(!desktop.setAutostart(false));
        QCOMPARE(readFile(victim),QByteArray("unchanged"));
        QVERIFY(DesktopIntegration::desktopExec("/bad\nentry").isEmpty());
        QVERIFY(DesktopIntegration::desktopExec("/bad=entry").isEmpty());
        QVERIFY(DesktopIntegration::desktopExec("relative").isEmpty());
        QVERIFY(DesktopIntegration::desktopExec("/percent%app").isEmpty());
    }
    void urlHandlerOptInAndLaunch() {
        QTemporaryDir temp;
        const auto executable=temp.filePath("app"), log=temp.filePath("url-args");
        qputenv("IDM_DESKTOP_LAUNCH_LOG",log.toUtf8());
        QVERIFY(writeFile(executable,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$IDM_DESKTOP_LAUNCH_LOG\"\n"));
        QVERIFY(QFile::setPermissions(executable,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        QProcess query;
        query.start("xdg-mime",{"query","default","x-scheme-handler/https"}); QVERIFY(query.waitForFinished());
        const auto browser=query.readAllStandardOutput();
        DesktopIntegration desktop(temp.path(),executable);
        QVERIFY(!QFileInfo::exists(dataPath()+"/applications/idm-native-url.desktop"));
        QVERIFY2(desktop.installUrlHandler(),qPrintable(desktop.error()));
        query.start("xdg-mime",{"query","default","x-scheme-handler/idm-native"}); QVERIFY(query.waitForFinished());
        QCOMPARE(query.readAllStandardOutput().trimmed(),QByteArray("idm-native-url.desktop"));
        query.start("xdg-mime",{"query","default","x-scheme-handler/https"}); QVERIFY(query.waitForFinished());
        QCOMPARE(query.readAllStandardOutput(),browser);
        const auto entry=dataPath()+"/applications/idm-native-url.desktop";
        QCOMPARE(QProcess::execute("desktop-file-validate",{entry}),0);
        const QString url="idm-native://download/?url=https%3A%2F%2Fexample.com%2Fa%2520file%3Fx%3D1%26y%3D2";
        QCOMPARE(QProcess::execute("gio",{"launch",entry,url}),0);
        QTRY_COMPARE(readFile(log),QByteArray("open\n")+url.toUtf8()+"\n--state-directory\n"+temp.path().toUtf8()+"\n");
        QCOMPARE(DesktopIntegration::externalUrl(url).toString(QUrl::FullyEncoded),QString("https://example.com/a%20file?x=1&y=2"));
        for (const auto &bad : {"file:///tmp/file","javascript:alert(1)","https://","https://x/\nExec=bad","idm-native:file:///tmp/file","https://x/a b"})
            QVERIFY(DesktopIntegration::externalUrl(bad).isEmpty());
    }
    void onlyCompletedRegularFilesCanDrag() {
        QTemporaryDir temp;
        const auto path=temp.filePath("report #1%?.txt");
        QVERIFY(writeFile(path,"report"));
        QVariantMap job{{"path",path},{"status","paused"}};
        QVERIFY(DesktopIntegration::completedFileUrls(job).isEmpty());
        job["status"]="complete";
        const auto urls=DesktopIntegration::completedFileUrls(job);
        QCOMPARE(urls,QList<QUrl>{QUrl::fromLocalFile(path)});
        QMimeData mime; mime.setUrls(urls);
        QVERIFY(mime.hasUrls());
        QVERIFY(mime.data("text/uri-list").contains("report%20%231%25%3F.txt"));
        job["path"]=temp.path(); QVERIFY(DesktopIntegration::completedFileUrls(job).isEmpty());
        job["path"]=temp.filePath("missing"); QVERIFY(DesktopIntegration::completedFileUrls(job).isEmpty());
        job["path"]=temp.filePath("link"); QVERIFY(QFile::link(path,job["path"].toString()));
        QVERIFY(DesktopIntegration::completedFileUrls(job).isEmpty());
    }
    void releaseValidation() {
        auto release=DesktopIntegration::parseRelease(releaseJson(),"0.2.1");
        QVERIFY(!release.isEmpty()); QVERIFY(release.value("newer").toBool());
        QCOMPARE(release.value("notes").toString(),QString("<img src=\"https://untrusted.example/pixel\">\nRun nothing."));
        QVERIFY(!DesktopIntegration::parseRelease(releaseJson(),"0.3.0").value("newer").toBool());
        QVERIFY(!DesktopIntegration::parseRelease(releaseJson(),"0.4.0").value("newer").toBool());
        for (const auto &url : {"https://evil.example/tag/v0.3.0","https://github.com/else/repo/releases/tag/v0.3.0", "http://github.com/earlyhours/idm-rust/releases/tag/v0.3.0", "https://github.com/earlyhours/idm-rust/releases/tag/v0.3.0?next=bad", "file:///bin/sh", "javascript:alert(1)"})
            QCOMPARE(DesktopIntegration::parseRelease(releaseJson("v0.3.0",url),"0.2.1").value("url").toString(),QString("https://github.com/earlyhours/idm-rust/releases/tag/v0.3.0"));
        QVERIFY(DesktopIntegration::parseRelease("invalid", "0.2.1").isEmpty());
        QVERIFY(DesktopIntegration::parseRelease(QByteArray(512*1024+1,'x'),"0.2.1").isEmpty());
        QVERIFY(DesktopIntegration::parseRelease(releaseJson("v0.3.0-beta"),"0.2.1").isEmpty());
        auto object=QJsonDocument::fromJson(releaseJson()).object(); object["prerelease"]=true;
        QVERIFY(DesktopIntegration::parseRelease(QJsonDocument(object).toJson(),"0.2.1").isEmpty());
    }
    void updateRequestAndFailures() {
        QTemporaryDir temp; Network network; DesktopIntegration desktop(temp.path(),{},&network);
        desktop.checkForUpdates(); desktop.checkForUpdates();
        QCOMPARE(network.calls,1); QVERIFY(desktop.checking());
        QTRY_VERIFY(!desktop.checking());
        QCOMPARE(network.last.url(),QUrl("https://api.github.com/repos/earlyhours/idm-rust/releases/latest"));
        QCOMPARE(network.last.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),int(QNetworkRequest::SameOriginRedirectPolicy));
        QCOMPARE(network.last.maximumRedirectsAllowed(),3);
        QVERIFY(desktop.release().value("newer").toBool());
        network.status=404; desktop.checkForUpdates();
        QVERIFY(desktop.release().isEmpty()); QTRY_VERIFY(!desktop.checking());
        QVERIFY(desktop.updateStatus().contains("No published release"));
        network.status=429; desktop.checkForUpdates(); QTRY_VERIFY(!desktop.checking());
        QVERIFY(desktop.updateStatus().contains("rate limit"));
        network.status=302; desktop.checkForUpdates(); QTRY_VERIFY(!desktop.checking());
        QVERIFY(desktop.release().isEmpty()); QVERIFY(desktop.updateStatus().contains("Could not check"));
        network.status=200; network.body=QByteArray(512*1024+1,'x'); desktop.checkForUpdates(); QTRY_VERIFY(!desktop.checking());
        QVERIFY(desktop.release().isEmpty());
        network.body="not json"; desktop.checkForUpdates(); QTRY_VERIFY(!desktop.checking());
        QVERIFY(desktop.updateStatus().contains("unsupported"));
    }
    void qmlLayoutStartupUpdateAndDrag() {
        QTemporaryDir temp;
        const auto state=temp.filePath("state"), path=temp.filePath("done #1.txt");
        QVERIFY(writeFile(path,"done"));
        QJsonObject job{{"id","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"url","https://example.com/done.txt"},{"path",path},{"status","complete"},{"total","4"},{"created","2026-01-01T00:00:00Z"}};
        QVERIFY(writeFile(state+"/queue.json",QJsonDocument(QJsonObject{{"version",1},{"downloads",QJsonArray{job}}}).toJson()));
        DownloadManager manager(state); Theme theme(temp.path()); Translator translator(temp.filePath("language")); PortalPicker picker;
        Network network; DesktopIntegration desktop(state,{},&network);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError> &errors) { for (const auto &error:errors) warnings << error.toString(); });
        engine.rootContext()->setContextProperty("backend",&manager);
        engine.rootContext()->setContextProperty("theme",&theme);
        engine.rootContext()->setContextProperty("translator",&translator);
        engine.rootContext()->setContextProperty("picker",&picker);
        engine.rootContext()->setContextProperty("desktop",&desktop);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY2(!engine.rootObjects().isEmpty(),qPrintable(warnings.join('\n')));
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto click = [&](const QString &name) {
            auto *item=qobject_cast<QQuickItem *>(named(window,name));
            QVERIFY2(item,qPrintable(name)); QVERIFY2(item->isVisible(),qPrintable(name));
            const auto point=item->mapToScene(QPointF(item->width()/2,item->height()/2));
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point.toPoint()); QCoreApplication::processEvents();
        };
        QCOMPARE(keys(window),QStringList({"name","status","total","speed"}));
        window->setProperty("selectedId","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        auto *advanced=named(window,"advancedColumnsAction"); QVERIFY(advanced);
        QVERIFY(QMetaObject::invokeMethod(advanced,"trigger",Q_ARG(QObject *,nullptr)));
        QCOMPARE(keys(window).size(),7);
        QVERIFY(QMetaObject::invokeMethod(advanced,"trigger",Q_ARG(QObject *,nullptr)));
        QCOMPARE(keys(window).size(),4);
        auto *customize=named(window,"customizeLayoutAction"); QVERIFY(customize);
        QVERIFY(QMetaObject::invokeMethod(customize,"trigger",Q_ARG(QObject *,nullptr)));
        QTRY_VERIFY(named(window,"layoutDialog")->property("visible").toBool());
        QTest::qWait(100);
        click("layout-column-speed"); QVERIFY(!keys(window).contains("speed"));
        click("layout-down-column-name"); QCOMPARE(keys(window).first(),QString("status"));
        QTRY_VERIFY(qobject_cast<QQuickItem *>(named(window,"tableHeader-status"))->x() < qobject_cast<QQuickItem *>(named(window,"tableHeader-name"))->x());
        QVERIFY(qobject_cast<QQuickItem *>(named(window,"addDownloadButton"))->x() > qobject_cast<QQuickItem *>(named(window,"selectionDetailsButton"))->x());
        click("layout-toolbar-add");
        QTRY_VERIFY(!qobject_cast<QQuickItem *>(named(window,"addDownloadButton"))->isVisible());
        click("layout-up-toolbar-add");
        QCOMPARE(desktop.layout().value("toolbarOrder").toStringList().at(4),QString("add"));
        click("layout-up-toolbar-add");
        click("layout-toolbar-add");
        QTRY_VERIFY(qobject_cast<QQuickItem *>(named(window,"addDownloadButton"))->x() < qobject_cast<QQuickItem *>(named(window,"selectionDetailsButton"))->x());
        click("layout-toolbar-add");
        { DesktopIntegration reopened(state); QCOMPARE(reopened.layout(),desktop.layout()); }
        {
            DesktopIntegration reopened(state);
            QQmlApplicationEngine restored;
            restored.rootContext()->setContextProperty("backend",&manager);
            restored.rootContext()->setContextProperty("theme",&theme);
            restored.rootContext()->setContextProperty("translator",&translator);
            restored.rootContext()->setContextProperty("picker",&picker);
            restored.rootContext()->setContextProperty("desktop",&reopened);
            restored.rootContext()->setContextProperty("startHidden",true);
            restored.load(QUrl("qrc:/Main.qml"));
            QVERIFY(!restored.rootObjects().isEmpty());
            QCOMPARE(keys(restored.rootObjects().first()),keys(window));
            QVERIFY(!qobject_cast<QQuickItem *>(named(restored.rootObjects().first(),"addDownloadButton"))->isVisible());
        }
        click("resetLayoutButton"); QCOMPARE(keys(window),QStringList({"name","status","total","speed"}));
        QVERIFY(QMetaObject::invokeMethod(named(window,"layoutDialog"),"close"));
        QTest::qWait(100);
        QVERIFY(QMetaObject::invokeMethod(named(window,"optionsDialog"),"open")); QTest::qWait(100);
        click("autostartCheck"); QVERIFY(desktop.autostart());
        click("autostartCheck"); QVERIFY(!desktop.autostart());
        named(window,"optionsTabs")->setProperty("currentIndex",6); QCoreApplication::processEvents();
        click("installUrlHandlerButton");
        QVERIFY(QFileInfo::exists(dataPath()+"/applications/idm-native-url.desktop"));
        QVERIFY(QMetaObject::invokeMethod(named(window,"optionsDialog"),"close")); QTest::qWait(100);
        auto *updates=named(window,"checkUpdatesAction"); QVERIFY(updates);
        QVERIFY(QMetaObject::invokeMethod(updates,"trigger",Q_ARG(QObject *,nullptr)));
        QTRY_VERIFY(!desktop.checking());
        QCOMPARE(named(window,"releaseNotes")->property("textFormat").toInt(),0);
        QCOMPARE(named(window,"releaseNotes")->property("text").toString(),desktop.release().value("notes").toString());
        QVERIFY(named(window,"openReleaseButton")->property("enabled").toBool());
        UrlSink sink;
        QDesktopServices::setUrlHandler("https",&sink,"receive");
        click("openReleaseButton");
        QDesktopServices::unsetUrlHandler("https");
        QCOMPARE(sink.url,QUrl("https://github.com/earlyhours/idm-rust/releases/tag/v0.3.0"));
        QVERIFY(QMetaObject::invokeMethod(named(window,"updatesDialog"),"close")); QTest::qWait(100);
        named(window,"loginInput")->setProperty("text","previous-user");
        named(window,"passwordInput")->setProperty("text","previous-secret");
        named(window,"useAuthorization")->setProperty("checked",true);
        QVERIFY(QMetaObject::invokeMethod(window,"presentUrl",Q_ARG(QVariant,QString("https://example.com/confirmed.zip"))));
        QTRY_VERIFY(named(window,"urlDialog")->property("visible").toBool());
        QCOMPARE(manager.downloads().size(),1);
        QCOMPARE(named(window,"loginInput")->property("text").toString(),QString());
        QCOMPARE(named(window,"passwordInput")->property("text").toString(),QString());
        QVERIFY(!named(window,"useAuthorization")->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(named(window,"urlDialog"),"close")); QTest::qWait(100);
        // Drive the actual MouseArea gesture; offscreen has no external drop receiver.
        auto *row=qobject_cast<QQuickItem *>(named(window,"rowMouse-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        QVERIFY2(row,qPrintable(QString::fromUtf8(readFile(state+"/queue.json"))));
        QSignalSpy drag(&desktop,&DesktopIntegration::fileDragStarted);
        const auto start=row->mapToScene(QPointF(100,20)).toPoint();
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseMove(window,start+QPoint(desktop.dragDistance()+10,0));
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,start+QPoint(30,0));
        QTRY_COMPARE(drag.size(),1);
        QCOMPARE(drag.first().first().value<QList<QUrl>>(),QList<QUrl>{QUrl::fromLocalFile(path)});
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};

int main(int argc, char **argv) {
    QTemporaryDir isolated;
    if (!isolated.isValid()) return 1;
    qputenv("HOME",isolated.path().toUtf8());
    qputenv("XDG_CONFIG_HOME",isolated.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME",isolated.filePath("data").toUtf8());
    qputenv("XDG_CACHE_HOME",isolated.filePath("cache").toUtf8());
    qputenv("XDG_CURRENT_DESKTOP","generic");
    QGuiApplication app(argc,argv);
    app.setApplicationName("idm-desktop-tests"); app.setApplicationVersion("0.2.1");
    QQuickStyle::setStyle("Basic");
    DesktopTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "desktop_tests.moc"
