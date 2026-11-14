#include "automation.h"
#include "downloadmanager.h"
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class SyncServer : public QTcpServer {
public:
  QByteArray body = "version one", etag = "\"one\"";
  QList<QByteArray> requests;
  bool slow = false;
  QMap<QByteArray, QByteArray> routes;
  SyncServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto socket = nextPendingConnection()) {
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, input] {
          input->append(socket->readAll());
          if (!input->contains("\r\n\r\n"))
            return;
          socket->disconnect(this);
          requests.append(*input);
          if (input->contains("If-None-Match: " + etag)) {
            socket->write(
                "HTTP/1.1 304 Unchanged\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
          }
          const auto path = input->split(' ').value(1);
          const auto source = routes.value(path, body);
          const auto match =
              QRegularExpression("Range: bytes=(\\d+)-(\\d+)",
                                 QRegularExpression::CaseInsensitiveOption)
                  .match(QString::fromLatin1(*input));
          const auto first =
              match.hasMatch() ? match.captured(1).toLongLong() : 0;
          const auto last = match.hasMatch()
                                ? qMin<qint64>(source.size() - 1,
                                               match.captured(2).toLongLong())
                                : source.size() - 1;
          const auto bytes = source.mid(first, last - first + 1);
          QByteArray header =
              match.hasMatch()
                  ? "HTTP/1.1 206 Partial\r\nContent-Range: bytes " +
                        QByteArray::number(first) + "-" +
                        QByteArray::number(last) + "/" +
                        QByteArray::number(source.size()) + "\r\n"
                  : "HTTP/1.1 200 OK\r\n";
          header += "ETag: " + etag + "\r\nContent-Type: " +
                    (path.endsWith(".png") ? QByteArray("image/png")
                                           : QByteArray("text/html")) +
                    "\r\nContent-Length: " + QByteArray::number(bytes.size()) +
                    "\r\nConnection: close\r\n\r\n";
          socket->write(header);
          if (!slow || bytes.size() == 1) {
            socket->write(bytes);
            socket->disconnectFromHost();
            return;
          }
          auto timer = new QTimer(socket);
          auto offset = std::make_shared<qsizetype>(0);
          connect(timer, &QTimer::timeout, socket,
                  [socket, timer, bytes, offset] {
                    if (socket->state() != QAbstractSocket::ConnectedState) {
                      timer->stop();
                      return;
                    }
                    socket->write(bytes.mid(*offset, 32768));
                    *offset += 32768;
                    if (*offset >= bytes.size()) {
                      timer->stop();
                      socket->disconnectFromHost();
                    }
                  });
          timer->start(25);
        });
      }
    });
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot listen");
  }
  QString url() const {
    return QString("http://127.0.0.1:%1/file").arg(serverPort());
  }
};

class AutomationSyncTests : public QObject {
  Q_OBJECT
  Automation::Executor quiet() {
    return [](const QStringList &, auto done) { done(0, {}); };
  }
  QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return {};
    return f.readAll();
  }
  void write(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(bytes), bytes.size());
  }
  QVariantMap project(SyncServer &server, const QString &directory) {
    return {{"url", server.url()},
            {"directory", directory},
            {"filename", "file.bin"},
            {"intervalMinutes", 1}};
  }
  void due(Automation &a) {
    auto jobs = a.preferences()["fileSyncJobs"].toList();
    auto job = jobs.first().toMap();
    job["nextRun"] =
        QDateTime::currentDateTime().addSecs(-1).toString(Qt::ISODate);
    jobs[0] = job;
    QVERIFY(a.configure({{"fileSyncJobs", jobs}}));
    a.checkSchedules();
  }
