#include "browserbridge.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <csignal>
#include <sys/stat.h>

class BridgeTests : public QObject {
  Q_OBJECT
  QByteArray frame(const QByteArray &json) {
    QByteArray result(4, '\0');
    qToLittleEndian<quint32>(quint32(json.size()), result.data());
    return result + json;
  }
  QVariantMap response(QProcess &process) {
    QByteArray bytes;
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 10000) {
      QCoreApplication::processEvents();
      bytes += process.readAllStandardOutput();
      if (bytes.size() >= 4) {
        auto length = qFromLittleEndian<quint32>(bytes.constData());
        if (bytes.size() >= length + 4)
          return QJsonDocument::fromJson(bytes.mid(4, length))
              .object()
              .toVariantMap();
      }
      QTest::qWait(5);
    }
    return {};
  }
  QString binary() const {
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath("../idm-native");
  }
private slots:
  void framedHostUsesPrivateIpc() {
    QTemporaryDir temporary;
    QVariantMap accepted;
    BrowserBridge bridge(
        temporary.path(),
        [&](const QVariantMap &request) {
          accepted = request;
          return QVariantMap{{"ok", true}, {"id", "accepted-job"}};
        },
        true);
    QVERIFY2(bridge.available(), qPrintable(bridge.error()));
    auto permissions =
        QFileInfo(BrowserBridge::socketPath(temporary.path())).permissions();
    QVERIFY(
        !(permissions & (QFileDevice::ReadOther | QFileDevice::WriteOther |
                         QFileDevice::ReadGroup | QFileDevice::WriteGroup)));
    QProcess host;
    host.start(binary(),
               {"browser-host", "--state-directory", temporary.path()});
    QVERIFY(host.waitForStarted());
    const auto ping = frame("{\"command\":\"ping\"}");
    host.write(ping.left(2));
    QTest::qWait(10);
    host.write(ping.mid(2));
    QCOMPARE(response(host).value("ok").toBool(), true);
    const QVariantMap payload{
        {"command", "download"},
        {"request",
         QVariantMap{{"url", "http://localhost/test.bin"},
                     {"headers", QVariantMap{{"Cookie", "private=test"}}}}}};
    host.write(frame(QJsonDocument(QJsonObject::fromVariantMap(payload))
                         .toJson(QJsonDocument::Compact)));
    QCOMPARE(response(host).value("id").toString(), QString("accepted-job"));
    QCOMPARE(accepted, payload);
    host.closeWriteChannel();
    QVERIFY(host.waitForFinished());
    QCOMPARE(host.exitCode(), 0);
  }
  void idlePingDoesNotLaunchApp() {
    QTemporaryDir temporary;
    QProcess host;
    host.start(binary(), {"browser-host", "--state-directory", temporary.path()});
    QVERIFY(host.waitForStarted());
    host.write(frame("{\"command\":\"ping\"}"));
    const auto result = response(host);
    QCOMPARE(result.value("ok").toBool(), true);
    QCOMPARE(result.value("running").toBool(), false);
    QVERIFY(!QFileInfo::exists(BrowserBridge::socketPath(temporary.path())));
    host.closeWriteChannel();
    QVERIFY(host.waitForFinished());
  }
  void queueOwnerSurvivesSecondStartupAndCrashRecovery() {
    QTemporaryDir temporary;
    QFile settings(temporary.filePath("automation.json"));
    const QByteArray savedSettings="{\"preferences\":{\"notifications\":false}}";
    const auto socket = BrowserBridge::socketPath(temporary.path());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    environment.insert("QT_QUICK_BACKEND", "software");
    QProcess owner;
    owner.setProcessEnvironment(environment);
    owner.start(binary(), {"background", "--state-directory", temporary.path()});
    QVERIFY(owner.waitForStarted());
    QTRY_VERIFY(QFileInfo::exists(socket));
    struct stat original;
    QCOMPARE(::stat(qPrintable(socket), &original), 0);

    // A stalled owner cannot answer handoffs, but it still owns queue.lock.
    QCOMPARE(::kill(pid_t(owner.processId()), SIGSTOP), 0);
    QVERIFY(settings.open(QIODevice::WriteOnly));
    QCOMPARE(settings.write(savedSettings), savedSettings.size());
    settings.close();
    QProcess second;
    second.setProcessEnvironment(environment);
    second.start(binary(), {"background", "--state-directory", temporary.path()});
    QVERIFY(second.waitForStarted());
    QVERIFY(second.waitForFinished(5000));
    QCOMPARE(second.exitCode(), 1);
    struct stat remaining;
    QCOMPARE(::stat(qPrintable(socket), &remaining), 0);
    QCOMPARE(remaining.st_ino, original.st_ino);
    QCOMPARE(remaining.st_dev, original.st_dev);
    QVERIFY(settings.open(QIODevice::ReadOnly));
    QCOMPARE(settings.readAll(), savedSettings);
    settings.close();
    QCOMPARE(::kill(pid_t(owner.processId()), SIGCONT), 0);

    QProcess host;
    host.start(binary(), {"browser-host", "--state-directory", temporary.path()});
    QVERIFY(host.waitForStarted());
    host.write(frame("{\"command\":\"download\",\"request\":{\"url\":\"https://example.com/saved.bin\",\"start\":false}}"));
    const auto accepted = response(host);
    QVERIFY(accepted.value("ok").toBool());
    QVERIFY(!accepted.value("id").toString().isEmpty());
    host.closeWriteChannel();
    QVERIFY(host.waitForFinished());
    owner.kill();
    QVERIFY(owner.waitForFinished());
    QVERIFY(QFileInfo::exists(socket));

    QProcess recovered;
    recovered.setProcessEnvironment(environment);
    recovered.start(binary(), {"background", "--state-directory", temporary.path()});
    QVERIFY(recovered.waitForStarted());
    QProcess probe;
    probe.start(binary(), {"browser-host", "--state-directory", temporary.path()});
    QVERIFY(probe.waitForStarted());
    QVariantMap result;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      probe.write(frame("{\"command\":\"ping\"}"));
      result = response(probe);
      return result.value("running").toBool();
    })(), 5000);
    QVERIFY(result.value("ok").toBool());
    QFile queue(temporary.filePath("queue.json"));
    QVERIFY(queue.open(QIODevice::ReadOnly));
    const auto records = QJsonDocument::fromJson(queue.readAll()).object().value("downloads").toArray();
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().toObject().value("id").toString(), accepted.value("id").toString());
    probe.closeWriteChannel();
    QVERIFY(probe.waitForFinished());
    recovered.terminate();
    QVERIFY(recovered.waitForFinished());
  }
  void malformedAndOversizeFramesFail() {
    QTemporaryDir temporary;
    QProcess host;
    host.start(binary(),
               {"browser-host", "--state-directory", temporary.path()});
    QVERIFY(host.waitForStarted());
    host.write(frame("not-json"));
    auto invalid = response(host);
    QCOMPARE(invalid.value("ok").toBool(), false);
    QVERIFY(invalid.contains("error"));
    QByteArray oversized(4, '\0');
    qToLittleEndian<quint32>(1024 * 1024 + 1, oversized.data());
    host.write(oversized);
    auto result = response(host);
    QVERIFY(result.value("error").toString().contains("limit"));
    QTRY_COMPARE(host.state(), QProcess::NotRunning);
    QCOMPARE(host.exitCode(), 1);
  }
};
QTEST_GUILESS_MAIN(BridgeTests)
#include "browserbridge_tests.moc"
