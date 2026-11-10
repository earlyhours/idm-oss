#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
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
    QString fixtureRoot, redirectTarget="/file.bin";
    QByteArray requiredCookie;
    QList<QVariantMap> observedHeaders;
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
                    QVariantMap receivedHeaders;
                    for (auto line : input->split('\n')) { const auto colon=line.indexOf(':'); if(colon>0) receivedHeaders.insert(QString::fromLatin1(line.left(colon).trimmed().toLower()),QString::fromLatin1(line.mid(colon+1).trimmed())); }
                    observedHeaders.append(receivedHeaders);
                    if(!requiredCookie.isEmpty() && receivedHeaders.value("cookie").toByteArray()!=requiredCookie) { socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                    QByteArray payload=body;QByteArray contentType="application/octet-stream";
                    if(!fixtureRoot.isEmpty()) {
                        const auto target=input->split(' ').value(1);const auto path=QUrl("http://fixture"+QString::fromLatin1(target)).path();
                        QFile file(fixtureRoot+path);
                        if(!QFileInfo(file).canonicalFilePath().startsWith(QFileInfo(fixtureRoot).canonicalFilePath()+"/") || !file.open(QIODevice::ReadOnly)) { socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                        payload=file.readAll();
                        if(path.endsWith(".m3u8")) contentType="application/vnd.apple.mpegurl";
                        else if(path.endsWith(".mpd")) contentType="application/dash+xml";
                        else if(path.endsWith(".mp4")) contentType="video/mp4";
                        else if(path.endsWith(".ts")) contentType="video/mp2t";
                    }
                    QByteArray range;
                    for(auto line:input->split('\n')) if(line.toLower().startsWith("range:")) range=line.mid(6).trimmed();
                    if(!range.isEmpty()) ranges.append(range);
                    if(mode=="error") { socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nConnection: close\r\n\r\nno!"); socket->disconnectFromHost(); return; }
                    if(mode=="redirect" && input->startsWith("GET /redirect")) { socket->write("HTTP/1.1 302 Found\r\nLocation: "+redirectTarget.toUtf8()+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost(); return; }
                    qint64 first=0,last=payload.size()-1; bool partial=false;
                    auto match=QRegularExpression("^bytes=([0-9]+)-([0-9]+)$").match(QString::fromLatin1(range));
                    if(mode!="serial" && mode!="unknown" && match.hasMatch()) { first=match.captured(1).toLongLong();last=match.captured(2).toLongLong();partial=true; ++rangeRequests; }
                    if(first>=payload.size() || last>=payload.size() || first>last) { socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                    const auto bytes=payload.mid(first,last-first+1);
                    QByteArray header=partial?"HTTP/1.1 206 Partial Content\r\n":"HTTP/1.1 200 OK\r\n";
                    if(mode!="unknown") header+="Content-Length: "+QByteArray::number(bytes.size())+"\r\n";
                    if(!etag.isEmpty())header+="ETag: "+etag+"\r\n";
                    if(partial) header+="Content-Range: bytes "+QByteArray::number(mode=="wrong" && last>0?first+1:first)+"-"+QByteArray::number(last)+"/"+QByteArray::number(payload.size())+"\r\n";
                    header+="Content-Type: "+contentType+"\r\nConnection: close\r\n\r\n"; socket->write(header);
                    if(input->startsWith("HEAD ")) { socket->disconnectFromHost();return; }
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
    QTemporaryDir mediaFixture;
    bool fixturesReady=false;
    bool command(const QString &program,const QStringList &args) { QProcess p;p.start(program,args);return p.waitForFinished(30000) && p.exitStatus()==QProcess::NormalExit && p.exitCode()==0; }
    bool makeMediaFixtures() {
        if(fixturesReady)return true;
        const auto direct=mediaFixture.path()+"/direct.mp4";
        if(!command("ffmpeg",{"-nostdin","-v","error","-f","lavfi","-i","testsrc=size=160x90:rate=12","-f","lavfi","-i","sine=frequency=440:sample_rate=48000","-t","2","-c:v","libx264","-pix_fmt","yuv420p","-g","12","-c:a","aac","-shortest",direct}))return false;
        if(!command("ffmpeg",{"-nostdin","-v","error","-i",direct,"-c","copy","-hls_time","1","-hls_playlist_type","vod","-hls_segment_filename",mediaFixture.path()+"/segment%02d.ts",mediaFixture.path()+"/stream.m3u8"}))return false;
        if(!command("ffmpeg",{"-nostdin","-v","error","-i",direct,"-map","0:v","-map","0:a","-c","copy","-f","dash","-seg_duration","1",mediaFixture.path()+"/stream.mpd"}))return false;
        fixturesReady=true;return true;
    }
    QJsonObject probe(const QString &path) { QProcess p;p.start("ffprobe",{"-v","error","-show_streams","-show_format","-of","json",path});if(!p.waitForFinished(10000) || p.exitCode()!=0)return {};return QJsonDocument::fromJson(p.readAllStandardOutput()).object(); }
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
    void browserHeadersAndDownloadLater() {
        Server s;s.requiredCookie="session=private";QTemporaryDir dir;QString id;
        {
            DownloadManager m(dir.path()+"/state");
            id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"filename","chosen.zip"},{"start",false},{"description","Browser download"},
                             {"headers",QVariantMap{{"Cookie","session=private"},{"User-Agent","FixtureAgent"},{"Referer",s.url("page")},{"Host","evil.invalid"},{"Range","bytes=99-100"}}}});
            QVERIFY(!id.isEmpty());QCOMPARE(row(m)["status"].toString(),QString("paused"));QTest::qWait(30);QCOMPARE(s.requests,0);
            QVERIFY(!row(m).contains("headers"));QCOMPARE(row(m)["category"].toString(),QString("Compressed"));QCOMPARE(row(m)["description"].toString(),QString("Browser download"));
        }
        DownloadManager m(dir.path()+"/state");m.resumeAll();QTRY_COMPARE(row(m)["status"].toString(),QString("complete"));
        QCOMPARE(contents(row(m)["path"].toString()),s.body);QCOMPARE(row(m)["name"].toString(),QString("chosen.zip"));
        QVERIFY(s.observedHeaders.size()>1);for(auto h:s.observedHeaders){QCOMPARE(h["cookie"].toString(),QString("session=private"));QCOMPARE(h["user-agent"].toString(),QString("FixtureAgent"));QVERIFY(h["host"].toString()!="evil.invalid");}
        QCOMPARE(s.ranges[0],QByteArray("bytes=0-0"));QVERIFY(!row(m)["parts"].toList().isEmpty());QVERIFY(row(m)["resumable"].toBool());
        const auto path=row(m)["path"].toString();m.removeCompleted();QVERIFY(m.downloads().isEmpty());QVERIFY(QFile::exists(path));
    }
    void rejectsBrowserHeaderInjection() {
        Server s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        for(auto headers:QList<QVariantMap>{{{"Cookie","secret\r\nInjected: yes"}},{{"Host\nBad","value"}},{{"Origin","file:///etc/passwd"}}})
            QVERIFY(m.addRequest({{"url",s.url()},{"directory",dir.path()},{"headers",headers}}).isEmpty());
        QVERIFY(m.addRequest({{"url",s.url()},{"method","POST"}}).isEmpty());QCOMPARE(s.requests,0);
    }
    void authenticatedCrossOriginRedirectIsBlocked() {
        Server other;Server source;source.mode="redirect";source.redirectTarget=other.url();QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        m.addRequest({{"url",source.url("redirect")},{"directory",dir.path()+"/out"},{"headers",QVariantMap{{"Cookie","private-cookie"},{"Authorization","Bearer private-token"}}}});
        QTRY_COMPARE(row(m)["status"].toString(),QString("error"));QCOMPARE(other.requests,0);QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void mediaSourcesProducePlayableOutput_data() {
        QTest::addColumn<QString>("source");QTest::addColumn<QString>("quality");
        QTest::newRow("direct")<<QString("direct.mp4")<<QString("best");
        QTest::newRow("HLS")<<QString("stream.m3u8")<<QString("720p");
        QTest::newRow("DASH")<<QString("stream.mpd")<<QString("1080p");
        QTest::newRow("audio")<<QString("direct.mp4")<<QString("audio");
    }
    void mediaSourcesProducePlayableOutput() {
        QFETCH(QString,source);QFETCH(QString,quality);QVERIFY(makeMediaFixtures());Server s;s.fixtureRoot=mediaFixture.path();s.requiredCookie="session=media";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url(source)},{"directory",dir.path()+"/out"},{"media",true},{"format",quality},{"headers",QVariantMap{{"Cookie","session=media"},{"Referer",s.url("page")}}}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["status"].toString()=="complete" || row(m)["status"].toString()=="error",30000);
        QVERIFY2(row(m)["status"].toString()=="complete",qPrintable(row(m)["error"].toString()));
        const auto metadata=probe(row(m)["path"].toString());QVERIFY(metadata["format"].toObject()["duration"].toString().toDouble()>0);
        bool video=false,audio=false;for(auto stream:metadata["streams"].toArray()){auto type=stream.toObject()["codec_type"].toString();video|=type=="video";audio|=type=="audio";}
        QVERIFY(audio);QCOMPARE(video,quality!="audio");QVERIFY(row(m)["media"].toBool());QVERIFY(!row(m)["resumable"].toBool());
        for(auto headers:s.observedHeaders)QCOMPARE(headers["cookie"].toString(),QString("session=media"));
    }
    void mediaRedirectStripsCrossOriginCredentials() {
        QVERIFY(makeMediaFixtures());Server other;other.fixtureRoot=mediaFixture.path();Server source;source.mode="redirect";source.redirectTarget=other.url("direct.mp4");source.requiredCookie="session=source";
        QTemporaryDir dir;DownloadManager m(dir.path()+"/state");const auto id=m.addRequest({{"url",source.url("redirect")},{"directory",dir.path()+"/out"},{"media",true},
                    {"headers",QVariantMap{{"Cookie","session=source"},{"Authorization","Bearer source-secret"},{"Referer",source.url("private-page")}}}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["status"].toString()=="complete" || row(m)["status"].toString()=="error",30000);
        QVERIFY2(row(m)["status"].toString()=="complete",qPrintable(row(m)["error"].toString()));QVERIFY(other.requests>0);
        for(auto headers:other.observedHeaders){QVERIFY(!headers.contains("cookie"));QVERIFY(!headers.contains("authorization"));QVERIFY(!headers.contains("referer"));}
        QVERIFY(!probe(row(m)["path"].toString()).isEmpty());
    }
    void pausedMediaPersistsAndRestartsSafely() {
        QVERIFY(makeMediaFixtures());Server s;s.fixtureRoot=mediaFixture.path();s.slow=true;QTemporaryDir dir;QString id;bool paused=false;
        {
            DownloadManager m(dir.path()+"/state");
            connect(&m,&DownloadManager::changed,this,[&] {
                if(paused || row(m)["received"].toLongLong()<=0 || row(m)["status"].toString()!="downloading")return;
                paused=true;m.pause(id);
            });
            id=m.addRequest({{"url",s.url("direct.mp4")},{"directory",dir.path()+"/out"},{"media",true},{"description","Retained media job"}});
            QVERIFY2(!id.isEmpty(),qPrintable(m.error()));QTRY_VERIFY_WITH_TIMEOUT(paused,30000);
            QCOMPARE(row(m)["status"].toString(),QString("paused"));QVERIFY(!QFile::exists(row(m)["path"].toString()));
        }
        DownloadManager m(dir.path()+"/state");QCOMPARE(row(m)["status"].toString(),QString("paused"));QVERIFY(row(m)["media"].toBool());QCOMPARE(row(m)["description"].toString(),QString("Retained media job"));m.resume(id);
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["status"].toString()=="complete" || row(m)["status"].toString()=="error",30000);
        QVERIFY2(row(m)["status"].toString()=="complete",qPrintable(row(m)["error"].toString()));QVERIFY(!probe(row(m)["path"].toString()).isEmpty());
    }
    void mediaFailureNeverPublishes() {
        Server s;s.mode="error";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.addRequest({{"url",s.url("bad.mp4")},{"directory",dir.path()+"/out"},{"media",true}});QVERIFY2(!id.isEmpty(),qPrintable(m.error()));
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),30000);QVERIFY(!QFile::exists(row(m)["path"].toString()));
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
