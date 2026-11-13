#include "automation.h"
#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class SystemHttpFixture : public QTcpServer {
public:
  QByteArray clean = "Clean IDM Native post-download scan fixture.\n";
  static QByteArray eicar() {
    return QByteArray("X5O!P%@AP[4\\PZX54(P^)7CC)7}$") +
           "EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";
  }
  SystemHttpFixture() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto socket = nextPendingConnection()) {
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, input] {
          *input += socket->readAll();
          if (!input->contains("\r\n\r\n")) return;
          socket->disconnect(this);
          const auto body = input->startsWith("GET /eicar") ? eicar() : clean;
          socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                        QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
    if (!listen(QHostAddress::LocalHost)) qFatal("Cannot open private test HTTP server");
  }
  QString url(const QString &name) const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(name);
  }
};

class SystemIntegrationTests : public QObject {
  Q_OBJECT
  QString scanner;
  static QByteArray read(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
  }
  static void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || file.write(bytes) != bytes.size()) qFatal("Cannot write private antivirus fixture");
  }
  QStringList scanCommand(const QString &root) const {
    const auto database = root + "/custom.hdb";
    const auto payload = SystemHttpFixture::eicar();
    write(database, QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex() + ":68:IDM.Native.EICAR.Test\n");
    return {scanner, "--database", database, "--no-summary", "%f"};
  }
  static QString scanState(const Automation &automation, const QString &id) {
    return automation.jobPolicies().value(id).toMap().value("scanStatus").toString();
  }
  static QVariantMap job(const DownloadManager &manager, const QString &id) {
    for (const auto &value : manager.downloads()) {
      if (value.toMap().value("id").toString() == id) return value.toMap();
    }
    return {};
  }
  static void scanReal(const QStringList &command, std::function<void(int, const QString &)> done, QList<int> &results) {
    QProcess process;
    process.start(command.first(), command.mid(1));
    if (!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(1000); results.append(-1); done(-1, "Scanner timed out in isolated test"); return; }
    const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    results.append(code); done(code, QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError()));
  }
