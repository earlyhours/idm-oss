#include "downloadmanager.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QPointer>
#include <QScopeGuard>
#include <QStandardPaths>
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
    int requests=0, rangeRequests=0, connectionsAccepted=0;
    QList<QByteArray> ranges;
    bool slow=false,keepAlive=false,uneven=false;
    int chunkSize=2048,chunkInterval=5;
    QString fixtureRoot, redirectTarget="/file.bin";
    QByteArray requiredCookie,requiredAuthorization,requiredProxyAuthorization;
    QVariantMap requiredHeaders;
    QStringList targets;
    QList<QVariantMap> observedHeaders;
    Server() {
        body.resize(512*1024);
        for(int i=0;i<body.size();++i) body[i]=char((i*37+i/251)%256);
        connect(this,&QTcpServer::newConnection,this,[this] {
            while(hasPendingConnections()) {
                auto socket=nextPendingConnection(); ++connectionsAccepted; auto input=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,input] {
                    *input+=socket->readAll(); if(!input->contains("\r\n\r\n")) return;
                    const QByteArray request=*input;input->clear();if(!keepAlive)socket->disconnect(this);++requests;
                    targets.append(QString::fromLatin1(request.split(' ').value(1)));
                    QVariantMap receivedHeaders;
                    for (auto line : request.split('\n')) { const auto colon=line.indexOf(':'); if(colon>0) receivedHeaders.insert(QString::fromLatin1(line.left(colon).trimmed().toLower()),QString::fromLatin1(line.mid(colon+1).trimmed())); }
                    observedHeaders.append(receivedHeaders);
                    for(auto it=requiredHeaders.begin();it!=requiredHeaders.end();++it)if(receivedHeaders.value(it.key()).toString()!=it.value().toString()){socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return;}

                    if(mode=="stall")return;
                    if(!requiredAuthorization.isEmpty() && receivedHeaders.value("authorization").toByteArray()!=requiredAuthorization){socket->write("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return;}
                    if(!requiredProxyAuthorization.isEmpty() && receivedHeaders.value("proxy-authorization").toByteArray()!=requiredProxyAuthorization){socket->write("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"ProxyFixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return;}

                    if(!requiredCookie.isEmpty() && receivedHeaders.value("cookie").toByteArray()!=requiredCookie) { socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                    QByteArray payload=body;QByteArray contentType="application/octet-stream";
                    if(!fixtureRoot.isEmpty()) {
                        const auto target=request.split(' ').value(1);const auto path=QUrl("http://fixture"+QString::fromLatin1(target)).path();
                        QFile file(fixtureRoot+path);
                        if(!QFileInfo(file).canonicalFilePath().startsWith(QFileInfo(fixtureRoot).canonicalFilePath()+"/") || !file.open(QIODevice::ReadOnly)) { socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                        payload=file.readAll();
                        if(path.endsWith(".m3u8")) contentType="application/vnd.apple.mpegurl";
                        else if(path.endsWith(".mpd")) contentType="application/dash+xml";
                        else if(path.endsWith(".mp4")) contentType="video/mp4";
                        else if(path.endsWith(".ts")) contentType="video/mp2t";
                    }
                    QByteArray range;
                    for(auto line:request.split('\n')) if(line.toLower().startsWith("range:")) range=line.mid(6).trimmed();
                    if(!range.isEmpty()) ranges.append(range);
                    if(mode=="error") { socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nConnection: close\r\n\r\nno!"); socket->disconnectFromHost(); return; }
                    if(mode=="redirect" && request.startsWith("GET /redirect")) { socket->write("HTTP/1.1 302 Found\r\nLocation: "+redirectTarget.toUtf8()+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost(); return; }
                    qint64 first=0,last=payload.size()-1; bool partial=false;
                    auto match=QRegularExpression("^bytes=([0-9]+)-([0-9]+)$").match(QString::fromLatin1(range));
                    if(mode!="serial" && mode!="unknown" && match.hasMatch()) { first=match.captured(1).toLongLong();last=match.captured(2).toLongLong();partial=true; ++rangeRequests; }
                    if(first>=payload.size() || last>=payload.size() || first>last) { socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");socket->disconnectFromHost();return; }
                    const auto bytes=payload.mid(first,last-first+1);
                    QByteArray header=partial?"HTTP/1.1 206 Partial Content\r\n":"HTTP/1.1 200 OK\r\n";
                    if(mode!="unknown") header+="Content-Length: "+QByteArray::number(bytes.size())+"\r\n";
                    if(!etag.isEmpty())header+="ETag: "+etag+"\r\n";
                    if(partial) header+="Content-Range: bytes "+QByteArray::number(mode=="wrong" && last>0?first+1:first)+"-"+QByteArray::number(last)+"/"+QByteArray::number(payload.size())+"\r\n";
                    header+="Content-Type: "+contentType+"\r\nConnection: "+(keepAlive?"keep-alive":"close")+"\r\n\r\n"; socket->write(header);
                    if(request.startsWith("HEAD ")) { socket->disconnectFromHost();return; }
                    if(slow && bytes.size()>1) {
                        auto timer=new QTimer(socket);auto offset=std::make_shared<int>(0);
                        connect(timer,&QTimer::timeout,socket,[this,socket,timer,offset,bytes] {
                            if(socket->state()!=QAbstractSocket::ConnectedState){timer->stop();return;}
                            socket->write(bytes.mid(*offset,chunkSize)); *offset+=chunkSize;
                            if(*offset>=bytes.size()){timer->stop();if(!keepAlive)socket->disconnectFromHost();}
                        }); timer->start(uneven && first==0 ? 20 : uneven ? 1 : chunkInterval);
                    } else { socket->write(bytes); if(!keepAlive)socket->disconnectFromHost(); }
                });
            }
        });
        if(!listen(QHostAddress::LocalHost)) qFatal("Cannot listen");
    }
    QString url(QString path="file.bin") const {return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(path);}
};

class SocksProxy : public QTcpServer {
public:
    int tunnels=0;
    QByteArray username,password;
    explicit SocksProxy(quint16 targetPort) {
        connect(this,&QTcpServer::newConnection,this,[this,targetPort] {
            while(hasPendingConnections()) {
                auto client=nextPendingConnection();auto remote=new QTcpSocket(client);auto data=std::make_shared<QByteArray>();auto stage=std::make_shared<int>(0);
                connect(client,&QTcpSocket::disconnected,client,&QObject::deleteLater);
                connect(remote,&QTcpSocket::readyRead,client,[client,remote]{client->write(remote->readAll());});
                connect(remote,&QTcpSocket::disconnected,client,&QTcpSocket::disconnectFromHost);
                connect(remote,&QTcpSocket::connected,client,[this,client,stage]{++tunnels;*stage=3;client->write(QByteArray::fromHex("05000001000000000000"));});
                connect(client,&QTcpSocket::readyRead,this,[this,client,remote,data,stage,targetPort] {
                    *data+=client->readAll();
                    if(*stage==3){remote->write(*data);data->clear();return;}
                    if(*stage==0){if(data->size()<2 || data->size()<2+quint8((*data)[1]))return;data->remove(0,2+quint8((*data)[1]));client->write(QByteArray::fromHex("0502"));*stage=1;}
                    if(*stage==1){if(data->size()<2)return;const int length=quint8((*data)[1]);if(data->size()<3+length)return;const int pass=quint8((*data)[2+length]);if(data->size()<3+length+pass)return;username=data->mid(2,length);password=data->mid(3+length,pass);data->remove(0,3+length+pass);client->write(QByteArray::fromHex("0100"));*stage=2;}
                    if(*stage==2){if(data->size()<5)return;const int address=quint8((*data)[3]);const int length=address==1?4:address==4?16:1+quint8((*data)[4]);if(data->size()<6+length)return;data->remove(0,6+length);*stage=4;remote->connectToHost(QHostAddress::LocalHost,targetPort);}
                });
            }
        });
        if(!listen(QHostAddress::LocalHost))qFatal("Cannot listen for SOCKS fixture");
    }
};

class FtpServer : public QTcpServer {
public:
    QByteArray body=QByteArray(131072,'f');QString username,password;int retrievals=0;
    FtpServer() {
        connect(this,&QTcpServer::newConnection,this,[this] {
            while(hasPendingConnections()) {
                auto control=nextPendingConnection();control->write("220 Fixture FTP\r\n");auto input=std::make_shared<QByteArray>();auto passive=new QTcpServer(control);auto data=std::make_shared<QPointer<QTcpSocket>>();auto retrieving=std::make_shared<bool>(false);
                connect(control,&QTcpSocket::disconnected,control,&QObject::deleteLater);
                auto send=[this,control,data,retrieving]{if(!*retrieving || !*data)return;*retrieving=false;(*data)->write(body);(*data)->disconnectFromHost();control->write("226 Transfer complete\r\n");};
                connect(passive,&QTcpServer::newConnection,control,[passive,data,send]{*data=passive->nextPendingConnection();send();});
                connect(control,&QTcpSocket::readyRead,this,[this,control,input,passive,retrieving,send] {
                    *input+=control->readAll();while(input->contains('\n')) {
                        const auto end=input->indexOf('\n');const auto line=input->left(end).trimmed();input->remove(0,end+1);const auto split=line.indexOf(' ');const auto command=split<0?line:line.left(split),argument=split<0?QByteArray():line.mid(split+1);
                        if(command=="USER"){username=QString::fromUtf8(argument);control->write("331 Password required\r\n");}
                        else if(command=="PASS"){password=QString::fromUtf8(argument);control->write(argument=="secret"?"230 Logged in\r\n":"530 Invalid login\r\n");}
                        else if(command=="PWD")control->write("257 \"/\"\r\n");
                        else if(command=="CWD" || command=="TYPE")control->write("200 OK\r\n");
                        else if(command=="EPSV"){if(!passive->isListening())passive->listen(QHostAddress::LocalHost);control->write("229 Entering Extended Passive Mode (|||"+QByteArray::number(passive->serverPort())+"|)\r\n");}
                        else if(command=="SIZE")control->write("213 "+QByteArray::number(body.size())+"\r\n");
                        else if(command=="MDTM")control->write("213 20260101000000\r\n");
                        else if(command=="RETR"){++retrievals;*retrieving=true;control->write("150 Opening data connection\r\n");send();}
                        else if(command=="QUIT"){control->write("221 Bye\r\n");control->disconnectFromHost();}
                        else control->write("500 Unsupported command\r\n");
                    }
                });
            }
        });
        if(!listen(QHostAddress::LocalHost))qFatal("Cannot listen for FTP fixture");
    }
    QString url()const{return QString("ftp://127.0.0.1:%1/file.bin").arg(serverPort());}
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
    void mediaWorkerChecksYtdlpWithoutRequiringAnExecutable() {
        QTemporaryDir tools,dir;
        for(const auto &name:QStringList{"ffmpeg","ffprobe"}) {
            const auto executable=QStandardPaths::findExecutable(name);
            QVERIFY(!executable.isEmpty());QVERIFY(QFile::link(executable,tools.filePath(name)));
        }
        const auto oldPath=qgetenv("PATH");
        const auto restorePath=qScopeGuard([&]{qputenv("PATH",oldPath);});
        qputenv("PATH",tools.path().toUtf8());
        QVERIFY(QStandardPaths::findExecutable("yt-dlp").isEmpty());
        Server s;s.mode="error";DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url("video.mp4")},{"directory",dir.path()+"/out"},{"media",true},{"start",false}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));QCOMPARE(row(m)["status"].toString(),QString("paused"));
        m.resume(id);QCOMPARE(row(m)["status"].toString(),QString("downloading"));
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),30000);
        QVERIFY2(row(m)["error"].toString().startsWith("Cannot load yt-dlp") || row(m)["error"].toString().startsWith("Media capture failed"),qPrintable(row(m)["error"].toString()));
        QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void largeFilePublication_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("range") << QString("range");
        QTest::newRow("serial") << QString("serial");
    }
    void largeFilePublication() {
        QFETCH(QString,mode);Server s;s.mode=mode;
        const auto block=s.body;for(int i=0;i<15;++i)s.body+=block;
        QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.add(s.url(),dir.path()+"/out",4);QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),15000);
        QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QCOMPARE(row(m)["received"].toLongLong(),qint64(s.body.size()));
        QVERIFY(!QFile::exists(row(m)["path"].toString()+".idm-native-"+id+".tmp"));
    }
    void rateLimitsDrainRangeAndSerialWithoutTruncation_data() {QTest::addColumn<QString>("mode");QTest::newRow("range")<<QString("range");QTest::newRow("serial")<<QString("serial");}
    void rateLimitsDrainRangeAndSerialWithoutTruncation() {
        QFETCH(QString,mode);Server s;s.mode=mode;s.body.resize(65536);QTemporaryDir dir;DownloadManager m(dir.path()+"/state");QElapsedTimer clock;clock.start();
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"rateLimitBytes",32768},{"timeoutSeconds",1}});QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QVERIFY(clock.elapsed()>=1700);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QCOMPARE(row(m)["rateLimitBytes"].toLongLong(),qint64(32768));
    }
    void liveRateLimitCanBeRemoved() {
        Server s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"rateLimitBytes",32768}});
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["received"].toLongLong()>8192,10000);m.setRateLimit(id,0);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void stalledServerTimesOut() {
        Server s;s.mode="stall";QTemporaryDir dir;DownloadManager m(dir.path()+"/state");QElapsedTimer clock;clock.start();
        m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"timeoutSeconds",1}});
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),5000);QVERIFY(clock.elapsed()<4000);QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void sourceBasicAuthenticationIsScopedAndPrivate() {
        Server s;s.requiredAuthorization="Basic "+QByteArray("user:secret").toBase64();QTemporaryDir dir;QString id;
        {DownloadManager m(dir.path()+"/state");id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"username","user"},{"password","secret"},{"start",false}});QVERIFY(!id.isEmpty());}
        DownloadManager m(dir.path()+"/state");m.resume(id);QTRY_VERIFY_WITH_TIMEOUT(row(m)["status"].toString()=="complete" || row(m)["status"].toString()=="error",10000);QVERIFY2(row(m)["status"].toString()=="complete",qPrintable(row(m)["error"].toString()));QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QVERIFY(!row(m).contains("password"));QVERIFY(!row(m).contains("username"));QCOMPARE(row(m)["httpStatus"].toInt(),206);
    }
    void httpProxyUsesItsOwnCredentials() {
        Server proxy;proxy.requiredProxyAuthorization="Basic "+QByteArray("proxy-user:proxy-secret").toBase64();QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url","http://fixture.invalid/file.bin"},{"directory",dir.path()+"/out"},
                 {"proxy",QVariantMap{{"type","http"},{"host","127.0.0.1"},{"port",proxy.serverPort()},{"username","proxy-user"},{"password","proxy-secret"}}}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),proxy.body);
        QVERIFY(proxy.targets[0].startsWith("http://fixture.invalid/"));bool authenticated=false;for(auto h:proxy.observedHeaders)authenticated|=h.value("proxy-authorization").toByteArray()==proxy.requiredProxyAuthorization;QVERIFY(authenticated);
    }
    void socksProxyActuallyTunnelsDownloads() {
        Server s;SocksProxy proxy(s.serverPort());QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},
                 {"proxy",QVariantMap{{"type","socks5"},{"host","127.0.0.1"},{"port",proxy.serverPort()},{"username","socks-user"},{"password","socks-secret"}}}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QVERIFY(proxy.tunnels>=1);QCOMPARE(proxy.username,QByteArray("socks-user"));QCOMPARE(proxy.password,QByteArray("socks-secret"));
    }
    void renameAndRefreshPreserveExistingData() {
        Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");auto id=m.add(s.url("file.bin?expired=1"),dir.path()+"/out");
        QTRY_VERIFY(row(m)["received"].toLongLong()>8192);m.pause(id);const auto before=row(m)["received"].toLongLong();
        QVERIFY(!m.refresh(id,s.url("another-file.bin")));QCOMPARE(row(m)["received"].toLongLong(),before);
        QVERIFY(m.refresh(id,s.url("file.bin?fresh=1")));QVERIFY(m.rename(id,"renamed.bin"));QVERIFY(!m.rename(id,"../bad.bin"));m.resume(id);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QVERIFY(m.rename(id,"finished.bin"));QVERIFY(row(m)["path"].toString().endsWith("finished.bin"));QCOMPARE(contents(row(m)["path"].toString()),s.body);
        QFile other(dir.path()+"/out/existing.bin");QVERIFY(other.open(QIODevice::WriteOnly));other.write("keep");other.close();QVERIFY(!m.rename(id,"existing.bin"));QCOMPARE(contents(other.fileName()),QByteArray("keep"));
    }
    void dynamicWorkStealingSurvivesRestartAndPreservesOrdering() {
        Server s;s.slow=true;s.uneven=true;s.chunkSize=8192;s.body=s.body.repeated(8);QTemporaryDir dir;QString id;bool paused=false;
        {
            DownloadManager m(dir.path()+"/state");
            connect(&m,&DownloadManager::changed,this,[&]{if(paused || row(m)["parts"].toList().size()<=4 || row(m)["status"].toString()!="downloading")return;paused=true;m.pause(id);});
            id=m.add(s.url(),dir.path()+"/out");QTRY_VERIFY_WITH_TIMEOUT(paused,10000);QVERIFY(row(m)["parts"].toList().size()>4);
        }
        DownloadManager m(dir.path()+"/state");QVERIFY2(m.error().isEmpty(),qPrintable(m.error()));m.resume(id);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),20000);QCOMPARE(contents(row(m)["path"].toString()),s.body);QVERIFY(s.rangeRequests>5);
    }
    void persistentHttpConnectionsAreReused() {
        Server s;s.keepAlive=true;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.add(s.url(),dir.path()+"/out",4);
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);QCOMPARE(s.requests,5);
        QVERIFY(s.connectionsAccepted>=2);QVERIFY(s.connectionsAccepted<=4);qInfo()<<"HTTP requests"<<s.requests<<"actual TCP connections"<<s.connectionsAccepted;
    }
    void controlledParallelBenchmark() {
        qint64 elapsed[2];int sockets[2];
        for(int run=0;run<2;++run){Server s;s.slow=true;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");QElapsedTimer clock;clock.start();m.add(s.url(),dir.path()+"/out",run==0?1:4);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);elapsed[run]=clock.elapsed();sockets[run]=s.connectionsAccepted;QCOMPARE(contents(row(m)["path"].toString()),s.body);}
        QVERIFY(elapsed[1]<elapsed[0]*.8);QCOMPARE(sockets[0],2);QCOMPARE(sockets[1],5);
        const QJsonObject result{{"fixture","512KiB at 2048 bytes per connection every 5ms"},{"bytes",524288},{"serialMs",elapsed[0]},{"parallel4Ms",elapsed[1]},{"serialTcpConnections",sockets[0]},{"parallelTcpConnections",sockets[1]},{"ratio",double(elapsed[0])/elapsed[1]}};
        QFile report("/tmp/idm-engine-benchmark.json");QVERIFY(report.open(QIODevice::WriteOnly));report.write(QJsonDocument(result).toJson());qInfo().noquote()<<QJsonDocument(result).toJson(QJsonDocument::Compact);
    }
    void ftpCredentialsAndExactOutput() {
        FtpServer s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"username","ftp-user"},{"password","secret"}});QVERIFY2(!id.isEmpty(),qPrintable(m.error()));
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["status"].toString()=="complete" || row(m)["status"].toString()=="error",10000);QVERIFY2(row(m)["status"].toString()=="complete",qPrintable(row(m)["error"].toString()));QCOMPARE(contents(row(m)["path"].toString()),s.body);QCOMPARE(s.username,QString("ftp-user"));QCOMPARE(s.password,QString("secret"));QCOMPARE(s.retrievals,1);
    }
    void ftpFailureDoesNotPublish() {
        FtpServer s;QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"username","ftp-user"},{"password","wrong"}});
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),10000);QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void arbitraryCustomHeadersWorkAndManagedHeadersStayOwned() {
        Server s;s.requiredHeaders={{"x-api-key","private-api-key"},{"x-client-context","custom-client"}};QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"headers",QVariantMap{
            {"X-Api-Key","private-api-key"},{"X-Client-Context","custom-client"},{"Host","evil.invalid"},{"Range","bytes=99-100"},
            {"Accept-Encoding","gzip"},{"Content-Length","999"},{"Transfer-Encoding","chunked"},{"Proxy-Authorization","do-not-forward"},
            {"Connection","X-Hop-Token"},{"X-Hop-Token","hop-secret"}}}});
        QVERIFY2(!id.isEmpty(),qPrintable(m.error()));QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        for(const auto &headers:s.observedHeaders){QCOMPARE(headers.value("x-api-key").toString(),QString("private-api-key"));QCOMPARE(headers.value("x-client-context").toString(),QString("custom-client"));
            QCOMPARE(headers.value("accept-encoding").toString(),QString("identity"));QVERIFY(headers.value("host").toString()!="evil.invalid");QVERIFY(headers.value("content-length").toString()!="999");
            QVERIFY(!headers.contains("transfer-encoding"));QVERIFY(!headers.contains("proxy-authorization"));QVERIFY(!headers.contains("x-hop-token"));}
        QCOMPARE(s.ranges.first(),QByteArray("bytes=0-0"));QVERIFY(!row(m).contains("headers"));
    }
    void customCredentialsCannotFollowCrossOriginRedirects() {
        Server other;Server source;source.mode="redirect";source.redirectTarget=other.url();source.requiredHeaders={{"x-api-key","origin-only-key"}};
        QTemporaryDir dir;DownloadManager m(dir.path()+"/state");m.addRequest({{"url",source.url("redirect")},{"directory",dir.path()+"/out"},{"headers",QVariantMap{{"X-Api-Key","origin-only-key"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),5000);QCOMPARE(other.requests,0);QVERIFY(source.requests>0);QVERIFY(!QFile::exists(row(m)["path"].toString()));
    }
    void expiredCookieCanBeUpdatedForTheSameUrl() {
        Server s;s.requiredCookie="session=fresh";s.requiredHeaders={{"x-api-key","fresh-key"}};QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"headers",QVariantMap{{"Cookie","session=expired"},{"X-Api-Key","expired-key"},{"User-Agent","OldBrowserContext"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("error"),5000);QCOMPARE(row(m)["httpStatus"].toInt(),403);const auto old=row(m);const int requests=s.requests;
        QVERIFY(m.updateContext(id,{{"url",s.url()+"#same-resource"},{"headers",QVariantMap{{"Cookie","session=fresh"},{"X-Api-Key","fresh-key"}}},{"pageUrl",s.url("watch")}}));
        QCOMPARE(row(m)["url"],old["url"]);QCOMPARE(row(m)["path"],old["path"]);QCOMPARE(row(m)["parts"],old["parts"]);QTest::qWait(30);QCOMPARE(s.requests,requests);
        m.resume(id);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        for(int i=requests;i<s.observedHeaders.size();++i){QCOMPARE(s.observedHeaders[i].value("cookie").toString(),QString("session=fresh"));QCOMPARE(s.observedHeaders[i].value("x-api-key").toString(),QString("fresh-key"));QCOMPARE(s.observedHeaders[i].value("user-agent").toString(),QString("IDM-Native/0.1"));}
    }
    void contextUpdatesKeepPartialBytesAndDiscardOldAuthenticationCache() {
        Server s;s.slow=true;s.requiredAuthorization="Basic "+QByteArray("old-user:old-password").toBase64();QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"username","old-user"},{"password","old-password"}});
        QTRY_VERIFY_WITH_TIMEOUT(row(m)["received"].toLongLong()>8192,10000);m.pause(id);QTest::qWait(30);const auto old=row(m);const int requests=s.requests;
        QVariantMap partials;QDir pieces(dir.path()+"/state/"+id);for(const auto &name:pieces.entryList({"*.part"},QDir::Files))partials[name]=QCryptographicHash::hash(contents(pieces.filePath(name)),QCryptographicHash::Sha256).toHex();
        s.requiredAuthorization="Basic "+QByteArray("new-user:new-password").toBase64();
        QVERIFY(m.updateContext(id,{{"username","new-user"},{"password","new-password"}}));QCOMPARE(row(m)["received"],old["received"]);QCOMPARE(row(m)["parts"],old["parts"]);QCOMPARE(row(m)["resumable"],old["resumable"]);
        for(auto it=partials.begin();it!=partials.end();++it)QCOMPARE(QCryptographicHash::hash(contents(pieces.filePath(it.key())),QCryptographicHash::Sha256).toHex(),it.value().toByteArray());
        m.resume(id);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
        for(int i=requests;i<s.observedHeaders.size();++i)QVERIFY(s.observedHeaders[i].value("authorization").toByteArray()!=QByteArray("Basic ")+QByteArray("old-user:old-password").toBase64());
    }
    void invalidContextUpdateCannotReplaceTheSavedContext() {
        Server s;s.requiredHeaders={{"x-api-key","original-key"}};QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"start",false},{"headers",QVariantMap{{"X-Api-Key","original-key"}}}});const auto old=row(m);
        const auto saved=contents(dir.path()+"/state/queue.json");
        for(const auto &context:QList<QVariantMap>{
            {{"headers",QVariantMap{{"X-Api-Key","bad\r\nInjected: yes"}}}},{{"headers","not a header map"}},{{"url","https://other.invalid/file.bin"}},
            {{"url",s.url("other.bin")}},{{"url",s.url()+"?different=resource"}},{{"pageUrl","file:///etc/passwd"}},{{"username","bad\nuser"}},{{"password","without-user"}},{{"method","POST"}}}) {
            QVERIFY(!m.updateContext(id,context));QVERIFY(!m.error().isEmpty());QCOMPARE(row(m),old);QCOMPARE(contents(dir.path()+"/state/queue.json"),saved);
        }
        m.resume(id);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void contextUpdateIsPrivatePersistentAndCannotRunOnActiveJobs() {
        Server s;s.slow=true;s.requiredCookie="session=fresh";s.requiredHeaders={{"x-api-key","fresh-key"}};QTemporaryDir dir;QString id;
        {
            DownloadManager m(dir.path()+"/state");id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"media",false},{"format","720p"},{"start",false}});
            QVERIFY(m.updateContext(id,{{"headers",QVariantMap{{"Cookie","session=fresh"},{"X-Api-Key","fresh-key"}}}}));QCOMPARE(row(m)["format"].toString(),QString("720p"));
            QFile file(dir.path()+"/state/queue.json");QVERIFY(!(file.permissions()&(QFile::ReadGroup|QFile::ReadOther)));QCOMPARE(s.requests,0);
        }
        DownloadManager m(dir.path()+"/state");m.resume(id);QVERIFY(!m.updateContext(id,{{"headers",QVariantMap{{"X-Api-Key","bad-key"}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QVERIFY(!m.updateContext(id,{}));QCOMPARE(contents(row(m)["path"].toString()),s.body);
    }
    void failedContextSaveRollsBackTheLiveCredentials() {
        Server s;s.requiredHeaders={{"x-api-key","original-key"}};QTemporaryDir dir;DownloadManager m(dir.path()+"/state");
        const auto id=m.addRequest({{"url",s.url()},{"directory",dir.path()+"/out"},{"start",false},{"headers",QVariantMap{{"X-Api-Key","original-key"}}}});
        const QString queue=dir.path()+"/state/queue.json",backup=dir.path()+"/state/queue.backup";QVERIFY(QFile::rename(queue,backup));QVERIFY(QDir().mkdir(queue));
        QVERIFY(!m.updateContext(id,{{"headers",QVariantMap{{"X-Api-Key","rejected-key"}}}}));QVERIFY(!m.error().isEmpty());QVERIFY(QDir().rmdir(queue));QVERIFY(QFile::rename(backup,queue));
        m.resume(id);QTRY_COMPARE_WITH_TIMEOUT(row(m)["status"].toString(),QString("complete"),10000);QCOMPARE(contents(row(m)["path"].toString()),s.body);
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
