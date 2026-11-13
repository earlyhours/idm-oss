#include "automation.h"
#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <memory>

class SnapshotServer : public QTcpServer {
public:
  int bodySize = 256 * 1024;
  int chunkSize = 1024;
  int interval = 100;
  bool unknownLength = false;
  int requests = 0;
  int activeTCP = 0;
  int peakTCP = 0;
  int activeBulk = 0;
  int peakBulk = 0;
  int peakJobs = 0;
  qint64 bodyBytesWritten = 0;
  QHash<QString, int> bulkByPath;
  QHash<QString, int> peakByPath;
  QList<QByteArray> ranges;

  SnapshotServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        ++activeTCP;
        peakTCP = qMax(peakTCP, activeTCP);
        struct Connection {
          QByteArray input;
          bool handled = false;
          bool bulk = false;
          QString path;
        };
        auto connection = std::make_shared<Connection>();
        connect(socket, &QTcpSocket::disconnected, this, [this, connection] {
          --activeTCP;
          if (connection->bulk) {
            --activeBulk;
            auto found = bulkByPath.find(connection->path);
            if (found != bulkByPath.end() && --found.value() == 0)
              bulkByPath.erase(found);
          }
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(
            socket, &QTcpSocket::readyRead, this, [this, socket, connection] {
              connection->input += socket->readAll();
              if (connection->handled ||
                  !connection->input.contains("\r\n\r\n"))
                return;
              connection->handled = true;
              ++requests;
              connection->path =
                  QString::fromLatin1(connection->input.split(' ').value(1));
              const auto reference = payload(connection->path);
              qint64 first = 0, last = reference.size() - 1;
              bool partial = false;
              QByteArray range;
              for (auto line : connection->input.split('\n'))
                if (line.toLower().startsWith("range:"))
                  range = line.mid(6).trimmed();
              if (!range.isEmpty())
                ranges.append(range);
              const auto match = QRegularExpression("^bytes=([0-9]+)-([0-9]+)$")
                                     .match(QString::fromLatin1(range));
              if (!unknownLength && match.hasMatch()) {
                first = match.captured(1).toLongLong();
                last = match.captured(2).toLongLong();
                partial = true;
              }
              if (first < 0 || last < first || last >= reference.size()) {
                socket->write("HTTP/1.1 416 Range Not Satisfiable\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
              }
              const auto bytes = reference.mid(first, last - first + 1);
              QByteArray headers = partial ? "HTTP/1.1 206 Partial Content\r\n"
                                           : "HTTP/1.1 200 OK\r\n";
              if (!unknownLength)
                headers +=
                    "Content-Length: " + QByteArray::number(bytes.size()) +
                    "\r\n";
              headers += "ETag: \"snapshot-" +
                         QCryptographicHash::hash(reference,
                                                  QCryptographicHash::Sha256)
                             .toHex() +
                         "\"\r\nAccept-Ranges: bytes\r\n";
              if (partial)
                headers += "Content-Range: bytes " + QByteArray::number(first) +
                           "-" + QByteArray::number(last) + "/" +
                           QByteArray::number(reference.size()) + "\r\n";
              headers += "Connection: close\r\n\r\n";
              socket->write(headers);
              if (bytes.size() == 1) {
                socket->write(bytes);
                socket->flush();
                socket->disconnectFromHost();
                return;
              }
              connection->bulk = true;
              ++activeBulk;
              peakBulk = qMax(peakBulk, activeBulk);
              const int active = ++bulkByPath[connection->path];
              peakByPath[connection->path] =
                  qMax(peakByPath.value(connection->path), active);
              peakJobs = qMax(peakJobs, int(bulkByPath.size()));
              auto offset = std::make_shared<qsizetype>(0);
              auto *timer = new QTimer(socket);
              connect(timer, &QTimer::timeout, socket,
                      [this, socket, timer, offset, bytes] {
                        if (socket->state() !=
                            QAbstractSocket::ConnectedState) {
                          timer->stop();
                          return;
                        }
                        const auto block = bytes.mid(*offset, chunkSize);
                        const auto sent = socket->write(block);
                        if (sent > 0)
                          bodyBytesWritten += sent;
                        *offset += block.size();
                        if (*offset >= bytes.size()) {
                          timer->stop();
                          socket->disconnectFromHost();
                        }
                      });
              timer->start(interval);
            });
      }
    });
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot listen for isolated snapshot fixture");
  }
  QByteArray payload(const QString &path) const {
    const auto seed = quint8(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256)
            .at(0));
    QByteArray bytes(bodySize, '\0');
    for (int index = 0; index < bytes.size(); ++index)
      bytes[index] = char((index * 17 + seed) % 251);
    return bytes;
  }
  QString url(const QString &name = "archive.zip") const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(name);
  }
};

