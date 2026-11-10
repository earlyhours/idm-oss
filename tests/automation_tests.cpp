#include "automation.h"
#include "downloadmanager.h"
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class Fixture : public QTcpServer {
public:
  QByteArray body = QByteArray(48 * 1024, 'z');
  int failures = 0, errorCode = 503, requests = 0, active = 0, maxActive = 0;
  QStringList paths;
  QByteArray lastRequest;
  bool slow = false;
  Fixture() {
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
          paths.append(QString::fromLatin1(input->split(' ').value(1)));
          ++active;
          maxActive = qMax(maxActive, active);
          connect(socket, &QTcpSocket::disconnected, this,
                  [this] { --active; });
          const int code = failures-- > 0 ? errorCode : 200;
          const auto bytes = code == 200 ? body : QByteArray("failed");
          socket->write("HTTP/1.1 " + QByteArray::number(code) +
                        " Response\r\nContent-Length: " +
                        QByteArray::number(bytes.size()) +
                        "\r\nConnection: close\r\n\r\n");
          if (!slow || code != 200) {
            socket->write(bytes);
            socket->disconnectFromHost();
            return;
          }
          auto *timer = new QTimer(socket);
          auto offset = std::make_shared<int>(0);
          connect(timer, &QTimer::timeout, socket,
                  [socket, timer, offset, bytes] {
                    if (socket->state() != QAbstractSocket::ConnectedState) {
                      timer->stop();
                      return;
                    }
                    socket->write(bytes.mid(*offset, 1024));
                    *offset += 1024;
                    if (*offset >= bytes.size()) {
                      timer->stop();
                      socket->disconnectFromHost();
                    }
                  });
          timer->start(10);
        });
      }
    });
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot listen");
  }
  QString url(const QString &path = "test.bin") const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(path);
  }
};
class AutomationTests : public QObject {
  Q_OBJECT
  Automation::Executor quiet() {
    return [](const QStringList &, auto done) { done(0, {}); };
  }
  bool complete(DownloadManager &manager) {
    if (manager.downloads().isEmpty())
      return false;
    for (const auto &value : manager.downloads())
      if (value.toMap()["status"] != "complete")
        return false;
    return true;
  }
  QVariantMap first(DownloadManager &manager) {
    return manager.downloads().first().toMap();
  }
private slots:
  void settingsPersistAndRejectInvalidValues() {
    QTemporaryDir dir;
    QDir().mkpath(dir.path() + "/state");
    {
      DownloadManager manager(dir.path() + "/state");
      Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
      QVERIFY(automation.configure({{"directory", dir.path() + "/out"},
                                    {"maxConcurrent", 4},
                                    {"clipboardMonitor", true},
                                    {"darkMode", true}}));
      QVERIFY(!automation.configure({{"maxConcurrent", 0}}));
      QCOMPARE(automation.preferences()["maxConcurrent"].toInt(), 4);
      QVERIFY(!automation.configure(
          {{"completionAction", "execute arbitrary shell"}}));
      QVERIFY(!(QFileInfo(dir.path() + "/state/automation.json").permissions() &
                (QFile::ReadOther | QFile::ReadGroup)));
    }
    {
      DownloadManager manager(dir.path() + "/state");
      Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
      QCOMPARE(automation.preferences()["maxConcurrent"].toInt(), 4);
      QCOMPARE(automation.preferences()["darkMode"].toBool(), true);
    }
  }
  void pausedBatchPriorityAndConcurrency() {
    Fixture server;
    server.slow = true;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", dir.path() + "/out"}, {"maxConcurrent", 1}}));
    auto low = automation.enqueue(
        {{"url", server.url("low.bin")}, {"priority", -10}, {"start", false}});
    auto high = automation.enqueue(
        {{"url", server.url("high.bin")}, {"priority", 10}, {"start", false}});
    QVERIFY(!low.isEmpty() && !high.isEmpty());
    QTest::qWait(50);
    QCOMPARE(server.requests, 0);
    automation.startQueue();
    QTRY_VERIFY_WITH_TIMEOUT(complete(manager), 10000);
    QCOMPARE(server.paths.first(), QString("/high.bin"));
    QCOMPARE(server.maxActive, 1);
    automation.removeCompleted();
    QCOMPARE(manager.downloads().size(), 0);
    QVERIFY(QFile::exists(dir.path() + "/out/high.bin"));
  }
  void duplicatesAndTypeFilters() {
    Fixture server;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
    automation.configure(
        {{"directory", dir.path() + "/out"}, {"typeFilter", "*.ZIP; bin"}});
    auto id = automation.enqueue({{"url", server.url()}, {"start", false}});
    QVERIFY(!id.isEmpty());
    QCOMPARE(automation.enqueue({{"url", server.url()}, {"start", false}}), id);
    QCOMPARE(manager.downloads().size(), 1);
    QVERIFY(
        automation
            .enqueue({{"url", server.url("excluded.exe")}, {"start", false}})
            .isEmpty());
    QVERIFY(automation.error().contains("filter"));
  }
  void transientErrorsRetryButExpiredLinksDoNot() {
    Fixture server;
    server.failures = 1;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
    automation.configure({{"directory", dir.path() + "/out"},
                          {"retryDelaySeconds", 1},
                          {"retryCount", 2}});
    automation.enqueue({{"url", server.url()}});
    QTRY_VERIFY_WITH_TIMEOUT(complete(manager), 10000);
    QCOMPARE(server.requests, 2);
    server.errorCode = 403;
    server.failures = 100;
    const auto expired =
        automation.enqueue({{"url", server.url("expired.bin")}});
    QVERIFY(!expired.isEmpty());
    QTRY_COMPARE(manager.downloads().last().toMap()["status"].toString(),
                 QString("error"));
    QTest::qWait(2200);
    QCOMPARE(server.requests, 3);
    QCOMPARE(automation.jobPolicies()[expired].toMap()["armed"].toBool(),
             false);
  }
  void schedulesStartAndStopQueues() {
    Fixture server;
    server.slow = true;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
    automation.configure({{"directory", dir.path() + "/out"}});
    auto id = automation.enqueue(
        {{"url", server.url()}, {"queue", "Scheduled"}, {"start", false}});
    QVERIFY(!id.isEmpty());
    QVERIFY(automation.setQueue(
        "Scheduled",
        {{"startAt",
          QDateTime::currentDateTime().addSecs(-1).toString(Qt::ISODate)}}));
    automation.checkSchedules();
    QTRY_VERIFY(server.requests > 0);
    QVERIFY(automation.setQueue(
        "Scheduled",
        {{"stopAt",
          QDateTime::currentDateTime().addSecs(-1).toString(Qt::ISODate)}}));
    automation.checkSchedules();
    QCOMPARE(first(manager)["status"].toString(), QString("paused"));
  }
  void historyExportImportAvoidsBrowserSecrets() {
    Fixture server;
    QTemporaryDir dir;
    QString file = dir.path() + "/list.json";
    {
      DownloadManager manager(dir.path() + "/one");
      Automation automation(&manager, dir.path() + "/one", nullptr, quiet());
      automation.configure({{"directory", dir.path() + "/out"}});
      automation.enqueue(
          {{"url", server.url()},
           {"headers", QVariantMap{{"Cookie", "private=secret"}}},
           {"description", "fixture"},
           {"start", false}});
      QVERIFY(automation.exportList(file));
      QFile exported(file);
      QVERIFY(exported.open(QIODevice::ReadOnly));
      const auto bytes = exported.readAll();
      QVERIFY(!bytes.contains("private=secret"));
      QVERIFY(bytes.contains("fixture"));
    }
    {
      DownloadManager manager(dir.path() + "/two");
      Automation automation(&manager, dir.path() + "/two", nullptr, quiet());
      automation.configure({{"directory", dir.path() + "/out2"}});
      QVERIFY(automation.importList(file));
      QCOMPARE(manager.downloads().size(), 1);
      QCOMPARE(first(manager)["status"].toString(), QString("paused"));
      QCOMPARE(server.requests, 0);
    }
  }
  void siteHeadersAndClipboardConfirmation() {
    Fixture server;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    Automation automation(&manager, dir.path() + "/state", nullptr, quiet());
    automation.configure(
        {{"clipboardMonitor", true},
         {"siteSettings",
          QVariantMap{
              {"127.0.0.1",
               QVariantMap{{"directory", dir.path() + "/site"},
                           {"headers",
                            QVariantMap{{"User-Agent", "IDM-Site-Test"},
                                        {"Referer", server.url("page")}}}}}}}});
    QSignalSpy urls(&automation, &Automation::clipboardUrl);
    QGuiApplication::clipboard()->setText(server.url());
    QTRY_COMPARE(urls.size(), 1);
    QCOMPARE(manager.downloads().size(), 0);
    automation.enqueue({{"url", server.url()}});
    QTRY_VERIFY(complete(manager));
    QVERIFY(server.lastRequest.contains("User-Agent: IDM-Site-Test"));
    QVERIFY(
        first(manager)["path"].toString().startsWith(dir.path() + "/site/"));
  }
  void scannerNotificationsSoundAndCancelableCompletion() {
    Fixture server;
    QTemporaryDir dir;
    DownloadManager manager(dir.path() + "/state");
    QList<QStringList> commands;
    Automation automation(&manager, dir.path() + "/state", nullptr,
                          [&](const QStringList &args, auto done) {
                            commands.append(args);
                            done(0, {});
                          });
    QVERIFY(automation.configure(
        {{"directory", dir.path() + "/out"},
         {"antivirus", true},
         {"scannerCommand", QStringList{"fixture-scanner", "--file", "%f"}},
         {"notifications", true},
         {"sound", true},
         {"completionAction", "shutdown"},
         {"completionDelaySeconds", 5}}));
    automation.enqueue({{"url", server.url()}});
    QTRY_VERIFY(complete(manager));
    QTRY_VERIFY(!automation.completionNotice().isEmpty());
    QCOMPARE(commands.size(), 3);
    QCOMPARE(commands[0].first(), QString("fixture-scanner"));
    QCOMPARE(commands[0].last(), first(manager)["path"].toString());
    automation.cancelCompletionAction();
    QVERIFY(automation.completionNotice().isEmpty());
    QTest::qWait(5100);
    QCOMPARE(commands.size(), 3);
    automation.enqueue({{"url", server.url("another.bin")}});
    QTRY_VERIFY(complete(manager));
    QTRY_VERIFY_WITH_TIMEOUT(commands.size() == 7, 8000);
    QCOMPARE(commands.last(), QStringList({"systemctl", "poweroff"}));
  }
};
QTEST_MAIN(AutomationTests)
#include "automation_tests.moc"
