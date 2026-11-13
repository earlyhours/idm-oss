#include "automation.h"
#include "browserbridge.h"
#include "downloadmanager.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <limits>
#include <memory>

class AdversarialServer : public QTcpServer {
public:
  QByteArray body = QByteArray(8192, 'd');
  int status = 200;
  bool hold = false;
  int requests = 0;
  QByteArray lastRequest;
  AdversarialServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, input] {
          *input += socket->readAll();
          if (!input->contains("\r\n\r\n"))
            return;
          socket->disconnect(this);
          lastRequest = *input;
          ++requests;
          auto payload = status == 200 ? body : QByteArray("unavailable");
          socket->write("HTTP/1.1 " + QByteArray::number(status) +
                        " Response\r\nContent-Length: " +
                        QByteArray::number(payload.size()) +
                        "\r\nConnection: close\r\n\r\n");
          if (hold)
            return;
          socket->write(payload);
          socket->disconnectFromHost();
        });
      }
    });
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot start isolated test HTTP server");
  }
  QString url(const QString &name = "fixture.bin") const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(name);
  }
};

class AutomationAdversarialTests : public QObject {
  Q_OBJECT
  static bool finished(const DownloadManager &manager) {
    if (manager.downloads().isEmpty())
      return false;
    for (const auto &item : manager.downloads())
      if (item.toMap()["status"] != "complete")
        return false;
    return true;
  }
  static bool dangerous(const QList<QStringList> &commands) {
    for (const auto &command : commands)
      if (command.value(0) == "systemctl" || command.value(0) == "nmcli")
        return true;
    return false;
  }
  static Automation::Executor quiet() {
    return [](const QStringList &, auto done) { done(0, {}); };
  }
  static void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
      qFatal("Cannot write isolated test fixture");
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  }
  static QByteArray frame(const QByteArray &body) {
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(body.size(), bytes.data());
    return bytes + body;
  }
  static int installer(const QString &home, const QString &binary,
                       const QString &share) {
    QProcess process;
    process.start("/usr/bin/python3",
                  {QFINDTESTDATA("../tools/install_browser.py"), "--home", home,
                   "--binary", binary, "--share", share});
    if (!process.waitForFinished(5000)) {
      process.kill();
      process.waitForFinished();
      return -1;
    }
    return process.exitCode();
  }
  static void installerFixture(const QString &root, QString &home,
                               QString &binary, QString &share) {
    home = root + "/home";
    binary = root + "/fixture binary";
    share = root + "/share";
    QDir().mkpath(home);
    QDir().mkpath(share + "/browser/chromium");
    write(binary, "#!/bin/sh\nexit 0\n");
    write(share + "/browser/chromium/manifest.json", "{}");
  }