private slots:
  void fileSyncPersistsCompletedEntriesAndValidatorsAcrossRestart() {
    SyncServer server;
    QTemporaryDir temp;
    auto state = temp.path() + "/state", out = temp.path() + "/files";
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(
          a.configure({{"notifications", false},
                       {"fileSyncJobs", QVariantList{project(server, out)}}}));
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(),
          QString("complete"), 5000);
      QCOMPARE(read(out + "/file.bin"), server.body);
      QCOMPARE(a.quota()["usedBytes"].toLongLong(), server.body.size());
      QCOMPARE(manager.downloads().size(), 0);
      a.removeCompleted();
      QCOMPARE(a.syncQueue().size(), 1);
      a.checkSchedules();
      QTest::qWait(100);
      QCOMPARE(server.requests.size(), 1);
    }
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QCOMPARE(a.syncQueue().first().toMap()["status"].toString(),
               QString("complete"));
      const auto used = a.quota()["usedBytes"].toLongLong();
      due(a);
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(),
          QString("complete"), 5000);
      QVERIFY(server.requests.last().contains("If-None-Match: \"one\""));
      QVERIFY(a.syncQueue().first().toMap()["unchanged"].toBool());
      QCOMPARE(a.quota()["usedBytes"].toLongLong(), used);
      server.body = "version two";
      server.etag = "\"two\"";
      due(a);
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(),
          QString("complete"), 5000);
      QCOMPARE(read(out + "/file.bin"), server.body);
      write(out + "/file.bin", "user edit");
      due(a);
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(), QString("error"),
          5000);
      QCOMPARE(read(out + "/file.bin"), QByteArray("user edit"));
      QCOMPARE(a.syncQueue().size(), 1);
    }
  }
  void quotaPausesAndSurvivesRestartThenResumesAtPeriodBoundary() {
    SyncServer server;
    server.body = QByteArray(4 * 1024 * 1024, 'x');
    server.slow = true;
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    QString id;
    qint64 used = 0;
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(a.configure({{"directory", temp.path() + "/out"},
                           {"notifications", false},
                           {"quotaMB", 1},
                           {"quotaPeriodMinutes", 1}}));
      id = a.enqueue({{"url", server.url()}, {"connections", 1}});
      QVERIFY(!id.isEmpty());
      QTRY_VERIFY_WITH_TIMEOUT(a.quota()["paused"].toBool(), 6000);
      QTRY_COMPARE_WITH_TIMEOUT(
          manager.downloads().first().toMap()["status"].toString(),
          QString("paused"), 2000);
      used = a.quota()["usedBytes"].toLongLong();
      QVERIFY(used >= 1024 * 1024);
      QVERIFY(used < server.body.size());
      QVERIFY(a.jobPolicies()[id].toMap()["quotaPaused"].toBool());
      a.resume(id);
      QTest::qWait(100);
      QCOMPARE(manager.downloads().first().toMap()["status"].toString(),
               QString("paused"));
    }
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      a.checkSchedules();
      QCOMPARE(a.quota()["usedBytes"].toLongLong(), used);
      const auto requests = server.requests.size();
      QTest::qWait(100);
      QCOMPARE(server.requests.size(), requests);
    }
    auto object =
        QJsonDocument::fromJson(read(state + "/automation.json")).object();
    auto quota = object["quota"].toObject();
    quota["periodStartedAt"] =
        QDateTime::currentDateTimeUtc().addSecs(-61).toString(Qt::ISODate);
    object["quota"] = quota;
    auto prefs = object["preferences"].toObject();
    prefs["quotaMB"] = 8;
    object["preferences"] = prefs;
    write(state + "/automation.json", QJsonDocument(object).toJson());
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(
          manager.downloads().first().toMap()["status"].toString(),
          QString("complete"), 10000);
      QCOMPARE(read(manager.downloads().first().toMap()["path"].toString()),
               server.body);
      QVERIFY(a.quota()["usedBytes"].toLongLong() < server.body.size());
      QVERIFY(!a.quota()["paused"].toBool());
    }
  }
  void workerQuotaIsDurableAndAutomaticallyRetriesAfterReset() {
    SyncServer server;
    QTemporaryDir temp;
    const auto state = temp.path() + "/state",
               target = temp.path() + "/out/file.bin";
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(
          a.configure({{"quotaMB", 1},
                       {"quotaPeriodMinutes", 1},
                       {"fileSyncJobs",
                        QVariantList{project(server, temp.path() + "/out")}}}));
    }
    auto object =
        QJsonDocument::fromJson(read(state + "/automation.json")).object();
    auto quota = object["quota"].toObject();
    quota["usedBytes"] = 1024 * 1024 - 5;
    object["quota"] = quota;
    write(state + "/automation.json", QJsonDocument(object).toJson());
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(),
          QString("quotaPaused"), 5000);
      QCOMPARE(a.quota()["usedBytes"].toLongLong(), 1024 * 1024);
      QVERIFY(!QFile::exists(target));
    }
    object = QJsonDocument::fromJson(read(state + "/automation.json")).object();
    quota = object["quota"].toObject();
    quota["periodStartedAt"] =
        QDateTime::currentDateTimeUtc().addSecs(-61).toString(Qt::ISODate);
    object["quota"] = quota;
    write(state + "/automation.json", QJsonDocument(object).toJson());
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(
          a.syncQueue().first().toMap()["status"].toString(),
          QString("complete"), 5000);
      QCOMPARE(read(target), QByteArray("version one"));
      QCOMPARE(a.quota()["usedBytes"].toLongLong(), 6);
      QVERIFY(server.requests.last().contains("Range: bytes=5-10"));
      QVERIFY(server.requests.last().contains("If-Range: \"one\""));
    }
  }
  void immediateGrabberAlsoResumesAfterQuotaAndRestart() {
    SyncServer server;
    server.body = "<html>direct grab</html>";
    QTemporaryDir temp;
    const auto state = temp.path() + "/state", site = temp.path() + "/site";
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(a.configure({{"quotaMB", 1}, {"quotaPeriodMinutes", 1}}));
    }
    auto object =
        QJsonDocument::fromJson(read(state + "/automation.json")).object();
    auto quota = object["quota"].toObject();
    quota["usedBytes"] = 1024 * 1024 - 5;
    object["quota"] = quota;
    write(state + "/automation.json", QJsonDocument(object).toJson());
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(a.mirror(server.url(), site, 0));
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("quotaPaused"), 5000);
    }
    object = QJsonDocument::fromJson(read(state + "/automation.json")).object();
    QVERIFY(!object["pendingMirror"].toObject().isEmpty());
    quota = object["quota"].toObject();
    quota["periodStartedAt"] =
        QDateTime::currentDateTimeUtc().addSecs(-61).toString(Qt::ISODate);
    object["quota"] = quota;
    write(state + "/automation.json", QJsonDocument(object).toJson());
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("complete"), 5000);
      QVERIFY(read(site + "/index.html").contains("direct grab"));
    }
    QVERIFY(QJsonDocument::fromJson(read(state + "/automation.json"))
                .object()["pendingMirror"]
                .toObject()
                .isEmpty());
  }
  void corruptQuotaCannotBeSilentlyResetOnRestart() {
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
    }
    auto object =
        QJsonDocument::fromJson(read(state + "/automation.json")).object();
    auto quota = object["quota"].toObject();
    quota["usedBytes"] = -1;
    object["quota"] = quota;
    const auto bytes = QJsonDocument(object).toJson();
    write(state + "/automation.json", bytes);
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(!a.error().isEmpty());
      QVERIFY(!a.configure({{"quotaMB", 0}}));
    }
    QCOMPARE(read(state + "/automation.json"), bytes);
  }
  void scheduledGrabberStartsOnceStopsAndKeepsItsScheduleAcrossRestart() {
    SyncServer server;
    server.body = "<html><body>scheduled</body></html>";
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    auto job = project(server, temp.path() + "/site");
    job.remove("filename");
    job["intervalMinutes"] = 0;
    job["startAt"] =
        QDateTime::currentDateTime().addSecs(3600).toString(Qt::ISODate);
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(a.configure({{"syncJobs", QVariantList{job}}}));
      a.checkSchedules();
      QCOMPARE(server.requests.size(), 0);
    }
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QCOMPARE(a.preferences()["syncJobs"].toList().first().toMap()["startAt"],
               job["startAt"]);
      job["startAt"] =
          QDateTime::currentDateTime().addSecs(-1).toString(Qt::ISODate);
      QVERIFY(a.configure({{"syncJobs", QVariantList{job}}}));
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("complete"), 5000);
      QVERIFY(read(temp.path() + "/site/index.html").contains("scheduled"));
      a.checkSchedules();
      QTest::qWait(100);
      QCOMPARE(server.requests.size(), 1);
      job["directory"] = temp.path() + "/slow";
      job["stopAt"] =
          QDateTime::currentDateTime().addSecs(2).toString(Qt::ISODate);
      server.body = QByteArray(8 * 1024 * 1024, 'x');
      server.slow = true;
      QVERIFY(a.configure({{"syncJobs", QVariantList{job}}}));
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("paused"), 5000);
      QVERIFY(!QFile::exists(temp.path() + "/slow/index.html"));
    }
  }
  void grabberFiltersReachTheWorkerAndPersist() {
    SyncServer server;
    server.body = "<html><img src='/yes.png'><img src='/no.png'></html>";
    server.routes["/yes.png"] = "yes image";
    server.routes["/no.png"] = "no image";
    QTemporaryDir temp;
    const auto state = temp.path() + "/state", site = temp.path() + "/site";
    QVariantMap job{{"url", server.url()},
                    {"directory", site},
                    {"depth", 0},
                    {"intervalMinutes", 1},
                    {"include", QStringList{"*.png"}},
                    {"exclude", QStringList{"no*"}}};
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      QVERIFY(a.configure({{"syncJobs", QVariantList{job}}}));
      a.checkSchedules();
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("complete"), 5000);
      QVERIFY(!QFile::exists(site + "/index.html"));
      const auto files = QDir(site + "/files").entryList(QDir::Files);
      QCOMPARE(files.size(), 1);
      QCOMPARE(read(site + "/files/" + files.first()), QByteArray("yes image"));
      QCOMPARE(server.requests.size(), 2);
    }
    {
      DownloadManager manager(state);
      Automation a(&manager, state, nullptr, quiet());
      const auto saved = a.preferences()["syncJobs"].toList().first().toMap();
      QCOMPARE(saved["include"].toStringList(), QStringList{"*.png"});
      QCOMPARE(saved["exclude"].toStringList(), QStringList{"no*"});
      QVERIFY(a.mirror(server.url(), temp.path() + "/immediate", 0,
                       {{"include", QStringList{"*.png"}},
                        {"exclude", QStringList{"no*"}}}));
      QTRY_COMPARE_WITH_TIMEOUT(a.grabber()["status"].toString(),
                                QString("complete"), 5000);
      QCOMPARE(
          QDir(temp.path() + "/immediate/files").entryList(QDir::Files).size(),
          1);
    }
  }
  void completionProgramRunsAfterDownloadsAndCanBeCancelled() {
    SyncServer server;QTemporaryDir temp;
    const auto state=temp.path()+"/state", marker=temp.path()+"/completed.txt";
    DownloadManager manager(state);Automation a(&manager,state);
    QVERIFY(!a.configure({{"completionAction","program"}}));
    QVERIFY(a.configure({{"directory",temp.path()+"/out"},{"notifications",false},{"sound",false},
      {"completionAction","program"},{"completionDelaySeconds",5},
      {"completionCommand",QStringList{"/usr/bin/python3","-c","import pathlib,sys;pathlib.Path(sys.argv[1]).write_text('completed')",marker}}}));
    const auto id=a.enqueue({{"url",server.url()}});QVERIFY(!id.isEmpty());
    QTRY_VERIFY(!a.completionNotice().isEmpty());
    a.cancelCompletionAction();QTest::qWait(5100);QVERIFY(!QFile::exists(marker));
    const auto next=a.enqueue({{"url",server.url()+"?second"}});QVERIFY(!next.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(read(marker),QByteArray("completed"),8000);
  }
  void failedProjectPreparationDoesNotConsumeItsScheduledInterval() {
    SyncServer server; QTemporaryDir temp;
    const auto directory=temp.path()+"/out";
    write(directory,"blocking file");
    DownloadManager manager(temp.path()+"/state");
    Automation a(&manager,temp.path()+"/state",nullptr,quiet());
    auto job=project(server,directory);job["intervalMinutes"]=525600;
    QVERIFY(a.configure({{"fileSyncJobs",QVariantList{job}}}));
    a.checkSchedules();
    QVERIFY(a.preferences()["fileSyncJobs"].toList().first().toMap()["nextRun"].toString().isEmpty());
    QVERIFY(QFile::remove(directory));
    a.checkSchedules();
    QTRY_COMPARE_WITH_TIMEOUT(a.syncQueue().first().toMap()["status"].toString(),QString("complete"),5000);
    QCOMPARE(read(directory+"/file.bin"),QByteArray("version one"));
  }
  void cancellationRemovesPrivateJobAndFlushesReceivedQuota() {
    SyncServer server;server.body=QByteArray(1024*1024,'x');server.slow=true;
    QTemporaryDir temp;const auto state=temp.path()+"/state";
    DownloadManager manager(state);Automation a(&manager,state,nullptr,quiet());
    QVERIFY(a.configure({{"fileSyncJobs",QVariantList{project(server,temp.path()+"/out")}}}));
    a.checkSchedules();
    QTRY_VERIFY_WITH_TIMEOUT(a.quota()["usedBytes"].toLongLong()>0,5000);
    a.cancelMirror();
    QVERIFY(!QFile::exists(state+"/website-job.json"));
    const auto saved=QJsonDocument::fromJson(read(state+"/automation.json")).object();
    QCOMPARE(saved.value("quota").toObject().value("usedBytes").toVariant().toLongLong(),a.quota()["usedBytes"].toLongLong());
  }
  void rejectsMalformedNewConfigurationWithoutChangingSavedSettings() {
    SyncServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation a(&manager, temp.path() + "/state", nullptr, quiet());
    const auto original = a.preferences();
    for (const auto &bad : QVariantList{-1, 1.2, true, QString("NaN")})
      QVERIFY(!a.configure({{"quotaMB", bad}}));
    QVERIFY(!a.configure({{"quotaPeriodMinutes", 0}}));
    QVERIFY(!a.configure({{"fileSyncJobs", QVariantMap{}}}));
    auto job = project(server, temp.path() + "/out");
    for (const auto &field : QStringList{"nextRun", "startAt", "stopAt"}) {
      auto bad = job;
      bad[field] = "bad date";
      QVERIFY(!a.configure({{"fileSyncJobs", QVariantList{bad}}}));
    }
    for (const auto &name :
         QStringList{"../escape", "a/b", "a\\b", ".idm-site.lock"}) {
      auto bad = job;
      bad["filename"] = name;
      QVERIFY(!a.configure({{"fileSyncJobs", QVariantList{bad}}}));
    }
    auto bad = job;
    bad["intervalMinutes"] = 1.5;
    QVERIFY(!a.configure({{"fileSyncJobs", QVariantList{bad}}}));
    QVERIFY(!a.configure({{"fileSyncJobs", QVariantList{job, job}}}));
    bad.remove("filename");
    bad["intervalMinutes"] = 1;
    bad["include"] = "*.png";
    QVERIFY(!a.configure({{"syncJobs", QVariantList{bad}}}));
    QCOMPARE(a.preferences(), original);
    QVERIFY(!a.mirror(server.url(), temp.path() + "/site", 0,
                      {{"exclude", QVariantList{QString(257, 'x')}}}));
    QCOMPARE(server.requests.size(), 0);
  }
};
QTEST_MAIN(AutomationSyncTests)
#include "automation_sync_tests.moc"
