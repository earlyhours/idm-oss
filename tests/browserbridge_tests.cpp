#include "browserbridge.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

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
