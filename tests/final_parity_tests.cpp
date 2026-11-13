#include "automation.h"
#include "downloadmanager.h"
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class BatchHttpFixture : public QTcpServer {
public:
  QList<QByteArray> paths;
  BatchHttpFixture() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          const auto request = socket->property("input").toByteArray() + socket->readAll();
          socket->setProperty("input", request);
          if (!request.contains("\r\n\r\n") || socket->property("answered").toBool()) return;
          socket->setProperty("answered", true);
          const auto path = request.split(' ').value(1);
          paths.append(path);
          const auto body = QByteArray("independent batch payload:") + path + QByteArray(8192, 'b');
          socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
    if (!listen(QHostAddress::LocalHost)) qFatal("Cannot listen for local batch fixture");
  }
  QString url(const QString &file) const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(file);
  }
};

class FinalParityTests : public QObject {
  Q_OBJECT
private slots:
  void actualBatchReportsPartialAcceptanceAndAutomaticallyDownloadsEveryAcceptedFile() {
    BatchHttpFixture server;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DownloadManager manager(temporary.filePath("state"));
    Automation automation(&manager, temporary.filePath("state"), nullptr,
                          [](const QStringList &, auto done) { done(0, {}); });
    QVERIFY(automation.configure({{"directory", temporary.filePath("out")},
                                  {"notifications", false}, {"sound", false},
                                  {"maxConcurrent", 2}}));
    const auto result = automation.batch({QVariantMap{{"url", server.url("one.bin")}},
                                          QVariantMap{{"url", "file:///etc/passwd"}},
                                          QVariantMap{{"url", server.url("two.bin")}}});
    QCOMPARE(result.value("ok").toBool(), true);
    const auto ids = result.value("ids").toList();
    QCOMPARE(ids.size(), 2);
    QVERIFY(!ids.first().toString().isEmpty());
    QVERIFY(ids.first() != ids.last());
    QCOMPARE(result.value("errors").toList().size(), 1);
    QVERIFY(!result.value("errors").toList().first().toString().isEmpty());
    QCOMPARE(manager.downloads().size(), 2);
    // No startQueue/resume call: acceptance must start the real jobs by itself.
    QTRY_VERIFY_WITH_TIMEOUT([&] {
      for (const auto &item : manager.downloads())
        if (item.toMap().value("status") != "complete") return false;
      return true;
    }(), 10000);
    for (const auto &file : {QString("one.bin"), QString("two.bin")}) {
      QFile output(temporary.filePath("out/") + file);
      QVERIFY(output.open(QIODevice::ReadOnly));
      QCOMPARE(output.readAll(), QByteArray("independent batch payload:/") + file.toUtf8() + QByteArray(8192, 'b'));
      QVERIFY(server.paths.contains("/" + file.toUtf8()));
    }
    const auto before = server.paths.size();
    const auto rejected = automation.batch({QVariantMap{{"url", "javascript:alert(1)"}},
                                            QVariantMap{{"url", "file:///etc/passwd"}}});
    QCOMPARE(rejected.value("ok").toBool(), false);
    QCOMPARE(rejected.value("ids").toList().size(), 0);
    QCOMPARE(rejected.value("errors").toList().size(), 2);
    QCOMPARE(manager.downloads().size(), 2);
    QTest::qWait(100);
    QCOMPARE(server.paths.size(), before);
  }
};
QTEST_GUILESS_MAIN(FinalParityTests)
#include "final_parity_tests.moc"