private slots:
  void initTestCase() {
    scanner = QStandardPaths::findExecutable("clamscan");
    QVERIFY2(!scanner.isEmpty(), "Install the free clamav package before running system tests");
    QCOMPARE(SystemHttpFixture::eicar().size(), 68);
    QCOMPARE(QCryptographicHash::hash(SystemHttpFixture::eicar(), QCryptographicHash::Md5).toHex(), QByteArray("44d88612fea8a8f36de82e1278abb02f"));
  }
  void defaultExecutorScansActualCleanDownload() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state");
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"}, {"antivirus", true}, {"scannerCommand", scanCommand(temp.path())}, {"notifications", false}, {"sound", false}, {"completionAction", "none"}}));
    const auto id = automation.enqueue({{"url", server.url("clean.txt")}}); QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(scanState(automation, id), QString("clean"), 10000);
    QCOMPARE(job(manager,id).value("status").toString(), QString("complete")); QCOMPARE(read(job(manager,id).value("path").toString()), server.clean);
    QVERIFY2(automation.error().isEmpty(), qPrintable(automation.error()));
  }
  void defaultExecutorFlagsActualEicarDownload() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state");
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"}, {"antivirus", true}, {"scannerCommand", scanCommand(temp.path())}, {"notifications", false}, {"sound", false}, {"completionAction", "none"}}));
    const auto id = automation.enqueue({{"url", server.url("eicar-test.txt")}}); QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(scanState(automation,id), QString("failed"), 10000);
    QVERIFY(automation.error().contains("Antivirus", Qt::CaseInsensitive)); QVERIFY(automation.error().contains("FOUND"));
    QCOMPARE(read(job(manager,id).value("path").toString()), SystemHttpFixture::eicar()); QVERIFY(automation.completionNotice().isEmpty());
  }
  void defaultOfficialDefinitionsScanTheActualDownload() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager,temp.path() + "/state");
    QVERIFY(automation.configure({{"directory",temp.path() + "/out"},{"antivirus",true},{"notifications",false},{"sound",false},{"completionAction","none"}}));
    const auto id=automation.enqueue({{"url",server.url("eicar-official.txt")}}); QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(scanState(automation,id),QString("failed"),45000);
    QVERIFY2(automation.error().contains("EICAR",Qt::CaseInsensitive),qPrintable(automation.error())); QVERIFY(automation.error().contains("FOUND"));
    QCOMPARE(read(job(manager,id).value("path").toString()),SystemHttpFixture::eicar());
  }
  void realMixedScanResultsBlockEveryCompletionAction() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state"); QList<QStringList> sideEffects; QList<int> results;
    Automation automation(&manager, temp.path() + "/state", nullptr, [&](const QStringList &command, auto done) {
      if (command.first() == scanner) scanReal(command, done, results);
      else { sideEffects.append(command); done(0, {}); }
    });
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"}, {"antivirus", true}, {"scannerCommand", scanCommand(temp.path())}, {"notifications", false}, {"sound", false}, {"completionAction", "shutdown"}, {"completionDelaySeconds", 5}}));
    const auto bad = automation.enqueue({{"url", server.url("eicar-test.txt")}}); const auto good = automation.enqueue({{"url", server.url("clean.txt")}});
    QVERIFY(!bad.isEmpty() && !good.isEmpty()); QTRY_COMPARE_WITH_TIMEOUT(scanState(automation,bad), QString("failed"), 10000); QTRY_COMPARE_WITH_TIMEOUT(scanState(automation,good), QString("clean"), 10000);
    QVERIFY(results.contains(0)); QVERIFY(results.contains(1)); QVERIFY(automation.completionNotice().isEmpty()); QTest::qWait(5200); QVERIFY(sideEffects.isEmpty());
    QCOMPARE(read(job(manager,bad).value("path").toString()), SystemHttpFixture::eicar());
  }
  void missingScannerUsesTheRealExecutorErrorPath() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state"); Automation automation(&manager,temp.path() + "/state");
    QVERIFY(automation.configure({{"directory",temp.path() + "/out"},{"antivirus",true},{"scannerCommand",QStringList{"idm-missing-scanner-fixture","%f"}},{"notifications",false},{"sound",false},{"completionAction","none"}}));
    const auto id=automation.enqueue({{"url",server.url("clean.txt")}}); QVERIFY(!id.isEmpty()); QTRY_COMPARE_WITH_TIMEOUT(scanState(automation,id),QString("failed"),5000); QVERIFY(automation.error().contains("missing",Qt::CaseInsensitive));
  }
  void isolatedSystemDispatch_data() {
    QTest::addColumn<QString>("kind"); QTest::addColumn<QStringList>("expected");
    QTest::newRow("notification") << QString("notification") << QStringList{"notify-send","--app-name=IDM Native","Download complete","clean.txt"};
    QTest::newRow("sound") << QString("sound") << QStringList{"canberra-gtk-play","--id","complete"};
    QTest::newRow("shutdown") << QString("shutdown") << QStringList{"systemctl","poweroff"};
    QTest::newRow("disconnect") << QString("disconnect") << QStringList{"nmcli","networking","off"};
  }
  void isolatedSystemDispatch() {
    QFETCH(QString,kind); QFETCH(QStringList,expected); SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state"); QList<QStringList> commands;
    Automation automation(&manager,temp.path() + "/state",nullptr,[&](const QStringList &command,auto done){commands.append(command); done(1,"Isolated executor denied this test action");});
    const bool power=kind=="shutdown" || kind=="disconnect";
    QVERIFY(automation.configure({{"directory",temp.path() + "/out"},{"notifications",kind=="notification"},{"sound",kind=="sound"},{"completionAction",power?kind:QString("none")},{"completionDelaySeconds",5}}));
    QVERIFY(!automation.enqueue({{"url",server.url("clean.txt")}}).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(commands.size(),1,8000); QCOMPARE(commands.first(),expected); QVERIFY(automation.error().contains("failed",Qt::CaseInsensitive)); QVERIFY(automation.error().contains("denied"));
  }
  void cancelPreventsInjectedDisconnectDispatch() {
    SystemHttpFixture server; QTemporaryDir temp; DownloadManager manager(temp.path() + "/state"); QList<QStringList> commands;
    Automation automation(&manager,temp.path() + "/state",nullptr,[&](const QStringList &command,auto done){commands.append(command); done(0,{});});
    QVERIFY(automation.configure({{"directory",temp.path() + "/out"},{"notifications",false},{"sound",false},{"completionAction","disconnect"},{"completionDelaySeconds",5}}));
    QVERIFY(!automation.enqueue({{"url",server.url("clean.txt")}}).isEmpty()); QTRY_VERIFY_WITH_TIMEOUT(!automation.completionNotice().isEmpty(),5000); automation.cancelCompletionAction(); QTest::qWait(5200); QVERIFY(commands.isEmpty());
  }
};
QTEST_MAIN(SystemIntegrationTests)
#include "system_integration_tests.moc"
