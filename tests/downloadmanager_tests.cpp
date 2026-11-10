#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <sys/stat.h>

class Server : public QTcpServer {
public:
    QByteArray body;
    QString mode="range";
    QByteArray etag="\"v1\"";
    int requests=0, rangeRequests=0;
    QList<QByteArray> ranges;
    bool slow=false;
    Server() {
        body.resize(512*1024);
        for(int i=0;i<body.size();++i) body[i]=char((i*37+i/251)%256);
        connect(this,&QTcpServer::newConnection,this,[this] {
            while(hasPendingConnections()) {
                auto socket=nextPendingConnection(); auto input=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,input] {
                    *input+=socket->readAll(); if(!input->contains("\r\n\r\n")) return;
                    socket->disconnect(this); ++requests;
                    QByteArray range;
                    for(auto line:input->split('\n')) if(line.toLower().startsWith("range:")) range=line.mid(6).trimmed();
                    if(!range.isEmpty()) ranges.append(range);
                    if(mode=="error") { socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nConnection: close\r\n\r\nno!"); socket->disconnectFromHost(); return; }
                    if(mode=="redirect" && input->startsWith("GET /redirect")) { socket->write("HTTP/1.1 302 Found\r\nLocation: /file.bin\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost(); return; }
                    qint64 first=0,last=body.size()-1; bool partial=false;
                    auto match=QRegularExpression("^bytes=([0-9]+)-([0-9]+)$").match(QString::fromLatin1(range));
                    if(mode!="serial" && mode!="unknown" && match.hasMatch()) { first=match.captured(1).toLongLong();last=match.captured(2).toLongLong();partial=true; ++rangeRequests; }
                    if(first>=body.size() || last>=body.size() || first>last) { socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                    const auto bytes=body.mid(first,last-first+1);
                    QByteArray header=partial?"HTTP/1.1 206 Partial Content\r\n":"HTTP/1.1 200 OK\r\n";
                    if(mode!="unknown") header+="Content-Length: "+QByteArray::number(bytes.size())+"\r\n";
                    if(!etag.isEmpty())header+="ETag: "+etag+"\r\n";
                    if(partial) header+="Content-Range: bytes "+QByteArray::number(mode=="wrong" && last>0?first+1:first)+"-"+QByteArray::number(last)+"/"+QByteArray::number(body.size())+"\r\n";
                    header+="Connection: close\r\n\r\n"; socket->write(header);
                    if(slow && bytes.size()>1) {
                        auto timer=new QTimer(socket);auto offset=std::make_shared<int>(0);
                        connect(timer,&QTimer::timeout,socket,[socket,timer,offset,bytes] {
                            if(socket->state()!=QAbstractSocket::ConnectedState){timer->stop();return;}
                            socket->write(bytes.mid(*offset,2048)); *offset+=2048;
                            if(*offset>=bytes.size()){timer->stop();socket->disconnectFromHost();}
                        }); timer->start(5);
                    } else { socket->write(bytes); socket->disconnectFromHost(); }
                });
            }
        });
        if(!listen(QHostAddress::LocalHost)) qFatal("Cannot listen");
    }
    QString url(QString path="file.bin") const {return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(path);}
};

class DownloadTests : public QObject {
    Q_OBJECT
    QVariantMap row(DownloadManager &manager){return manager.downloads().isEmpty()?QVariantMap():manager.downloads()[0].toMap();}
    QByteArray contents(const QString &path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
private slots:
    void rangesProduceExactFile() {
        Server s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        QVERIFY(!m.add(s.url(),dir.path()+"/out",4).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);
        QCOMPARE(contents(row(m)["path"].toString()),s.body); QCOMPARE(s.rangeRequests,5);
        QCOMPARE(row(m)["connections"].toInt(),4);
        m.remove(row(m)["id"].toString()); QVERIFY(QFile::exists(dir.path()+"/out/file.bin"));
    }
    void serialWithoutRanges_data(){QTest::addColumn<QString>("mode");QTest::newRow("length")<<QString("serial");QTest::newRow("unknown length")<<QString("unknown");}
    void serialWithoutRanges() {
        QFETCH(QString,mode);Server s;s.mode=mode;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);
        QCOMPARE(contents(row(m)["path"].toString()),s.body);QCOMPARE(row(m)["connections"].toInt(),1);
    }
    void rangesWithoutValidatorUseSerial() {
        Server s;s.etag.clear();QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);
        QCOMPARE(contents(row(m)["path"].toString()),s.body);QCOMPARE(s.requests,2);
    }
    void pauseAndResumeAfterRestart() {
        Server s;s.slow=true;QTemporaryDir dir;QString id,path;qint64 before=0;
        {
            DownloadManager m(dir.path()+"/state");id=m.add(s.url(),dir.path()+"/out");
            QTRY_VERIFY_WITH_TIMEOUT(row(m)["received"].toLongLong()>16384,10000);
            m.pause(id);QCOMPARE(row(m)["status"].toString(),QString("paused"));before=row(m)["received"].toLongLong();QVERIFY(before<s.body.size());path=row(m)["path"].toString();
        }
        {
            DownloadManager m(dir.path()+"/state");QCOMPARE(row(m)["status"].toString(),QString("paused"));QCOMPARE(row(m)["received"].toLongLong(),before);
            m.resume(id);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(path),s.body);
        }
        for(int i=0;i<3;++i) { DownloadManager m(dir.path()+"/state");QVERIFY(m.error().isEmpty());QCOMPARE(row(m)["status"].toString(),QString("complete"));QCOMPARE(row(m)["received"].toLongLong(),qint64(s.body.size())); }
    }
    void emptyFileCompletes() {
        Server s;s.body.clear();QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE(row(m)["status"].toString(),QString("complete"));QVERIFY(QFile::exists(row(m)["path"].toString()));QCOMPARE(row(m)["total"].toLongLong(),0);
    }
    void immediatePauseCanResumeZeroByteParts() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE(row(m)["status"].toString(),QString("downloading"));m.pause(id);m.resume(id);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void zeroBytePauseRebuildsRangesForChangedRemoteSize() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");QString id;bool paused=false;
        connect(&m,&DownloadManager::changed,this,[&] {
            if(paused || row(m)["status"].toString()!="downloading") return;
            paused=true;m.pause(id);QCOMPARE(row(m)["received"].toLongLong(),qint64(0));
            s.body+=s.body;s.etag="\"larger\"";
            QTimer::singleShot(0,&m,[&]{m.resume(id);});
        });
        id=m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);
        QVERIFY(paused);QCOMPARE(row(m)["total"].toLongLong(),qint64(s.body.size()));QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void changedValidatorPreservesPartials() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.add(s.url(),dir.path()+"/out");
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["received"].toLongLong()>8192,10000);m.pause(id);const auto before=row(m)["received"].toLongLong();s.etag="\"changed\"";m.resume(id);
        QTRY_COMPARE(row(m)["status"].toString(),QString("error"));QCOMPARE(row(m)["received"].toLongLong(),before);QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void invalidRangeNeverPublishes() {
        Server s;s.mode="wrong";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE(row(m)["status"].toString(),QString("error"));QVERIFY(!QFile::exists(row(m)["path"].toString()));QCOMPARE(row(m)["received"].toLongLong(),0);
    }
    void httpErrorNeverPublishes() {
        Server s;s.mode="error";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_COMPARE(row(m)["status"].toString(),QString("error"));QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void existingDestinationSurvives() {
        Server s;QTemporaryDir dir;QDir().mkpath(dir.path()+"/out");QFile original(dir.path()+"/out/file.bin");QVERIFY(original.open(QIODevice::WriteOnly));original.write("original");original.close();
        DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");QTRY_COMPARE(row(m)["status"].toString(),QString("complete"));
        QCOMPARE(contents(original.fileName()),QByteArray("original"));QCOMPARE(contents(row(m)["path"].toString()),s.body);QVERIFY(row(m)["path"].toString().endsWith(".1"));
    }
    void destinationCollisionAtCompletionSurvives() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out");
        QTRY_VERIFY(row(m)["received"].toLongLong()>8192);QFile f(row(m)["path"].toString());QVERIFY(f.open(QIODevice::WriteOnly));f.write("other file");f.close();
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),10000);QCOMPARE(contents(f.fileName()),QByteArray("other file"));
    }
    void staleOwnedStagingCanBeRecovered() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");const auto id=m.add(s.url(),dir.path()+"/out");
        QTRY_VERIFY(row(m)["received"].toLongLong()>8192);
        QFile stale(row(m)["path"].toString()+".idm-native-"+id+".tmp");QVERIFY(stale.open(QIODevice::WriteOnly));stale.write("interrupted assembly");stale.close();
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);QVERIFY(!stale.exists());
    }
    void removalRejectsReplacedPartialDirectory() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.add(s.url(),dir.path()+"/out");
        QTRY_VERIFY(row(m)["received"].toLongLong()>8192);m.pause(id);
        const QString original=dir.path()+"/state/"+id, saved=dir.path()+"/saved";
        QVERIFY(QDir().rename(original,saved));QVERIFY(QFile::link(saved,original));
        m.remove(id);QCOMPARE(m.downloads().size(),1);QVERIFY(QFile::exists(saved+"/0.part"));QCOMPARE(row(m)["status"].toString(),QString("error"));
    }
    void validatesUrlsAndFilename() {
        Server s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        QVERIFY(m.add("file:///etc/passwd",dir.path()).isEmpty());QVERIFY(m.add("https://a:b@example.com/file",dir.path()).isEmpty());
        m.add(s.url("a%5Cb%0Ac.bin"),dir.path()+"/out");QTRY_COMPARE(row(m)["status"].toString(),QString("complete"));
        QVERIFY(!row(m)["name"].toString().contains('\\'));QVERIFY(!row(m)["name"].toString().contains('\n'));
    }
    void redirectsRemainValid() {
        Server s;s.mode="redirect";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url("redirect"),dir.path()+"/out");QTRY_COMPARE(row(m)["status"].toString(),QString("complete"));QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void queueAndDownloadFilesArePrivate() {
        Server s;s.slow=true;QTemporaryDir dir;const auto state=dir.path()+"/state";
        QVERIFY(QDir().mkpath(state));QVERIFY(QFile::setPermissions(state,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther));
        DownloadManager m(state);const auto id=m.add(s.url("file.bin?token=private"),dir.path()+"/out");
        QTRY_VERIFY(row(m)["received"].toLongLong()>8192);
        auto mode=[](const QString &path) { struct stat info{};return ::stat(QFile::encodeName(path).constData(),&info)==0 ? int(info.st_mode&0777) : -1; };
        QCOMPARE(mode(state),0755);QCOMPARE(mode(state+"/queue.json"),0600);QCOMPARE(mode(state+"/"+id+"/0.part"),0600);
        m.pause(id);QCOMPARE(mode(state+"/queue.json"),0600);m.resume(id);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);
        QCOMPARE(mode(row(m)["path"].toString()),0600);QCOMPARE(mode(state+"/queue.json"),0600);QCOMPARE(mode(state),0755);
    }
    void queueLockAndInvalidQueue() {
        QTemporaryDir dir;{
            DownloadManager m(dir.path());DownloadManager second(dir.path());QVERIFY(!second.error().isEmpty());QVERIFY(second.add("https://example.com/a",dir.path()).isEmpty());
        }
        QFile file(dir.path()+"/queue.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write("invalid queue");file.close();
        {DownloadManager m(dir.path());QVERIFY(!m.error().isEmpty());QVERIFY(m.add("https://example.com/a",dir.path()).isEmpty());}
        QCOMPARE(contents(file.fileName()),QByteArray("invalid queue"));
    }
};
QTEST_GUILESS_MAIN(DownloadTests)
#include "downloadmanager_tests.moc"
