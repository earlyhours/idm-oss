#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

class TLSFixture : public QTcpServer {
public:
  QSslCertificate certificate;
  QSslKey key;
  QByteArray body;
  QByteArray request;
  int connections = 0;
  int httpRequests = 0;
  TLSFixture(const QSslCertificate &cert, const QSslKey &privateKey)
      : certificate(cert), key(privateKey) {
    body.resize(64 * 1024);
    for (int index = 0; index < body.size(); ++index)
      body[index] = char(index % 251);
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot start isolated HTTPS fixture");
  }
  QString url() const {
    return QString("https://127.0.0.1:%1/secure.bin").arg(serverPort());
  }

protected:
  void incomingConnection(qintptr descriptor) override {
    ++connections;
    auto *socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(descriptor))
      qFatal("Cannot accept HTTPS fixture connection");
    socket->setLocalCertificate(certificate);
    socket->setPrivateKey(key);
    socket->setPeerVerifyMode(QSslSocket::VerifyNone);
    auto input = std::make_shared<QByteArray>();
    connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QSslSocket::readyRead, this, [this, socket, input] {
      *input += socket->readAll();
      if (!input->contains("\r\n\r\n"))
        return;
      socket->disconnect(this);
      request = *input;
      ++httpRequests;
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                    "application/octet-stream\r\nContent-Length: " +
                    QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
      socket->disconnectFromHost();
    });
    socket->startServerEncryption();
  }
};

class TLSDefaults {
  QSslConfiguration original = QSslConfiguration::defaultConfiguration();

public:
  explicit TLSDefaults(const QSslCertificate &ca = {}) {
    if (!ca.isNull()) {
      auto configuration = original;
      configuration.addCaCertificate(ca);
      QSslConfiguration::setDefaultConfiguration(configuration);
    }
  }
  ~TLSDefaults() { QSslConfiguration::setDefaultConfiguration(original); }
};

class TLSTests : public QObject {
  Q_OBJECT
  QTemporaryDir certificates;
  QSslCertificate ca;
  QSslCertificate serverCertificate;
  QSslKey serverKey;
  bool openssl(const QStringList &arguments) {
    QProcess process;
    process.start("/usr/bin/openssl", arguments);
    if (!process.waitForFinished(15000)) {
      process.kill();
      process.waitForFinished();
      return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
  }
  QByteArray bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    return file.readAll();
  }
  QVariantMap job(const DownloadManager &manager) {
    return manager.downloads().first().toMap();
  }
private slots:
  void initTestCase() {
    QVERIFY2(QSslSocket::supportsSsl(),
             "Qt TLS backend is required for the actual HTTPS tests");
    QVERIFY(certificates.isValid());
    const auto path = certificates.path();
    QVERIFY(openssl({"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                     path + "/ca.key", "-out", path + "/ca.pem", "-subj",
                     "/CN=IDM Native Isolated Test CA", "-days", "1", "-addext",
                     "basicConstraints=critical,CA:TRUE", "-addext",
                     "keyUsage=critical,keyCertSign,cRLSign"}));
    QVERIFY(openssl({"req", "-newkey", "rsa:2048", "-nodes", "-keyout",
                     path + "/server.key", "-out", path + "/server.csr",
                     "-subj", "/CN=127.0.0.1"}));
    QFile extension(path + "/server.ext");
    QVERIFY(extension.open(QIODevice::WriteOnly));
    extension.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,"
                    "digitalSignature,keyEncipherment\nextendedKeyUsage="
                    "serverAuth\nsubjectAltName=IP:127.0.0.1\n");
    extension.close();
    QVERIFY(openssl({"x509", "-req", "-in", path + "/server.csr", "-CA",
                     path + "/ca.pem", "-CAkey", path + "/ca.key",
                     "-CAcreateserial", "-out", path + "/server.pem", "-days",
                     "1", "-extfile", path + "/server.ext"}));
    ca = QSslCertificate(bytes(path + "/ca.pem"));
    serverCertificate = QSslCertificate(bytes(path + "/server.pem"));
    serverKey = QSslKey(bytes(path + "/server.key"), QSsl::Rsa);
    QVERIFY(!ca.isNull());
    QVERIFY(!serverCertificate.isNull());
    QVERIFY(!serverKey.isNull());
  }
  void trustedTemporaryCAAllowsARealEncryptedDownload() {
    TLSDefaults defaults(ca);
    TLSFixture server(serverCertificate, serverKey);
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    const auto id = manager.add(server.url(), temp.path() + "/downloads");
    QVERIFY2(!id.isEmpty(), qPrintable(manager.error()));
    QTRY_COMPARE_WITH_TIMEOUT(job(manager)["status"].toString(),
                              QString("complete"), 10000);
    QCOMPARE(server.httpRequests, 1);
    QVERIFY(server.connections >= 1);
    QVERIFY(server.request.startsWith("GET /secure.bin HTTP/1.1\r\n"));
    const auto output = bytes(job(manager)["path"].toString());
    QCOMPARE(output, server.body);
    QCOMPARE(QCryptographicHash::hash(output, QCryptographicHash::Sha256),
             QCryptographicHash::hash(server.body, QCryptographicHash::Sha256));
    QCOMPARE(job(manager)["received"].toLongLong(), qint64(server.body.size()));
  }
  void untrustedCertificateRefusesTheTransferAndNeverPublishesAFile() {
    TLSDefaults defaults;
    TLSFixture server(serverCertificate, serverKey);
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    const auto id = manager.add(server.url(), temp.path() + "/downloads");
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(job(manager)["status"].toString(),
                              QString("error"), 10000);
    QVERIFY(server.connections >= 1);
    QCOMPARE(server.httpRequests, 0);
    QVERIFY(!job(manager)["error"].toString().isEmpty());
    QCOMPARE(job(manager)["received"].toLongLong(), qint64(0));
    QVERIFY(!QFileInfo::exists(job(manager)["path"].toString()));
    QVERIFY(QDir(temp.path() + "/downloads").entryList(QDir::Files).isEmpty());
  }
  void trustedIssuerStillRejectsTheWrongHostname() {
    TLSDefaults defaults(ca);
    TLSFixture server(serverCertificate, serverKey);
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    auto url = server.url();
    url.replace("127.0.0.1", "localhost");
    const auto id = manager.add(url, temp.path() + "/downloads");
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(job(manager)["status"].toString(),
                              QString("error"), 10000);
    QVERIFY(server.connections >= 1);
    QCOMPARE(server.httpRequests, 0);
    QVERIFY(!QFileInfo::exists(job(manager)["path"].toString()));
  }
};
QTEST_GUILESS_MAIN(TLSTests)
#include "tls_tests.moc"