private slots:
  void mixedAntivirusResultsNeverScheduleCompletionAction() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    QList<std::function<void(int, const QString &)>> scans;
    QList<QStringList> commands;
    Automation automation(&manager, temp.path() + "/state", nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            if (args.value(0) == "fixture-scan")
                              scans.append(done);
                            else
                              done(0, {});
                          });
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/out"},
         {"notifications", false},
         {"antivirus", true},
         {"scannerCommand", QStringList{"fixture-scan", "%f"}},
         {"completionAction", "shutdown"},
         {"completionDelaySeconds", 5}}));
    QVERIFY(!automation.enqueue({{"url", server.url("one.bin")}}).isEmpty());
    QVERIFY(!automation.enqueue({{"url", server.url("two.bin")}}).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(scans.size(), 2, 5000);
    scans[0](1, "fixture reports infected file");
    scans[1](0, {});
    QVERIFY(automation.error().contains("Antivirus", Qt::CaseInsensitive));
    QVERIFY2(automation.completionNotice().isEmpty(),
             "A successful second scan re-armed shutdown after the first scan "
             "failed");
    QVERIFY(!dangerous(commands));
  }
  void invalidPersistedCancellationDelayCannotDispatchPoweroff() {
    AdversarialServer server;
    QTemporaryDir temp;
    const QString state = temp.path() + "/state";
    QDir().mkpath(state);
    write(state + "/automation.json",
          QJsonDocument(
              QJsonObject{{"version", 1},
                          {"preferences",
                           QJsonObject{{"directory", temp.path() + "/out"},
                                       {"notifications", false},
                                       {"completionAction", "shutdown"},
                                       {"completionDelaySeconds", 0}}}})
              .toJson());
    DownloadManager manager(state);
    QList<QStringList> commands;
    Automation automation(&manager, state, nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    const auto id = automation.enqueue({{"url", server.url()}});
    if (!id.isEmpty())
      QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QTest::qWait(50);
    QVERIFY2(!dangerous(commands), "Persisted invalid zero cancellation delay "
                                   "dispatched a poweroff command");
    QVERIFY2(!automation.error().isEmpty(),
             "Invalid persisted safety settings must be reported");
  }
  void invalidSavedSafetyPolicyCannotActuateThroughAnExistingJobResume() {
    AdversarialServer server;
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    {
      DownloadManager manager(state);
      QVERIFY(!manager
                   .addRequest({{"url", server.url()},
                                {"directory", temp.path() + "/out"},
                                {"start", false}})
                   .isEmpty());
    }
    write(state + "/automation.json",
          QJsonDocument(
              QJsonObject{{"version", 1},
                          {"preferences",
                           QJsonObject{{"directory", temp.path() + "/out"},
                                       {"notifications", false},
                                       {"completionAction", "shutdown"},
                                       {"completionDelaySeconds", 0}}}})
              .toJson());
    DownloadManager manager(state);
    QList<QStringList> commands;
    Automation automation(&manager, state, nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    QVERIFY(!automation.error().isEmpty());
    automation.resumeAll();
    QTest::qWait(300);
    if (server.requests > 0)
      QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QTest::qWait(50);
    QVERIFY2(!dangerous(commands),
             "Invalid saved action policy executed through resumeAll on an "
             "existing job despite readonly settings");
  }
  void hugeConfiguredDelayCannotOverflowIntoImmediatePoweroff() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    QList<QStringList> commands;
    Automation automation(&manager, temp.path() + "/state", nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    const bool accepted = automation.configure(
        {{"directory", temp.path() + "/out"},
         {"notifications", false},
         {"completionAction", "shutdown"},
         {"completionDelaySeconds", std::numeric_limits<int>::max()}});
    if (!accepted) {
      QVERIFY(!automation.error().isEmpty());
      return;
    }
    QVERIFY(!automation.enqueue({{"url", server.url()}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QTest::qWait(50);
    QVERIFY2(!dangerous(commands),
             "Accepted long delay overflowed the millisecond timer and "
             "dispatched poweroff immediately");
  }
  void failedPreferenceSaveDoesNotChangeLivePolicy() {
    QTemporaryDir temp;
    const QString state = temp.path() + "/state";
    DownloadManager manager(state);
    Automation automation(&manager, state, nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/out"}, {"maxConcurrent", 2}}));
    QVERIFY(QFile::remove(state + "/automation.json"));
    QVERIFY(QDir().mkdir(state + "/automation.json"));
    QVERIFY(!automation.configure({{"maxConcurrent", 9}}));
    QCOMPARE(automation.preferences()["maxConcurrent"].toInt(), 2);
  }
  void duplicateBrowserHandoffCannotAcknowledgeAStillPausedJob() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/out"}, {"notifications", false}}));
    const auto original =
        automation.enqueue({{"url", server.url()}, {"start", false}});
    QVERIFY(!original.isEmpty());
    const auto accepted =
        automation.enqueue({{"url", server.url()}, {"start", true}});
    if (accepted.isEmpty()) {
      QVERIFY(!automation.error().isEmpty());
      return;
    }
    QCOMPARE(accepted, original);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests > 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
  }
  void malformedSettingsAreKeptAndCannotBeSilentlyOverwritten() {
    QTemporaryDir temp;
    const QString state = temp.path() + "/state";
    QDir().mkpath(state);
    const QByteArray original = "{this is broken settings";
    write(state + "/automation.json", original);
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(!automation.error().isEmpty());
      QVERIFY(!automation.configure({{"maxConcurrent", 1}}));
    }
    QFile saved(state + "/automation.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), original);
  }
  void zeroRetryBudgetDoesNotLoopFailedRequests() {
    AdversarialServer server;
    server.status = 503;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"notifications", false},
                                  {"retryCount", 0},
                                  {"retryDelaySeconds", 1}}));
    const auto id = automation.enqueue({{"url", server.url()}});
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(
        manager.downloads().first().toMap()["status"].toString(),
        QString("error"), 5000);
    QTest::qWait(1200);
    QCOMPARE(server.requests, 1);
    QCOMPARE(automation.jobPolicies()[id].toMap()["armed"].toBool(), false);
  }
  void exportNeverReplacesExistingOrSymlinkedUserFiles() {
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    const auto target = temp.path() + "/list.json";
    write(target, "original user data");
    QVERIFY(!automation.exportList(target));
    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("original user data"));
    file.close();
    QVERIFY(QFile::link(target, temp.path() + "/alias.json"));
    QVERIFY(!automation.exportList(temp.path() + "/alias.json"));
    QVERIFY(automation.exportList(temp.path() + "/fresh.json"));
  }
  void scheduledRateLimitActuallyConstrainsTheTransfer() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    const auto now = QTime::currentTime();
    QVariantList schedule{
        QVariantMap{{"start", now.addSecs(-60).toString("HH:mm")},
                    {"end", now.addSecs(60).toString("HH:mm")},
                    {"rateLimit", 1024}}};
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"notifications", false},
                                  {"speedSchedule", schedule}}));
    QVERIFY(!automation.enqueue({{"url", server.url()}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(server.requests > 0, 2000);
    QTest::qWait(200);
    const auto job = manager.downloads().first().toMap();
    QCOMPARE(job["rateLimitBytes"].toLongLong(), qint64(1024));
    QVERIFY2(job["status"] != "complete",
             "Stored speed schedule did not constrain a real transfer");
    QVERIFY(job["received"].toLongLong() < server.body.size());
    automation.pauseAll();
  }
  void periodicWebsiteScheduleRunsTheRealWorkerAndDoesNotRunAgainEarly() {
    AdversarialServer server;
    server.body =
        "<!doctype html><html><body>scheduled mirror fixture</body></html>";
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVariantList jobs{QVariantMap{
        {"url", server.url("site")},
        {"directory", temp.path() + "/site"},
        {"depth", 0},
        {"intervalMinutes", 1},
        {"nextRun",
         QDateTime::currentDateTime().addSecs(-1).toString(Qt::ISODate)}}};
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"syncJobs", jobs},
                                  {"notifications", false}}));
    automation.checkSchedules();
    QTRY_COMPARE_WITH_TIMEOUT(automation.grabber()["status"].toString(),
                              QString("complete"), 5000);
    QFile output(temp.path() + "/site/index.html");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QVERIFY(output.readAll().contains("scheduled mirror fixture"));
    QCOMPARE(server.requests, 1);
    const auto next = QDateTime::fromString(automation.preferences()["syncJobs"]
                                                .toList()
                                                .first()
                                                .toMap()["nextRun"]
                                                .toString(),
                                            Qt::ISODate);
    QVERIFY(next > QDateTime::currentDateTime());
    automation.checkSchedules();
    QTest::qWait(100);
    QCOMPARE(server.requests, 1);
  }
  void partialImportStaysPausedAndCannotImportPrivateHeaders() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/out"}, {"notifications", false}}));
    const auto file = temp.path() + "/import.json";
    write(file,
          QJsonDocument(
              QJsonObject{
                  {"downloads",
                   QJsonArray{
                       QJsonObject{{"url", server.url()},
                                   {"headers",
                                    QJsonObject{{"Cookie",
                                                 "untrusted-import-secret"}}}},
                       QJsonObject{{"url", "file:///etc/passwd"}}}}})
              .toJson());
    QVERIFY(!automation.importList(file));
    QCOMPARE(manager.downloads().size(), 1);
    QCOMPARE(manager.downloads().first().toMap()["status"].toString(),
             QString("paused"));
    QCOMPARE(server.requests, 0);
    automation.resumeAll();
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QVERIFY(!server.lastRequest.contains("untrusted-import-secret"));
  }
  void browserHostInstallerPreservesAnUnrelatedRegularLauncher() {
    QTemporaryDir temp;
    QString home, binary, share;
    installerFixture(temp.path(), home, binary, share);
    const auto launcher = home + "/.local/share/idm-native/browser-host";
    QDir().mkpath(QFileInfo(launcher).absolutePath());
    write(launcher, "an unrelated user launcher");
    QVERIFY2(installer(home, binary, share) != 0,
             "Installer silently overwrote an unrelated regular launcher");
    QFile file(launcher);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("an unrelated user launcher"));
  }
  void browserHostInstallerPreservesUnrelatedManifestAndSymlinkTarget() {
    QTemporaryDir temp;
    QString home, binary, share;
    installerFixture(temp.path(), home, binary, share);
    const auto manifest =
        home +
        "/.config/chromium/NativeMessagingHosts/org.earlyhours.idm_native.json";
    QDir().mkpath(QFileInfo(manifest).absolutePath());
    write(manifest, "{\"name\":\"other.host\",\"description\":\"original\"}");
    QVERIFY(installer(home, binary, share) != 0);
    QFile original(manifest);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QVERIFY(original.readAll().contains("other.host"));
    original.close();
    QVERIFY(QFile::remove(manifest));
    const auto launcher = home + "/.local/share/idm-native/browser-host";
    QFile::remove(launcher);
    const auto target = temp.path() + "/original-file";
    write(target, "do not replace");
    QDir().mkpath(QFileInfo(launcher).absolutePath());
    QVERIFY(QFile::link(target, launcher));
    QVERIFY(installer(home, binary, share) != 0);
    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("do not replace"));
  }
  void nativeSocketRejectsMalformedFramesBeforeCallingTheDownloadHandler() {
    QTemporaryDir temp;
    int calls = 0;
    BrowserBridge bridge(
        temp.path(),
        [&](const QVariantMap &) {
          ++calls;
          return QVariantMap{{"ok", true}, {"id", "accepted"}};
        },
        true);
    QVERIFY(bridge.available());
    const auto permissions =
        QFileInfo(BrowserBridge::socketPath(temp.path())).permissions();
    QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup |
                             QFile::ReadOther | QFile::WriteOther)));
    const QList<QByteArray> bodies = {"[]", "{not JSON}",
                                      "{\"command\":\"unsupported\"}"};
    for (const auto &body : bodies) {
      QLocalSocket socket;
      socket.connectToServer(BrowserBridge::socketPath(temp.path()));
      QVERIFY(socket.waitForConnected(1000));
      socket.write(frame(body));
      socket.flush();
      QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() >= 4, 1000);
      const auto bytes = socket.readAll();
      const auto reply = QJsonDocument::fromJson(bytes.mid(4)).object();
      QCOMPARE(reply["ok"].toBool(), false);
      QCOMPARE(calls, 0);
    }
    QLocalSocket socket;
    socket.connectToServer(BrowserBridge::socketPath(temp.path()));
    QVERIFY(socket.waitForConnected(1000));
    QByteArray oversized(4, '\0');
    qToLittleEndian<quint32>(1024 * 1024 + 1, oversized.data());
    socket.write(oversized);
    socket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() >= 4, 1000);
    QCOMPARE(calls, 0);
  }
  void secondNonOwnerCannotRemoveTheLiveNativeSocket() {
    QTemporaryDir temp;
    BrowserBridge owner(
        temp.path(),
        [](const QVariantMap &) {
          return QVariantMap{{"ok", true}, {"id", "accepted"}};
        },
        true);
    QVERIFY(owner.available());
    BrowserBridge observer(
        temp.path(), [](const QVariantMap &) { return QVariantMap{}; }, false);
    QVERIFY(!observer.available());
    QLocalSocket socket;
    socket.connectToServer(BrowserBridge::socketPath(temp.path()));
    QVERIFY(socket.waitForConnected(1000));
    const auto bytes = frame("{\"command\":\"ping\"}");
    for (char byte : bytes) {
      socket.write(&byte, 1);
      socket.flush();
      QCoreApplication::processEvents();
    }
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() >= 4, 1000);
    QVERIFY(QJsonDocument::fromJson(socket.readAll().mid(4))
                .object()["ok"]
                .toBool());
  }
  void startingAWebsiteJobPreventsAnAlreadyPendingPoweroff() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    QList<QStringList> commands;
    Automation automation(&manager, temp.path() + "/state", nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"notifications", false},
                                  {"completionAction", "shutdown"},
                                  {"completionDelaySeconds", 5}}));
    QVERIFY(!automation.enqueue({{"url", server.url()}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!automation.completionNotice().isEmpty(), 1000);
    server.body = "<html><body>held website response</body></html>";
    server.hold = true;
    QVERIFY(
        automation.mirror(server.url("slow-site"), temp.path() + "/site", 0));
    QTRY_COMPARE_WITH_TIMEOUT(server.requests, 2, 2000);
    QTest::qWait(5200);
    QVERIFY2(!dangerous(commands), "Pending poweroff fired while a later "
                                   "website download was still running");
    QCOMPARE(automation.grabber()["status"].toString(), QString("downloading"));
    automation.cancelMirror();
  }
  void sitePoliciesDoNotLeakCredentialsToAnotherOrigin() {
    AdversarialServer server, otherPort;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVariantMap policy{
        {automation.siteOrigin(server.url()),
         QVariantMap{
             {"directory", temp.path() + "/private-site"},
             {"username", "private-user"}, {"password", "private-password"},
             {"headers", QVariantMap{{"Cookie", "private-site=only"},
                                     {"Authorization", "Bearer private-token"},
                                     {"X-Api-Key", "private-api-key"}}}}}};
    QVERIFY(automation.configure({{"directory", temp.path() + "/global"},
                                  {"notifications", false},
                                  {"siteSettings", policy}}));
    auto otherHost = server.url("other-host.bin");
    otherHost.replace("127.0.0.1", "localhost");
    QVERIFY(!automation.enqueue({{"url", otherHost}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QVERIFY(!server.lastRequest.contains("private-"));
    QVERIFY(!automation.enqueue({{"url", otherPort.url("other-port.bin")}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QVERIFY(!otherPort.lastRequest.contains("private-"));
    auto otherScheme = server.url("other-scheme.bin");
    otherScheme.replace("http:", "https:");
    QVERIFY(!automation.enqueue({{"url", otherScheme}, {"start", false}}).isEmpty());
    QVERIFY(!automation.enqueue({{"url", server.url("same-origin.bin")}, {"start", false}}).isEmpty());
    QFile saved(temp.path() + "/state/queue.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto jobs = QJsonDocument::fromJson(saved.readAll()).object().value("downloads").toArray();
    QCOMPARE(jobs.size(), 4);
    for (int i = 0; i < 3; ++i) {
      const auto job = jobs[i].toObject();
      QVERIFY(job.value("username").toString().isEmpty());
      QVERIFY(job.value("password").toString().isEmpty());
      QVERIFY(job.value("headers").toObject().isEmpty());
      QVERIFY(job.value("path").toString().startsWith(temp.path() + "/global/"));
    }
    QCOMPARE(jobs[3].toObject().value("username").toString(), "private-user");
    QCOMPARE(jobs[3].toObject().value("password").toString(), "private-password");
    QCOMPARE(jobs[3].toObject().value("headers").toObject().value("x-api-key").toString(), "private-api-key");
  }
  void legacyHostnamePoliciesStayInertAndRecoverOnlyByExplicitEditing() {
    AdversarialServer server;
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    QVERIFY(QDir().mkpath(state));
    const QJsonObject profile{{"username", "legacy-user"}, {"password", "legacy-password"},
                             {"headers", QJsonObject{{"Cookie", "legacy-secret"}}}};
    write(state + "/automation.json", QJsonDocument(QJsonObject{
        {"version", 1}, {"preferences", QJsonObject{
            {"directory", temp.path() + "/out"}, {"notifications", false},
            {"siteSettings", QJsonObject{{"127.0.0.1", profile}}}}}}).toJson());
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(automation.error().isEmpty());
      QVERIFY(automation.hasLegacySiteSettings());
      QVERIFY(automation.preferences().value("siteSettings").toMap().isEmpty());
      QVERIFY(!automation.enqueue({{"url", server.url()}}).isEmpty());
      QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
      QVERIFY(!server.lastRequest.contains("legacy-"));
      const auto recovered = automation.siteSettingsForEditing(automation.siteOrigin(server.url()));
      QCOMPARE(QJsonObject::fromVariantMap(recovered), profile);
      QVERIFY(automation.preferences().value("siteSettings").toMap().isEmpty());
      QVERIFY(automation.configure({{"siteSettings", QVariantMap{{automation.siteOrigin(server.url()), recovered}}}}));
      QVERIFY(!automation.enqueue({{"url", server.url("recovered.bin")}}).isEmpty());
      QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
      QVERIFY(server.lastRequest.contains("legacy-secret"));
    }
    QFile saved(state + "/automation.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto object = QJsonDocument::fromJson(saved.readAll()).object();
    QCOMPARE(object.value("legacySiteSettings").toObject().value("127.0.0.1").toObject(), profile);
    QCOMPARE(object.value("preferences").toObject().value("siteSettings").toObject().keys(),
             QStringList{QString("http://127.0.0.1:%1").arg(server.serverPort())});
  }
  void installedNativeLauncherQuotesBinaryPathsAndForwardsArguments() {
    QTemporaryDir temp;
    QString home, binary, share;
    installerFixture(temp.path(), home, binary, share);
    const auto result = temp.path() + "/args.txt";
    binary = temp.path() + "/app ' $(touch SHOULD_NOT_EXIST)";
    write(binary,
          "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + result.toUtf8() + "'\n");
    QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner |
                                              QFile::ExeOwner));
    QCOMPARE(installer(home, binary, share), 0);
    QProcess wrapper;
    wrapper.start(home + "/.local/share/idm-native/browser-host",
                  {"fixture-origin"});
    QVERIFY(wrapper.waitForFinished(3000));
    QCOMPARE(wrapper.exitCode(), 0);
    QFile args(result);
    QVERIFY(args.open(QIODevice::ReadOnly));
    QCOMPARE(args.readAll(), QByteArray("browser-host\nfixture-origin\n"));
    QVERIFY(!QFile::exists("SHOULD_NOT_EXIST"));
  }
  void disconnectCompletionDispatchesOnlyTheInjectedCommand() {
    AdversarialServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    QList<QStringList> commands;
    Automation automation(&manager, temp.path() + "/state", nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"notifications", false},
                                  {"completionAction", "disconnect"},
                                  {"completionDelaySeconds", 5}}));
    QVERIFY(!automation.enqueue({{"url", server.url()}}).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(finished(manager), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!automation.completionNotice().isEmpty(), 1000);
    QVERIFY(commands.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(commands.size(), 1, 7000);
    QCOMPARE(commands.first(), QStringList({"nmcli", "networking", "off"}));
  }
};
QTEST_MAIN(AutomationAdversarialTests)
#include "automation_adversarial_tests.moc"