class FeatureSnapshotTests : public QObject {
  Q_OBJECT
  static QVariantMap row(const DownloadManager &manager, int index = 0) {
    return manager.downloads().value(index).toMap();
  }
  static QByteArray contents(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    return file.readAll();
  }
  static bool partitionMatches(const QVariantMap &job) {
    const auto parts = job.value("parts").toList();
    if (parts.isEmpty())
      return false;
    qint64 received = 0, total = 0;
    for (const auto &value : parts) {
      auto part = value.toMap();
      const qint64 bytes = part.value("received").toLongLong();
      const qint64 size = part.value("total").toLongLong();
      if (bytes < 0 || size < bytes)
        return false;
      received += bytes;
      total += size;
    }
    return received == job.value("received").toLongLong() &&
           total == job.value("total").toLongLong();
  }
private slots:
  void progressSpeedEtaPartsPauseAndRestartMatchRealTraffic() {
    SnapshotServer server;
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    const auto destination = temp.path() + "/downloads";
    QString id, created;
    qint64 pausedBytes = 0;
    {
      DownloadManager manager(state);
      id = manager.add(server.url(), destination, 4);
      QVERIFY2(!id.isEmpty(), qPrintable(manager.error()));
      QTRY_VERIFY_WITH_TIMEOUT(
          row(manager)["received"].toLongLong() >= server.bodySize / 4, 5000);
      const auto job = row(manager);
      QCOMPARE(job["status"].toString(), QString("downloading"));
      QVERIFY(job["received"].toLongLong() < server.bodySize);
      QCOMPARE(job["total"].toLongLong(), qint64(server.bodySize));
      QVERIFY(partitionMatches(job));
      QCOMPARE(job["parts"].toList().size(), 4);
      QVERIFY(std::isfinite(job["speed"].toDouble()));
      QVERIFY(job["speed"].toDouble() > 0);
      QVERIFY(job["eta"].toLongLong() > 0);
      QVERIFY(job["received"].toLongLong() <= server.bodyBytesWritten);
      QVERIFY(server.peakBulk <= 4);
      QVERIFY(server.peakBulk >= 2);
      QVERIFY(server.peakTCP <= 4);
      created = job["created"].toString();
      manager.pause(id);
      const auto stopped = row(manager);
      QCOMPARE(stopped["status"].toString(), QString("paused"));
      pausedBytes = stopped["received"].toLongLong();
      QCOMPARE(stopped["speed"].toDouble(), 0.0);
      QCOMPARE(stopped["eta"].toLongLong(), qint64(-1));
      QTest::qWait(350);
      QCOMPARE(row(manager)["received"].toLongLong(), pausedBytes);
      QVERIFY(partitionMatches(row(manager)));
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.activeTCP, 0, 1000);
    const auto oldRangeCount = server.ranges.size();
    DownloadManager restored(state);
    QCOMPARE(restored.downloads().size(), 1);
    QCOMPARE(row(restored)["id"].toString(), id);
    QCOMPARE(row(restored)["created"].toString(), created);
    QCOMPARE(row(restored)["received"].toLongLong(), pausedBytes);
    QCOMPARE(row(restored)["status"].toString(), QString("paused"));
    restored.resume(id);
    QElapsedTimer clock;
    clock.start();
    qint64 previous = pausedBytes;
    bool sampled = false;
    qint64 sampledAt = 0, sampledBytes = 0, sampledEta = -1;
    double sampledSpeed = 0;
    while (clock.elapsed() < 12000 && row(restored)["status"] != "complete" &&
           row(restored)["status"] != "error") {
      QTest::qWait(25);
      const auto job = row(restored);
      const qint64 received = job["received"].toLongLong();
      QVERIFY2(received >= previous, "Reported progress moved backwards");
      QVERIFY(received <= server.bodyBytesWritten);
      QVERIFY(received <= server.bodySize);
      QVERIFY(partitionMatches(job));
      previous = received;
      if (!sampled && job["status"] == "downloading" &&
          received >= pausedBytes + 16 * 1024 &&
          received < server.bodySize / 2) {
        sampled = true;
        sampledAt = clock.elapsed();
        sampledBytes = received;
        sampledSpeed = job["speed"].toDouble();
        sampledEta = job["eta"].toLongLong();
        QVERIFY(std::isfinite(sampledSpeed));
        QVERIFY(sampledSpeed > 0);
        QVERIFY(sampledEta > 0);
      }
    }
    QCOMPARE(row(restored)["status"].toString(), QString("complete"));
    QVERIFY2(sampled, "No actual in-flight speed/ETA sample was observed");
    const double remainingSeconds = (clock.elapsed() - sampledAt) / 1000.0;
    QVERIFY(remainingSeconds > 2);
    const double futureRate =
        (server.bodySize - sampledBytes) / remainingSeconds;
    QVERIFY2(sampledSpeed > futureRate / 4 && sampledSpeed < futureRate * 4,
             "Displayed speed disagrees with subsequent measured delivery");
    QVERIFY2(sampledEta >= remainingSeconds / 3 &&
                 sampledEta <= remainingSeconds * 3 + 1,
             "Displayed ETA disagrees with measured time to completion");
    const auto complete = row(restored);
    QCOMPARE(complete["received"].toLongLong(), qint64(server.bodySize));
    QCOMPARE(complete["total"].toLongLong(), qint64(server.bodySize));
    QCOMPARE(complete["speed"].toDouble(), 0.0);
    QCOMPARE(complete["eta"].toLongLong(), qint64(-1));
    QVERIFY(partitionMatches(complete));
    const auto output = contents(complete["path"].toString());
    const auto expected = server.payload("/archive.zip");
    QCOMPARE(output, expected);
    QCOMPARE(QCryptographicHash::hash(output, QCryptographicHash::Sha256),
             QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    QVERIFY(server.peakBulk <= 4);
    QVERIFY(server.peakTCP <= 4);
    bool resumedOffset = false;
    for (int index = oldRangeCount; index < server.ranges.size(); ++index) {
      const auto match = QRegularExpression("^bytes=([0-9]+)-([0-9]+)$")
                             .match(QString::fromLatin1(server.ranges[index]));
      if (match.hasMatch() && match.captured(1).toLongLong() > 0)
        resumedOffset = true;
    }
    QVERIFY(resumedOffset);
  }
  void unknownLengthHasNoInventedEtaAndResolvesItsFinalSize() {
    SnapshotServer server;
    server.unknownLength = true;
    server.bodySize = 32 * 1024;
    server.interval = 20;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    QVERIFY(!manager.add(server.url("stream.dat"), temp.path() + "/out", 4)
                 .isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(row(manager)["received"].toLongLong() > 0, 2000);
    const auto partial = row(manager);
    QCOMPARE(partial["status"].toString(), QString("downloading"));
    QCOMPARE(partial["total"].toLongLong(), qint64(-1));
    QCOMPARE(partial["eta"].toLongLong(), qint64(-1));
    QCOMPARE(partial["connections"].toInt(), 1);
    QVERIFY(std::isfinite(partial["speed"].toDouble()));
    QVERIFY(partial["speed"].toDouble() > 0);
    QTRY_COMPARE_WITH_TIMEOUT(row(manager)["status"].toString(),
                              QString("complete"), 4000);
    const auto complete = row(manager);
    QCOMPARE(complete["total"].toLongLong(), qint64(server.bodySize));
    QCOMPARE(complete["received"].toLongLong(), qint64(server.bodySize));
    QCOMPARE(complete["eta"].toLongLong(), qint64(-1));
    QVERIFY(partitionMatches(complete));
    QCOMPARE(contents(complete["path"].toString()),
             server.payload("/stream.dat"));
    QCOMPARE(server.peakBulk, 1);
  }
  void queuedFilesHaveActualCategoriesAndIsoCreationDatesWithoutRequests() {
    SnapshotServer server;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    const QList<QPair<QString, QString>> examples = {
        {"bundle.zip", "Compressed"}, {"paper.PDF", "Documents"},
        {"song.flac", "Music"},       {"installer.AppImage", "Programs"},
        {"movie.webm", "Video"},      {"payload.blob", "Other"}};
    const auto before = QDateTime::currentDateTimeUtc();
    for (const auto &example : examples) {
      const auto id = manager.addRequest({{"url", server.url(example.first)},
                                          {"directory", temp.path() + "/out"},
                                          {"start", false}});
      QVERIFY2(!id.isEmpty(), qPrintable(manager.error()));
    }
    const auto after = QDateTime::currentDateTimeUtc();
    QCOMPARE(manager.downloads().size(), examples.size());
    for (int index = 0; index < examples.size(); ++index) {
      const auto job = row(manager, index);
      QCOMPARE(job["name"].toString(), examples[index].first);
      QCOMPARE(job["category"].toString(), examples[index].second);
      QCOMPARE(job["status"].toString(), QString("paused"));
      const auto created =
          QDateTime::fromString(job["created"].toString(), Qt::ISODate);
      QVERIFY(created.isValid());
      QVERIFY(created >= before.addSecs(-1));
      QVERIFY(created <= after.addSecs(1));
      QCOMPARE(job["received"].toLongLong(), qint64(0));
      QCOMPARE(job["speed"].toDouble(), 0.0);
      QCOMPARE(job["eta"].toLongLong(), qint64(-1));
    }
    QTest::qWait(150);
    QCOMPARE(server.requests, 0);
  }
  void twoRealJobsRunTogetherWithoutExceedingJobOrWorkerLimits() {
    SnapshotServer server;
    server.bodySize = 128 * 1024;
    server.chunkSize = 2048;
    server.interval = 25;
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr,
                          [](const QStringList &, auto done) { done(0, {}); });
    QVERIFY(automation.configure({{"directory", temp.path() + "/out"},
                                  {"connections", 2},
                                  {"maxConcurrent", 2},
                                  {"notifications", false}}));
    const QStringList names = {"one.bin", "two.bin", "three.bin", "four.bin"};
    for (const auto &name : names)
      QVERIFY(!automation.enqueue({{"url", server.url(name)}}).isEmpty());
    QElapsedTimer timer;
    timer.start();
    int observedConcurrent = 0;
    bool allComplete = false;
    while (timer.elapsed() < 8000 && !allComplete) {
      QTest::qWait(20);
      int active = 0;
      allComplete = true;
      for (const auto &value : manager.downloads()) {
        const auto job = value.toMap();
        if (job["status"] == "downloading" || job["status"] == "probing")
          ++active;
        if (job["status"] != "complete")
          allComplete = false;
        QVERIFY2(job["status"] != "error", qPrintable(job["error"].toString()));
      }
      observedConcurrent = qMax(observedConcurrent, active);
      QVERIFY(active <= 2);
      QVERIFY(server.peakJobs <= 2);
      QVERIFY(server.peakBulk <= 4);
      for (int workers : server.peakByPath)
        QVERIFY(workers <= 2);
    }
    QVERIFY2(allComplete, "Four real queued transfers did not finish");
    QCOMPARE(observedConcurrent, 2);
    QCOMPARE(server.peakJobs, 2);
    QVERIFY(server.peakBulk >= 2);
    for (int index = 0; index < names.size(); ++index) {
      const auto job = row(manager, index);
      QVERIFY(partitionMatches(job));
      QCOMPARE(contents(job["path"].toString()),
               server.payload("/" + names[index]));
    }
  }
};
QTEST_GUILESS_MAIN(FeatureSnapshotTests)
#include "feature_snapshot_tests.moc"
