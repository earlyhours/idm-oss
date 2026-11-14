#include "downloadmanager.h"
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QAuthenticator>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {
bool httpUrl(const QUrl &url) {
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty() && url.userInfo().isEmpty();
}
QString origin(const QUrl &url) { return url.scheme().toLower() + "://" + url.host().toLower() + ":" + QString::number(url.port(url.scheme() == "https" ? 443 : url.scheme() == "ftp" ? 21 : url.scheme() == "ftps" ? 990 : 80)); }
bool requestHeaders(const QVariantMap &input, QVariantMap &result, QString &error) {
    QStringList managed{"host", "content-length", "transfer-encoding", "connection", "proxy-connection", "keep-alive", "te", "trailer", "upgrade", "expect", "range", "if-range", "content-range", "accept-encoding", "proxy-authorization", "proxy-authenticate"};
    for(auto it=input.begin();it!=input.end();++it)if(it.key().compare("connection",Qt::CaseInsensitive)==0)
        for(const auto &name:it.value().toString().split(','))managed.append(name.trimmed().toLower());
    if (input.size() > 64) { error = DownloadManager::tr("Too many request headers."); return false; }
    qsizetype length = 0;
    for (auto it = input.begin(); it != input.end(); ++it) {
        const auto name = it.key().toLower(); const auto value = it.value().toString(); length += name.size() + value.size();
        if (it.value().metaType().id() != QMetaType::QString || value.size() > 16384 || length > 65536 ||
            !QRegularExpression("^[!#$%&'*+.^_`|~0-9a-z-]+$").match(name).hasMatch() ||
            value.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))) {
            error = DownloadManager::tr("Invalid request header. Control characters and oversized headers are rejected."); return false;
        }
        if (managed.contains(name)) continue;
        if ((name == "referer" || name == "origin") && !(name == "origin" && value == "null") && !httpUrl(QUrl(value,QUrl::StrictMode))) {
            error = DownloadManager::tr("Invalid request origin or referrer."); return false;
        }
        result.insert(name,value);
    }
    return true;
}
bool advancedRequest(const QVariantMap &request, QString &error) {
    const auto controls=QRegularExpression("[\\x00-\\x1f\\x7f]");
    for(const auto &key:QStringList{"username","password"}) {
        const auto value=request.value(key).toString();
        if(value.size()>4096 || value.contains(controls) || (key=="username" && value.contains(':'))) { error=DownloadManager::tr("Invalid source credentials.");return false; }
    }
    if(request.value("username").toString().isEmpty() && !request.value("password").toString().isEmpty()) { error=DownloadManager::tr("A source password needs a username.");return false; }
    bool rateOk=true,timeoutOk=true;
    const auto rate=request.contains("rateLimitBytes")?request.value("rateLimitBytes").toLongLong(&rateOk):0;
    const int timeout=request.contains("timeoutSeconds")?request.value("timeoutSeconds").toInt(&timeoutOk):30;
    if(!rateOk || rate<0 || rate>1000000000000LL || !timeoutOk || timeout<1 || timeout>3600) { error=DownloadManager::tr("Invalid speed limit or transfer timeout.");return false; }
    if(!QStringList{"auto","basic","digest","ntlm","negotiate","kerberos"}.contains(request.value("authMethod","auto").toString())) { error=DownloadManager::tr("Choose Auto, Basic, Digest, NTLM, Negotiate or Kerberos authentication.");return false; }
    if(request.contains("proxy") && request.value("proxy").metaType().id()!=QMetaType::QVariantMap) { error=DownloadManager::tr("Proxy settings must be a map.");return false; }
    const auto proxy=request.value("proxy").toMap();if(proxy.isEmpty())return true;
    const auto type=proxy.value("type").toString(),host=proxy.value("host").toString();
    if(!QStringList{"http","https","socks5"}.contains(type) || host.isEmpty() || host.size()>253 || host.contains(controls) || host.contains('/') || host.contains('@') || host.contains(' ') || proxy.value("port").toInt()<1 || proxy.value("port").toInt()>65535 ||
       proxy.value("username").toString().contains(controls) || proxy.value("username").toString().contains(':') || proxy.value("password").toString().contains(controls)) {
        error=DownloadManager::tr("Enter a valid HTTP, HTTPS or SOCKS5 proxy host and port.");return false;
    }
    return true;
}
bool downloadUrl(const QUrl &url) { return httpUrl(url) || (url.isValid() && (url.scheme()=="ftp" || url.scheme()=="ftps") && !url.host().isEmpty() && url.userInfo().isEmpty()); }
QString category(const QString &filename, bool media) {
    const auto suffix = QFileInfo(filename).suffix().toLower();
    if (QStringList{"zip","7z","rar","tar","gz","xz","bz2","zst"}.contains(suffix)) return "Compressed";
    if (QStringList{"pdf","txt","doc","docx","odt","ods","xls","xlsx","ppt","pptx","epub","csv","rtf"}.contains(suffix)) return "Documents";
    if (QStringList{"mp3","m4a","wav","flac","ogg","opus","aac","mka"}.contains(suffix)) return "Music";
    if (media || QStringList{"mp4","mkv","webm","mov","avi","m4v","ts"}.contains(suffix)) return "Video";
    if (QStringList{"exe","msi","appimage","deb","rpm","apk","sh"}.contains(suffix)) return "Programs";
    return "Other";
}
}

struct DownloadManager::Impl {
    struct Part {
        qint64 first = 0, last = -1, bytes = 0;
        std::unique_ptr<QFile> file;
        QPointer<QNetworkReply> reply;
        bool checked = false;
        QPointer<QTimer> timer;
        QElapsedTimer activity;
    };
    struct Job {
        QString id, url, path, status = "paused", error, title, pageUrl, description;
        QString format, authMethod = "auto";
        bool dateValidatorStrong = false;
        bool workerResumable = false;
        QString created = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QVariantMap headers;
        bool media = false;
        QPointer<QProcess> process;
        QByteArray processOutput;
        double mediaSpeed = 0;
        qint64 mediaEta = -1;
        int connections = 4, httpStatus = 0, timeoutSeconds = 30;
        qint64 rateLimitBytes = 0;
        double tokens = 0;
        QElapsedTimer budgetClock;
        QString username, password;
        QVariantMap proxy;
        std::unique_ptr<QNetworkAccessManager> access;
        QPointer<QTimer> probeTimer;
        qint64 total = -1, baseline = 0;
        QByteArray validator, validatorHeader;
        bool ranged = false;
        QElapsedTimer clock;
        QVector<std::shared_ptr<Part>> parts;
        QPointer<QNetworkReply> probe;
    };
    DownloadManager *q;
    QString root, destination, error;
    QTimer checkpoint;
    std::unique_ptr<QLockFile> lock;
    QVector<std::shared_ptr<Job>> jobs;
    bool writable = false;

    Impl(DownloadManager *owner, const QString &state) : q(owner) {
        destination = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (destination.isEmpty()) destination = QDir::homePath() + "/Downloads";
        root = state.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : state;
        root = QDir(root).absolutePath();
        if (!QDir().mkpath(root)) { error = q->tr("Cannot create the download state directory."); return; }
        lock = std::make_unique<QLockFile>(root + "/queue.lock");
        if (!lock->tryLock(0)) { error = q->tr("Another download manager is using this queue."); return; }
        writable = true;
        load();
        checkpoint.setInterval(1000);
        QObject::connect(&checkpoint, &QTimer::timeout, q, [this] { save(); emit q->changed(); });
        checkpoint.start();
    }
    ~Impl() { checkpoint.stop(); for (auto &j : jobs) stop(j); save(); }
    QString partPath(const Job &j, int index) const { return root + "/" + j.id + "/" + QString::number(index) + ".part"; }
    qint64 received(const Job &j) const { qint64 n = 0; for (const auto &p : j.parts) n += p->bytes; return n; }
    std::shared_ptr<Job> find(const QString &id) const { for (auto &j : jobs) if (j->id == id) return j; return {}; }
    void stop(const std::shared_ptr<Job> &j) {
        if (j->status == "probing" || j->status == "downloading") j->status = "paused";
        if (j->probeTimer) { j->probeTimer->stop(); j->probeTimer->deleteLater(); j->probeTimer = nullptr; }
        if (j->probe) { j->probe->disconnect(q); j->probe->abort(); j->probe->deleteLater(); j->probe = nullptr; }
        if (j->process) {
            auto process = j->process; j->process = nullptr; process->disconnect(q);
            process->terminate(); if (!process->waitForFinished(1500)) { process->kill(); process->waitForFinished(1000); }
            process->deleteLater();
        }
        for (auto &p : j->parts) {
            if (p->timer) { p->timer->stop(); p->timer->deleteLater(); p->timer = nullptr; }
            if (p->reply) { p->reply->disconnect(q); p->reply->abort(); p->reply->deleteLater(); p->reply = nullptr; }
            if (p->file) { p->file->close(); p->file.reset(); }
        }
    }
    void fail(const std::shared_ptr<Job> &j, const QString &message) { stop(j); j->status = "error"; j->error = message; save(); emit q->changed(); }
    bool save() {
        if (!writable) return false;
        QJsonArray rows;
        for (const auto &j : jobs) {
            QJsonArray parts;
            if (!j->media && j->status != "complete") for (auto &p : j->parts) parts.append(QJsonObject{{"first", QString::number(p->first)}, {"last", QString::number(p->last)}});
            rows.append(QJsonObject{{"id", j->id}, {"url", j->url}, {"path", j->path}, {"status", j->status}, {"error", j->error},
                        {"connections", j->connections}, {"timeoutSeconds", j->timeoutSeconds}, {"rateLimitBytes", QString::number(j->rateLimitBytes)},
                        {"workerResumable",j->workerResumable},{"authMethod",j->authMethod},{"username",j->username},{"password",j->password},{"proxy",QJsonObject::fromVariantMap(j->proxy)}, {"total", QString::number(j->total)}, {"ranged", j->ranged},
                        {"dateValidatorStrong",j->dateValidatorStrong},{"validator", QString::fromLatin1(j->validator.toBase64())}, {"validatorHeader", QString::fromLatin1(j->validatorHeader)}, {"parts", parts},
                        {"headers", QJsonObject::fromVariantMap(j->headers)}, {"created", j->created}, {"title", j->title}, {"pageUrl", j->pageUrl},
                        {"description", j->description}, {"format", j->format}, {"media", j->media}, {"mediaReceived", QString::number(received(*j))}});
        }
        QSaveFile file(root + "/queue.json");
        const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"downloads", rows}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = q->tr("Cannot save download queue: %1").arg(file.errorString()); emit q->changed(); return false;
        }
        return true;
    }
    void load() {
        QFile file(root + "/queue.json");
        if (!file.exists()) return;
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) { error = q->tr("Cannot read download queue."); writable = false; return; }
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        const auto object = doc.object();
        if (parse.error != QJsonParseError::NoError || object.value("version").toInt() != 1 || !object.value("downloads").isArray()) {
            error = q->tr("Invalid download queue. The saved file was left untouched."); writable = false; return;
        }
        for (const auto &row : object.value("downloads").toArray()) {
            auto o = row.toObject(); auto j = std::make_shared<Job>();
            j->id = o.value("id").toString(); j->url = o.value("url").toString(); j->path = o.value("path").toString();
            QUrl url(j->url); bool ok = false; j->total = o.value("total").toString().toLongLong(&ok);
            if (!ok || j->total < -1 || !QRegularExpression("^[a-f0-9]{32}$").match(j->id).hasMatch() || find(j->id) ||
                !downloadUrl(url) || !QFileInfo(j->path).isAbsolute()) {
                error = q->tr("Invalid entry in download queue. The saved file was left untouched."); writable = false; jobs.clear(); return;
            }
            j->connections = std::clamp(o.value("connections").toInt(4), 1, 16);
            j->timeoutSeconds = std::clamp(o.value("timeoutSeconds").toInt(30),1,3600);
            j->rateLimitBytes = std::max<qint64>(0,o.value("rateLimitBytes").toString().toLongLong());
            j->authMethod = o.value("authMethod").toString("auto");
            j->username = o.value("username").toString(); j->password = o.value("password").toString(); j->proxy = o.value("proxy").toObject().toVariantMap();
            j->validator = QByteArray::fromBase64(o.value("validator").toString().toLatin1());
            j->validatorHeader = o.value("validatorHeader").toString().toLatin1();
            j->dateValidatorStrong = o.value("dateValidatorStrong").toBool(false);
            j->ranged = o.value("ranged").toBool();
            j->workerResumable = o.value("workerResumable").toBool();
            j->status = o.value("status").toString() == "complete" ? "complete" : "paused";
            j->error = o.value("error").toString();
            j->media = o.value("media").toBool(); j->title = o.value("title").toString(); j->pageUrl = o.value("pageUrl").toString();
            j->description = o.value("description").toString(); j->format = o.value("format").toString();
            if (!o.value("created").toString().isEmpty()) j->created = o.value("created").toString();
            QString headerError;
            if (!advancedRequest(QVariantMap{{"authMethod",j->authMethod},{"username",j->username},{"password",j->password},{"proxy",j->proxy},{"rateLimitBytes",j->rateLimitBytes},{"timeoutSeconds",j->timeoutSeconds}},headerError) || (!o.value("headers").isUndefined() && !o.value("headers").isObject()) ||
                !requestHeaders(o.value("headers").toObject().toVariantMap(),j->headers,headerError) ||
                (!j->pageUrl.isEmpty() && !httpUrl(QUrl(j->pageUrl,QUrl::StrictMode)))) {
                error = q->tr("Invalid browser context in saved queue. The queue was left untouched."); writable = false; jobs.clear(); return;
            }
            if (j->media) {
                j->ranged = false;
                auto p = std::make_shared<Part>(); p->bytes = j->status == "complete" ? std::max<qint64>(0,j->total) : std::max<qint64>(0,o.value("mediaReceived").toString().toLongLong());
                j->parts.append(p); jobs.append(j); continue;
            }
            qint64 next = 0; bool valid = true;
            const auto parts = o.value("parts").toArray();
            if (parts.size() > 16 || (j->ranged && (j->total <= 0 || j->validator.isEmpty() ||
                (j->validatorHeader != "ETag" && j->validatorHeader != "Last-Modified")))) valid = false;
            for (int i = 0; i < parts.size() && valid; ++i) {
                auto p = std::make_shared<Part>(); const auto po = parts[i].toObject(); bool a, b;
                p->first = po.value("first").toString().toLongLong(&a); p->last = po.value("last").toString().toLongLong(&b);
                if (!a || !b || p->first < 0 || (j->ranged && (p->last < p->first || p->last >= j->total)) || (!j->ranged && (parts.size() != 1 || p->last != -1))) { valid = false; break; }
                const QFileInfo fi(partPath(*j, i));
                if (QFileInfo(root + "/" + j->id).isSymLink() || fi.isSymLink() || (fi.exists() && !fi.isFile())) { valid = false; break; }
                p->bytes = fi.exists() ? fi.size() : 0;
                if (j->ranged && p->bytes > p->last - p->first + 1) { valid = false; break; }
                next = p->last + 1; j->parts.append(p);
            }
            if (j->ranged) {
                auto ordered=j->parts;std::sort(ordered.begin(),ordered.end(),[](const auto &a,const auto &b){return a->first<b->first;});next=0;
                for(const auto &p:ordered){if(p->first!=next){valid=false;break;}next=p->last+1;}
            }
            if (!valid || (j->ranged && !parts.isEmpty() && next != j->total)) {
                error = q->tr("Invalid partial download metadata. The queue was left untouched."); writable = false; jobs.clear(); return;
            }
            if (!j->ranged && !j->media && j->status != "complete" && (!httpUrl(url) || curlHttp(*j))) {
                auto p = std::make_shared<Part>();
                p->bytes = std::max<qint64>(0,o.value("mediaReceived").toString().toLongLong());
                j->parts = {p};
            }
            if (j->status == "complete") { j->parts.clear(); auto p = std::make_shared<Part>(); p->bytes = std::max<qint64>(0,j->total); j->parts.append(p); }
            jobs.append(j);
        }
    }
    bool curlHttp(const Job &j) const {
        return httpUrl(QUrl(j.url)) && (j.authMethod != "auto" || j.proxy.value("type").toString() == "https");
    }
    QNetworkRequest request(const Job &j) {
        QNetworkRequest r{QUrl(j.url)};
        r.setRawHeader("Accept-Encoding", "identity"); r.setRawHeader("User-Agent", "IDM-Native/0.1");
        for (auto it = j.headers.begin(); it != j.headers.end(); ++it) r.setRawHeader(it.key().toLatin1(),it.value().toString().toUtf8());
        r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        r.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        r.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, (!j.username.isEmpty() || !j.proxy.value("username").toString().isEmpty()) ? QNetworkRequest::Automatic : QNetworkRequest::Manual);
        r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        r.setTransferTimeout(j.rateLimitBytes > 0 ? 0 : j.timeoutSeconds*1000); return r;
    }
    QNetworkReply *get(const std::shared_ptr<Job> &j, const QNetworkRequest &request) {
        if (!j->access) {
            j->access = std::make_unique<QNetworkAccessManager>();
            if (!j->proxy.isEmpty()) {
                QNetworkProxy proxy(j->proxy.value("type").toString() == "socks5" ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                    j->proxy.value("host").toString(),quint16(j->proxy.value("port").toInt()),j->proxy.value("username").toString(),j->proxy.value("password").toString());
                j->access->setProxy(proxy);
            }
            const std::weak_ptr<Job> weak = j;
            QObject::connect(j->access.get(),&QNetworkAccessManager::authenticationRequired,q,[this,weak](QNetworkReply *reply,QAuthenticator *auth) {
                auto job = weak.lock(); if(!job)return;
                if (!job->username.isEmpty() && origin(reply->url()) == origin(QUrl(job->url)) && reply->property("idmAuthAttempts").toInt() < 2) { reply->setProperty("idmAuthAttempts",reply->property("idmAuthAttempts").toInt()+1);auth->setUser(job->username); auth->setPassword(job->password); }
                else fail(job,q->tr("Authentication failed or belongs to another origin."));
            });
            QObject::connect(j->access.get(),&QNetworkAccessManager::proxyAuthenticationRequired,q,[weak](const QNetworkProxy &,QAuthenticator *auth) {
                if(auto job=weak.lock()){ auth->setUser(job->proxy.value("username").toString());auth->setPassword(job->proxy.value("password").toString()); }
            });
        }
        auto reply = j->access->get(request);
        QObject::connect(reply,&QNetworkReply::metaDataChanged,q,[j,reply] { j->httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); });
        QObject::connect(reply,&QNetworkReply::redirected,q,[this,j,reply](const QUrl &target) {
            if (!httpUrl(target) || (QUrl(j->url).scheme() == "https" && target.scheme() != "https") ||
                ((!j->headers.isEmpty() || !j->username.isEmpty()) && origin(target) != origin(QUrl(j->url)))) {
                fail(j,q->tr("Redirect to a different origin was blocked to protect browser request credentials.")); return;
            }
            reply->redirectAllowed();
        });
        return reply;
    }
    bool identity(QNetworkReply *r) { const auto encoding = r->rawHeader("Content-Encoding").trimmed().toLower(); return encoding.isEmpty() || encoding == "identity"; }
    QByteArray validator(QNetworkReply *r, QByteArray &header) {
        auto value = r->rawHeader("ETag");
        if (!value.isEmpty() && !value.startsWith("W/") && value.startsWith('"') && value.endsWith('"')) { header = "ETag"; return value; }
        value = r->rawHeader("Last-Modified");
        const auto modified = QDateTime::fromString(QString::fromLatin1(value).replace(" GMT", " +0000"), Qt::RFC2822Date);
        const auto date = QDateTime::fromString(QString::fromLatin1(r->rawHeader("Date")).replace(" GMT", " +0000"), Qt::RFC2822Date);
        // A date validator is strong only when it predates the response by at least 60 seconds.
        if (modified.isValid() && date.isValid() && modified.secsTo(date) >= 60) { header = "Last-Modified"; return value; }
        header.clear(); return {};
    }
    bool openPart(const std::shared_ptr<Job> &j, int i, bool append) {
        auto &p = j->parts[i]; const QString path = partPath(*j,i);
        if (QFileInfo(root + "/" + j->id).isSymLink() || QFileInfo(path).isSymLink() || !QDir().mkpath(root + "/" + j->id)) { fail(j,q->tr("Unsafe partial download path.")); return false; }
        p->file = std::make_unique<QFile>(path);
        const QFileInfo existing(path);
        if (existing.exists() && (!append || existing.size() != p->bytes)) { fail(j,q->tr("Partial download size changed unexpectedly.")); return false; }
        auto mode = QIODevice::WriteOnly | (existing.exists() ? QIODevice::Append : QIODevice::NewOnly);
        if (!p->file->open(mode)) { fail(j,q->tr("Cannot open partial download: %1").arg(p->file->errorString())); return false; }
        if (!p->file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { fail(j,q->tr("Cannot make partial download private: %1").arg(p->file->errorString())); return false; }
        return true;
    }
    bool clearParts(const std::shared_ptr<Job> &j) {
        if (QFileInfo(root + "/" + j->id).isSymLink()) { fail(j,q->tr("Unsafe partial download path.")); return false; }
        for (int i = 0; i < j->parts.size(); ++i) {
            const auto path = partPath(*j,i);
            if (QFileInfo(path).isSymLink() || (QFileInfo::exists(path) && !QFile::remove(path))) { fail(j,q->tr("Cannot reset partial download.")); return false; }
        }
        j->parts.clear(); return true;
    }
    qint64 allowance(const std::shared_ptr<Job> &j) {
        if (j->rateLimitBytes <= 0) return 65536;
        if (!j->budgetClock.isValid()) j->budgetClock.start();
        j->tokens = std::min<double>(j->rateLimitBytes,j->tokens + j->budgetClock.restart()*j->rateLimitBytes/1000.0);
        return std::min<qint64>(65536,qint64(j->tokens));
    }
    void write(const std::shared_ptr<Job> &j, const std::shared_ptr<Part> &p, QNetworkReply *r) {
        while (r->bytesAvailable() > 0) {
            const auto allowed = allowance(j); if(allowed <= 0) return;
            const auto bytes = r->read(std::min<qint64>(allowed,r->bytesAvailable()));
            if(j->rateLimitBytes>0)j->tokens-=bytes.size();
            if (j->ranged && bytes.size() > p->last - p->first + 1 - p->bytes) { fail(j,q->tr("Server sent more bytes than the requested range.")); return; }
            const qint64 written = p->file->write(bytes);
            if (written > 0) { p->bytes += written; p->activity.restart(); }
            if (written != bytes.size()) { fail(j,q->tr("Cannot write download: %1").arg(p->file->errorString())); return; }
        }
    }
    void finish(const std::shared_ptr<Job> &j) {
        if (j->status != "downloading") return;
        for (auto &p : j->parts) if (p->reply) return;
        if (j->total >= 0 && received(*j) != j->total) { fail(j,q->tr("Downloaded size does not match the server's file size.")); return; }
        if (QFileInfo(root + "/" + j->id).isSymLink()) { fail(j,q->tr("Unsafe partial download path.")); return; }
        const QString staging = j->path + ".idm-native-" + j->id + ".tmp";
        QFile output(staging);
        // A crash can leave this transfer's private staging file; its source parts remain intact.
        if (QFileInfo(staging).isSymLink() || (output.exists() && !output.remove())) { fail(j,q->tr("Cannot reset completed-file staging area.")); return; }
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { fail(j,q->tr("Cannot create completed download: %1").arg(output.errorString())); return; }
        if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { const auto why = output.errorString(); output.close(); QFile::remove(staging); fail(j,q->tr("Cannot make completed download private: %1").arg(why)); return; }
        bool good = true;
        QVector<int> order;for(int i=0;i<j->parts.size();++i)order.append(i);
        if(j->ranged)std::sort(order.begin(),order.end(),[j](int a,int b){return j->parts[a]->first<j->parts[b]->first;});
        for (int i : order) {
            if(!good)break;
            QFile source(partPath(*j,i));
            if (QFileInfo(source.fileName()).isSymLink() || !source.open(QIODevice::ReadOnly) || source.size() != j->parts[i]->bytes) { good = false; break; }
            while (!source.atEnd()) {
                auto bytes = source.read(65536);
                if (bytes.isEmpty() && source.error() != QFileDevice::NoError) { good = false; break; }
                if (output.write(bytes) != bytes.size()) { good = false; break; }
            }
        }
        good = good && output.size() == received(*j) && output.flush() && ::fsync(output.handle()) == 0;
        output.close();
        // Linking a sibling staging file publishes atomically without replacing another file.
        if (!good || ::link(QFile::encodeName(staging).constData(), QFile::encodeName(j->path).constData()) != 0) {
            const QString why = good ? QString::fromLocal8Bit(std::strerror(errno)) : q->tr("disk read or write failed");
            QFile::remove(staging); fail(j,q->tr("Cannot publish completed file: %1").arg(why)); return;
        }
        QFile::remove(staging);
        j->total = received(*j); j->status = "complete"; j->error.clear();
        for (int i = 0; i < j->parts.size(); ++i) QFile::remove(partPath(*j,i));
        QDir(root + "/" + j->id).rmdir("."); save(); emit q->changed();
    }
    void launchRange(const std::shared_ptr<Job> &j, int index) {
        auto p = j->parts[index];
        if (p->bytes == p->last-p->first+1) return;
        if (!openPart(j,index,true)) return;
        auto req = request(*j); const qint64 first = p->first+p->bytes;
        req.setRawHeader("Range","bytes="+QByteArray::number(first)+"-"+QByteArray::number(p->last)); req.setRawHeader("If-Range",j->validator);
        auto r = get(j,req); p->reply=r; p->checked=false; p->activity.start();
        r->setReadBufferSize(j->rateLimitBytes > 0 ? std::max<qint64>(1,std::min<qint64>(65536,j->rateLimitBytes/j->connections)) : 65536);
        auto timer = new QTimer(q);p->timer=timer;timer->setInterval(20);
        auto consume = [this,j,p,r,first,timer] {
            if(j->status!="downloading" || p->reply!=r)return;
            if(r->isFinished() && r->error()!=QNetworkReply::NoError){fail(j,r->errorString());return;}
            if(!p->checked) {
                const int status=r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if(status==0 || (!r->isFinished() && ((status>=300 && status<400) || status==401 || status==407))) {
                    if(p->activity.elapsed()>j->timeoutSeconds*1000){fail(j,q->tr("The server did not respond before the transfer timeout."));} return;
                }
                const auto expected="bytes "+QByteArray::number(first)+"-"+QByteArray::number(p->last)+"/"+QByteArray::number(j->total);
                if(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=206 || r->rawHeader("Content-Range")!=expected || !identity(r) || r->rawHeader(j->validatorHeader)!=j->validator) {
                    fail(j,q->tr("Server changed the file or returned an invalid byte range."));return;
                }
                p->checked=true;
            }
            write(j,p,r);if(j->status!="downloading")return;
            if(r->bytesAvailable()>0)return;
            if(!r->isFinished()) {
                if(p->activity.elapsed()>j->timeoutSeconds*1000){fail(j,q->tr("The transfer stalled before completion."));} return;
            }
            p->file->close();p->file.reset();p->reply=nullptr;r->deleteLater();timer->stop();timer->deleteLater();p->timer=nullptr;
            if(p->bytes!=p->last-p->first+1){fail(j,q->tr("Server returned an incomplete byte range."));return;}
            stealRange(j);finish(j);
        };
        QObject::connect(r,&QIODevice::readyRead,q,consume);QObject::connect(r,&QNetworkReply::finished,q,consume);QObject::connect(timer,&QTimer::timeout,q,consume);timer->start();
    }
    void stealRange(const std::shared_ptr<Job> &j) {
        if(j->status!="downloading" || j->parts.size()>=16)return;
        int active=0,index=-1;qint64 largest=131071;
        for(int i=0;i<j->parts.size();++i){const auto &p=j->parts[i];if(p->reply)++active;const auto remaining=p->last-p->first+1-p->bytes;if(p->reply && p->checked && remaining>largest){largest=remaining;index=i;}}
        if(active>=j->connections || index<0)return;
        const auto p=j->parts[index];const auto last=p->last,first=p->first+p->bytes+largest/2;
        p->reply->disconnect(q);p->reply->abort();p->reply->deleteLater();p->reply=nullptr;p->file->close();p->file.reset();
        p->timer->stop();p->timer->deleteLater();p->timer=nullptr;
        p->last=first-1;auto stolen=std::make_shared<Part>();stolen->first=first;stolen->last=last;j->parts.append(stolen);
        if(!save()){fail(j,error);return;}
        launchRange(j,index);if(j->status=="downloading")launchRange(j,j->parts.size()-1);emit q->changed();
    }
    void startRanges(const std::shared_ptr<Job> &j) {
        j->status="downloading";j->baseline=received(*j);j->clock.start();j->budgetClock.start();j->tokens=0;
        if(!save()){fail(j,error);return;}
        for(int i=0;i<j->parts.size() && j->status=="downloading";++i)launchRange(j,i);
        finish(j);emit q->changed();
    }
    void drainSerial(const std::shared_ptr<Job> &j, const std::shared_ptr<Part> &p, QNetworkReply *r, QTimer *timer) {
        if(j->status!="downloading" || j->probe!=r)return;
        if(r->isFinished() && r->error()!=QNetworkReply::NoError){fail(j,r->errorString());return;}
        write(j,p,r);if(j->status!="downloading")return;
        if(r->bytesAvailable()>0)return;
        if(!r->isFinished()) {
            if(p->activity.elapsed()>j->timeoutSeconds*1000){fail(j,q->tr("The transfer stalled before completion."));} return;
        }
        p->file->close();p->file.reset();j->probe=nullptr;r->deleteLater();timer->stop();timer->deleteLater();j->probeTimer=nullptr;finish(j);
    }
    void startWorker(const std::shared_ptr<Job> &j) {
        const bool transfer=!j->media;
        if (!QFileInfo::exists("/usr/bin/python3") || ((transfer || j->authMethod != "auto" || QStringList{"socks5","https"}.contains(j->proxy.value("type").toString())) && QStandardPaths::findExecutable("curl").isEmpty()) || (!transfer && (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()))) { fail(j,q->tr("A required free transfer tool is missing. Install Python, curl, yt-dlp and FFmpeg."));return; }
        const QString directory = root + "/" + j->id, work = directory + (transfer?"/transfer":"/media");
        if (QFileInfo(directory).isSymLink() || QFileInfo(work).isSymLink()) { fail(j,q->tr("Unsafe media working directory.")); return; }
        // Fragment files lack a common strong validator, so a paused capture restarts safely.
        if (!transfer && QFileInfo::exists(work) && !QDir(work).removeRecursively()) { fail(j,q->tr("Cannot reset media working directory.")); return; }
        if (!QDir().mkpath(work) || !QFile::setPermissions(work,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner)) { fail(j,q->tr("Cannot create private media working directory.")); return; }
        QFile source(":/mediaworker.py"); const QString helper = root + "/mediaworker.py"; QSaveFile worker(helper);
        if (QFileInfo(helper).isSymLink() || !source.open(QIODevice::ReadOnly) || !worker.open(QIODevice::WriteOnly) || !worker.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)) { fail(j,q->tr("Cannot prepare private media worker.")); return; }
        const auto script = source.readAll();
        if (worker.write(script) != script.size() || !worker.commit()) { fail(j,q->tr("Cannot save private media worker.")); return; }
        if (j->ranged && received(*j)>0) { fail(j,q->tr("Existing HTTP range parts need their original transport. Start a new download to change authentication transport.")); return; }
        j->ranged = false; j->validator.clear(); j->validatorHeader.clear();
        const auto previousBytes = transfer ? received(*j) : 0;
        j->parts = {std::make_shared<Part>()}; j->parts[0]->bytes = previousBytes; j->total = -1; j->baseline = previousBytes; j->mediaSpeed = 0; j->mediaEta = -1;
        j->error.clear(); j->status = "downloading"; j->clock.start(); j->processOutput.clear();
        if (!save()) { fail(j,error); return; }
        auto process = new QProcess(q); j->process = process;
        auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("PYTHONHOME"); environment.remove("PYTHONPATH"); environment.insert("PYTHONNOUSERSITE","1"); environment.insert("PYTHONSAFEPATH","1");
        process->setProcessEnvironment(environment); process->setWorkingDirectory(work);
        process->setStandardErrorFile(QProcess::nullDevice());
        QObject::connect(process,&QProcess::readyReadStandardOutput,q,[this,j,process,work,transfer] {
            j->processOutput += process->readAllStandardOutput();
            if (j->processOutput.size() > 1024*1024) { fail(j,q->tr("Media worker returned too much output.")); return; }
            while (j->processOutput.contains('\n')) {
                if (j->process != process || j->status != "downloading") return;
                const auto end = j->processOutput.indexOf('\n'); const auto line = j->processOutput.left(end); j->processOutput.remove(0,end+1);
                QJsonParseError parse; const auto frame = QJsonDocument::fromJson(line,&parse).object();
                if (parse.error != QJsonParseError::NoError) { fail(j,q->tr("Media worker returned invalid progress.")); return; }
                const auto type = frame.value("type").toString();
                if (type == "error") { fail(j,frame.value("error").toString()); return; }
                if (type == "metadata") {
                    j->workerResumable = frame.value("resumable").toBool();
                    j->httpStatus = frame.value("httpStatus").toInt();
                    emit q->changed();
                } else if (type == "progress") {
                    j->parts[0]->bytes = std::max<qint64>(0,frame.value("received").toVariant().toLongLong());
                    j->total = frame.value("total").toVariant().toLongLong(); if(j->total < received(*j)) j->total = -1;
                    j->mediaSpeed = std::max(0.0,frame.value("speed").toDouble()); j->mediaEta = frame.value("eta").toVariant().toLongLong(); emit q->changed();
                } else if (type == "complete") {
                    const QString path = frame.value("path").toString(), expected = transfer ? work + "/verified.bin" : work + "/verified." + QFileInfo(j->path).suffix().toLower();
                    const auto streams = frame.value("streams").toArray(); bool playable = false;
                    for (auto stream : streams) if (stream.toString() == "audio" || stream.toString() == "video") playable = true;
                    const QFileInfo info(path);
                    if (path != expected || QFileInfo(work).isSymLink() || info.isSymLink() || !info.isFile() || info.size() < 0 || (!transfer && (info.size() == 0 || frame.value("duration").toDouble() <= 0 || !playable))) { fail(j,q->tr("Media worker did not produce verified playable output.")); return; }
                    const QString part = partPath(*j,0);
                    if (QFileInfo(part).isSymLink() || (QFileInfo::exists(part) && !QFile::remove(part)) || !QFile::rename(path,part)) { fail(j,q->tr("Cannot preserve verified media output.")); return; }
                    j->parts[0]->bytes = info.size(); j->total = info.size(); finish(j);
                    if (j->status == "complete") QDir(work).removeRecursively();
                }
            }
        });
        QObject::connect(process,&QProcess::errorOccurred,q,[this,j](QProcess::ProcessError) { if(j->status == "downloading") fail(j,q->tr("Cannot run the free media worker. Check Python, yt-dlp and FFmpeg installation.")); });
        QObject::connect(process,&QProcess::finished,q,[this,j,process](int,QProcess::ExitStatus) {
            if (j->process != process) return;
            j->process = nullptr; process->deleteLater();
            if (j->status == "downloading") fail(j,q->tr("Media worker stopped before producing verified output."));
        });
        const auto config = QJsonObject{{"parentPid",qint64(QCoreApplication::applicationPid())},{"kind",transfer?(httpUrl(QUrl(j->url))?"http":"ftp"):"media"},{"curl",QStandardPaths::findExecutable("curl")},{"authMethod",j->authMethod},{"username",j->username},{"password",j->password},
                                       {"proxy",QJsonObject::fromVariantMap(j->proxy)},{"rateLimitBytes",QString::number(j->rateLimitBytes)},{"timeoutSeconds",j->timeoutSeconds},
                                       {"url",j->url},{"headers",QJsonObject::fromVariantMap(j->headers)},{"directory",work},{"connections",j->connections},
                                       {"format",j->format},{"extension",QFileInfo(j->path).suffix().toLower()},
                                       {"ffmpeg",QStandardPaths::findExecutable("ffmpeg")},{"ffprobe",QStandardPaths::findExecutable("ffprobe")}};
        process->start("/usr/bin/python3",{helper}); process->write(QJsonDocument(config).toJson(QJsonDocument::Compact)); process->closeWriteChannel(); emit q->changed();
    }
    void start(const std::shared_ptr<Job> &j) {
        if (!writable) return;
        if (j->status == "complete" || j->status == "downloading" || j->status == "probing") return;
        if (QFileInfo::exists(j->path) || QFileInfo(j->path).isSymLink()) { fail(j,q->tr("Destination already exists. Choose a different filename.")); return; }
        if (j->media || !httpUrl(QUrl(j->url)) || curlHttp(*j)) { startWorker(j); return; }
        if (j->ranged && received(*j)>0 && j->validatorHeader=="Last-Modified" && !j->dateValidatorStrong) { fail(j,q->tr("Old date-validated partials cannot be verified safely. Start a new download.")); return; }
        if (QFileInfo::exists(root+"/"+j->id+"/transfer/resume.json")) { fail(j,q->tr("Saved worker partials need their original transport. Start a new download to change authentication transport.")); return; }
        j->status = "probing"; j->error.clear(); j->clock.start(); j->budgetClock.start();j->tokens=0; save(); emit q->changed();
        auto req = request(*j); req.setRawHeader("Range", "bytes=0-0");
        auto r = get(j,req); j->probe = r; r->setReadBufferSize(65536);
        auto probeBody = std::make_shared<QByteArray>();
        auto serial = std::make_shared<bool>(false);
        auto timer=new QTimer(q);j->probeTimer=timer;timer->setInterval(20);
        auto consume = [this,j,r,probeBody,serial,timer] {
            if(j->probe!=r)return;
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200 && !*serial) {
                if (!identity(r)) { fail(j,q->tr("Server ignored identity transfer encoding.")); return; }
                if (j->ranged && received(*j) > 0) { fail(j,q->tr("Server no longer supports resuming this file. Remove it and start again.")); return; }
                if (!clearParts(j)) return;
                j->ranged = false; j->validator.clear(); j->validatorHeader.clear();
                bool ok; auto size = r->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok); j->total = ok ? size : -1;
                auto p = std::make_shared<Part>(); j->parts.append(p);
                if (!openPart(j,0,false)) return;
                j->status = "downloading"; j->baseline = 0; j->clock.start(); p->activity.start(); *serial = true;
                if (!save()) { fail(j,error); return; }
                emit q->changed();
            }
            if (*serial) drainSerial(j,j->parts[0],r,timer);
            else {
                probeBody->append(r->read(2));
                if (probeBody->size() > 1 || r->bytesAvailable() > 0) fail(j,q->tr("Invalid byte-range probe response."));
            }
        };
        QObject::connect(r,&QIODevice::readyRead,q,consume);
        QObject::connect(timer,&QTimer::timeout,q,[this,j,r,consume] {
            if(j->probe!=r)return;
            if(j->status=="probing" && j->clock.elapsed()>j->timeoutSeconds*1000){fail(j,q->tr("The server did not respond before the transfer timeout."));return;}
            consume();
        });timer->start();
        QObject::connect(r,&QNetworkReply::finished,q,[this,j,r,probeBody,serial,consume,timer] {
            if (j->probe != r) return;
            if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 416 && r->rawHeader("Content-Range") == "bytes */0") {
                if (received(*j) > 0) { fail(j,q->tr("The remote file changed. Existing partial data was preserved.")); return; }
                if (!clearParts(j)) return;
                j->ranged = false; j->total = 0; j->status = "downloading";
                j->parts.append(std::make_shared<Part>());
                if (!openPart(j,0,false)) return;
                j->parts[0]->file->close(); j->parts[0]->file.reset(); j->probe = nullptr; r->deleteLater(); timer->stop();timer->deleteLater();j->probeTimer=nullptr;finish(j); return;
            }
            if (r->error() != QNetworkReply::NoError) { fail(j,r->errorString()); return; }
            consume(); if (j->probe != r || *serial) return;
            j->probe = nullptr;timer->stop();timer->deleteLater();j->probeTimer=nullptr;
            QRegularExpression expression("^bytes 0-0/([1-9][0-9]*)$");
            auto match = expression.match(QString::fromLatin1(r->rawHeader("Content-Range")));
            bool ok = false; const qint64 total = match.captured(1).toLongLong(&ok);
            QByteArray header; const auto value = validator(r,header);
            if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206 || !match.hasMatch() || !ok || probeBody->size() != 1 || !identity(r)) {
                r->deleteLater(); fail(j,q->tr("Server returned an invalid byte-range probe.")); return;
            }
            r->deleteLater();
            const bool metadataChanged = j->total != total || j->validator != value || j->validatorHeader != header;
            if (j->ranged && received(*j)>0 && metadataChanged) { fail(j,q->tr("The remote file changed. Existing partial data was preserved.")); return; }
            if (value.isEmpty()) {
                if (j->ranged && received(*j)>0) { fail(j,q->tr("Server no longer provides a resume validator.")); return; }
                if (!clearParts(j)) return;
                j->status = "paused";
                startSerial(j); return;
            }
            if (!j->ranged || j->parts.isEmpty() || metadataChanged) {
                if (!clearParts(j)) return;
                const int count = int(std::min<qint64>(j->connections,total));
                for (int i = 0; i < count; ++i) { auto p = std::make_shared<Part>(); p->first = total / count * i; p->last = i == count-1 ? total-1 : total/count*(i+1)-1; j->parts.append(p); }
            }
            j->ranged = true; j->total = total; j->validator = value; j->validatorHeader = header; j->dateValidatorStrong = header=="Last-Modified"; startRanges(j);
        });
    }
    void startSerial(const std::shared_ptr<Job> &j) {
        auto req=request(*j);auto r=get(j,req);j->probe=r;
        auto p=std::make_shared<Part>();j->parts={p};if(!openPart(j,0,false))return;
        j->ranged=false;j->total=-1;j->status="downloading";j->baseline=0;j->clock.start();p->activity.start();
        r->setReadBufferSize(j->rateLimitBytes>0?std::max<qint64>(1,std::min<qint64>(65536,j->rateLimitBytes)):65536);
        auto timer=new QTimer(q);j->probeTimer=timer;timer->setInterval(20);
        auto consume=[this,j,p,r,timer] {
            if(j->probe!=r || j->status!="downloading")return;
            if(!p->checked) {
                const int status=r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if(status==0 || (!r->isFinished() && ((status>=300 && status<400) || status==401 || status==407))) {
                    if(p->activity.elapsed()>j->timeoutSeconds*1000){fail(j,q->tr("The server did not respond before the transfer timeout."));} return;
                }
                if(status!=200 || !identity(r)){fail(j,q->tr("Server returned an invalid download response."));return;}
                bool ok;auto total=r->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);j->total=ok?total:-1;p->checked=true;
            }
            drainSerial(j,p,r,timer);
        };
        QObject::connect(r,&QIODevice::readyRead,q,consume);QObject::connect(r,&QNetworkReply::finished,q,consume);QObject::connect(timer,&QTimer::timeout,q,consume);timer->start();
        save();emit q->changed();
    }

};

DownloadManager::DownloadManager(const QString &stateDirectory, QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this,stateDirectory)) {}
DownloadManager::~DownloadManager() = default;
QVariantList DownloadManager::downloads() const {
    QVariantList result;
    for (const auto &j : d->jobs) {
        const auto bytes = d->received(*j);
        const double speed = j->media ? j->mediaSpeed : (j->status == "downloading" && j->clock.isValid() ? (bytes-j->baseline)*1000.0/std::max<qint64>(1,j->clock.elapsed()) : 0);
        QVariantList parts;
        for (const auto &p : j->parts) parts.append(QVariantMap{{"received",p->bytes},{"total",j->ranged ? p->last-p->first+1 : j->total}});
        const qint64 eta = j->status != "downloading" ? -1 : j->media ? j->mediaEta : (speed > 0 && j->total >= bytes ? qint64((j->total-bytes)/speed) : -1);
        result.append(QVariantMap{{"id",j->id},{"url",j->url},{"path",j->path},{"name",QFileInfo(j->path).fileName()},{"status",j->status},{"received",bytes},{"total",j->total},
                     {"speed",j->status == "downloading" ? speed : 0},{"connections",j->ranged ? j->connections : 1},{"error",j->error},{"media",j->media},
                     {"category",category(j->path,j->media)},{"created",j->created},{"resumable",!j->media && ((j->ranged && !j->validator.isEmpty() && (j->validatorHeader!="Last-Modified" || j->dateValidatorStrong)) || j->workerResumable)},{"authMethod",j->authMethod},
                     {"parts",parts},{"eta",eta},{"httpStatus",j->httpStatus},{"rateLimitBytes",j->rateLimitBytes},{"timeoutSeconds",j->timeoutSeconds},{"description",j->description},{"title",j->title},{"format",j->format}});
    }
    return result;
}
QString DownloadManager::error() const { return d->error; }
QString DownloadManager::downloadDirectory() const { return d->destination; }
QString DownloadManager::add(const QString &input, const QString &directory, int connections) {
    return addRequest({{"url",input},{"directory",directory},{"connections",connections}});
}
QString DownloadManager::addRequest(const QVariantMap &request) {
    auto reject = [this](const QString &why) { d->error=why; emit changed(); return QString(); };
    if (!d->writable) return reject(d->error);
    QUrl url(request.value("url").toString().trimmed(),QUrl::StrictMode);
    if (!downloadUrl(url)) return reject(tr("Enter an HTTP, HTTPS, FTP or FTPS URL without embedded credentials."));
    QString advancedError;if(!advancedRequest(request,advancedError))return reject(advancedError);
    if (request.value("method","GET").toString().compare("GET",Qt::CaseInsensitive) != 0) return reject(tr("Only GET browser requests are supported."));
    if (request.contains("headers") && request.value("headers").metaType().id() != QMetaType::QVariantMap) return reject(tr("Request headers must be a map of strings."));
    QVariantMap headers; QString why;
    if (!requestHeaders(request.value("headers").toMap(),headers,why)) return reject(why);
    const QString pageUrl = request.value("pageUrl").toString();
    if (!pageUrl.isEmpty() && !httpUrl(QUrl(pageUrl,QUrl::StrictMode))) return reject(tr("Invalid browser page URL."));
    const bool media = request.value("media").toBool();
    if(!httpUrl(url) && request.value("authMethod","auto").toString()!="auto")return reject(tr("HTTP authentication methods do not apply to FTP. Use its username and password."));
    if(!httpUrl(url) && (media || !headers.isEmpty()))return reject(tr("FTP requests do not support browser headers or media capture."));
    const auto format = request.value("format","best").toString();
    if (media && !QStringList{"best","1080p","720p","audio"}.contains(format)) return reject(tr("Choose Best, 1080p, 720p, or Audio quality."));
    if (media && (!QFileInfo::exists("/usr/bin/python3") || QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty())) return reject(tr("Media capture needs the free Python, yt-dlp and FFmpeg packages."));
    if (!httpUrl(url) && QStandardPaths::findExecutable("curl").isEmpty())return reject(tr("FTP transfers need the free curl package."));
    if ((media || !httpUrl(url)) && !QFile::exists(":/mediaworker.py")) return reject(tr("This build is missing its media worker resource."));
    QString name = request.value("filename").toString();
    if (name.isEmpty()) name = media && !request.value("title").toString().isEmpty() ? request.value("title").toString() : QFileInfo(url.path()).fileName();
    name.replace(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\]"),"_");
    if (name.isEmpty() || name=="." || name=="..") name="download";
    while(name.toUtf8().size()>160) name.chop(1);
    if (media) {
        const auto extension = QFileInfo(name).suffix().toLower();
        if (format == "audio") name = QFileInfo(name).completeBaseName() + ".mka";
        else if (!QStringList{"mp4","mkv","webm","m4a","mp3","ogg","opus","wav","flac","aac"}.contains(extension)) name = QFileInfo(name).completeBaseName() + ".mkv";
    }
    QString dir = request.value("directory").toString(); if(dir.isEmpty()) dir=d->destination;
    if (dir.startsWith("file:")) dir=QUrl(dir).toLocalFile();
    if (!QFileInfo(dir).isAbsolute() || !QDir().mkpath(dir)) return reject(tr("Choose a writable absolute download directory."));
    QString path = QDir(dir).absoluteFilePath(name);
    auto occupied = [this](const QString &path) { if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return true; for (auto &j:d->jobs) if(j->path==path) return true; return false; };
    const QString original = path;
    for (int i=1; occupied(path); ++i) { if(i>10000) return reject(tr("Cannot choose an unused destination filename.")); path=media ? QDir(dir).absoluteFilePath(QFileInfo(original).completeBaseName()+"."+QString::number(i)+"."+QFileInfo(original).suffix()) : original+"."+QString::number(i); }
    auto j=std::make_shared<Impl::Job>(); j->id=QUuid::createUuid().toString(QUuid::Id128); j->url=url.toString(QUrl::FullyEncoded); j->path=path; j->connections=std::clamp(request.value("connections",4).toInt(),1,16);
    j->rateLimitBytes=request.value("rateLimitBytes",0).toLongLong();j->timeoutSeconds=request.value("timeoutSeconds",30).toInt();j->authMethod=request.value("authMethod","auto").toString();j->username=request.value("username").toString();j->password=request.value("password").toString();j->proxy=request.value("proxy").toMap();
    j->headers=headers; j->media=media; j->pageUrl=pageUrl; j->title=request.value("title").toString().left(4096); j->description=request.value("description").toString().left(4096); j->format=format;
    d->jobs.append(j); d->error.clear();
    if (!d->save()) { d->jobs.removeLast(); return {}; }
    if (request.value("start",true).toBool() && !request.value("paused",false).toBool()) d->start(j); else emit changed();
    return j->id;
}
void DownloadManager::pause(const QString &id) { if(auto j=d->find(id)) { d->stop(j); d->save(); emit changed(); } }
void DownloadManager::resume(const QString &id) { if(auto j=d->find(id)) d->start(j); }
void DownloadManager::remove(const QString &id) {
    auto j=d->find(id); if(!j || !d->writable) return;
    d->stop(j);
    if (QFileInfo(d->root+"/"+j->id).isSymLink()) { d->fail(j,tr("Unsafe partial download path.")); return; }
    if(j->status!="complete") for(int i=0;i<j->parts.size();++i) QFile::remove(d->partPath(*j,i));
    if (j->media) QDir(d->root+"/"+j->id+"/media").removeRecursively();
    if(!j->media)QDir(d->root+"/"+j->id+"/transfer").removeRecursively();
    QDir(d->root+"/"+j->id).rmdir("."); d->jobs.removeOne(j); d->save(); emit changed();
}
void DownloadManager::pauseAll() { for(auto &j:d->jobs) d->stop(j); d->save(); emit changed(); }

void DownloadManager::resumeAll() { for(auto &j:d->jobs) if(j->status == "paused" || j->status == "error") d->start(j); }
void DownloadManager::removeCompleted() { QStringList ids; for(auto &j:d->jobs) if(j->status == "complete") ids.append(j->id); for(const auto &id:ids) remove(id); }

void DownloadManager::setRateLimit(const QString &id, qint64 bytesPerSecond) {
    auto j=d->find(id);if(!j || !d->writable || j->rateLimitBytes==bytesPerSecond)return;
    if(bytesPerSecond<0 || bytesPerSecond>1000000000000LL){d->error=tr("Invalid speed limit.");emit changed();return;}
    const bool active=j->status=="downloading" || j->status=="probing";if(active)d->stop(j);
    j->rateLimitBytes=bytesPerSecond;j->tokens=0;j->budgetClock.restart();d->save();if(active)d->start(j);emit changed();
}
bool DownloadManager::rename(const QString &id, const QString &filename) {
    auto reject=[this](const QString &why){d->error=why;emit changed();return false;};auto j=d->find(id);if(!j || !d->writable)return reject(tr("Download not found or queue is unavailable."));
    if(filename.isEmpty() || filename=="." || filename==".." || filename.toUtf8().size()>180 || filename.contains(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\]")))return reject(tr("Enter a filename without directory separators or control characters."));
    if(j->media && QFileInfo(filename).suffix().compare(QFileInfo(j->path).suffix(),Qt::CaseInsensitive)!=0)return reject(tr("Keep the media container extension when renaming."));
    const QString path=QFileInfo(j->path).absoluteDir().absoluteFilePath(filename);if(path==j->path)return true;
    for(const auto &other:d->jobs)if(other!=j && other->path==path)return reject(tr("Another download already uses that filename."));
    if(QFileInfo::exists(path) || QFileInfo(path).isSymLink())return reject(tr("Destination already exists."));
    const bool active=j->status=="downloading" || j->status=="probing";if(active)d->stop(j);
    const auto old=j->path;
    if(j->status=="complete" && (QFileInfo(old).isSymLink() || !QFile::rename(old,path)))return reject(tr("Cannot rename completed download."));
    j->path=path;
    if(!d->save()){j->path=old;if(j->status=="complete")QFile::rename(path,old);return false;}
    d->error.clear();if(active)d->start(j);emit changed();return true;
}
bool DownloadManager::refresh(const QString &id, const QString &input) {
    auto reject=[this](const QString &why){d->error=why;emit changed();return false;};auto j=d->find(id);if(!j || !d->writable)return reject(tr("Download not found or queue is unavailable."));
    if(j->status=="complete")return reject(tr("This download is already complete."));
    const QUrl url(input.trimmed(),QUrl::StrictMode),old(j->url);if(!downloadUrl(url) || (j->media && !httpUrl(url)))return reject(tr("Enter a supported URL without embedded credentials."));
    if(((j->ranged && d->received(*j)>0) || QFileInfo::exists(d->root+"/"+j->id+"/transfer/resume.json")) && (origin(old)!=origin(url) || old.path()!=url.path()))return reject(tr("A different resource cannot reuse saved bytes. Start it as a new download."));
    const bool active=j->status=="downloading" || j->status=="probing" || j->status=="error";d->stop(j);
    if(origin(old)!=origin(url)){j->headers.clear();j->username.clear();j->password.clear();j->access.reset();}
    j->url=url.toString(QUrl::FullyEncoded);j->error.clear();j->status="paused";if(!d->save())return false;
    d->error.clear();if(active)d->start(j);emit changed();return true;
}

bool DownloadManager::updateContext(const QString &id, const QVariantMap &request) {
    auto reject=[this](const QString &why){d->error=why;emit changed();return false;};
    auto j=d->find(id);if(!j || !d->writable)return reject(tr("Download not found or queue is unavailable."));
    if(j->status!="paused" && j->status!="error")return reject(tr("Pause the unfinished download before updating its request context."));
    if(request.contains("method") && request.value("method").toString().compare("GET",Qt::CaseInsensitive)!=0)return reject(tr("Only GET browser requests are supported."));
    if(request.contains("url")) {
        if(request.value("url").metaType().id()!=QMetaType::QString)return reject(tr("Request context needs a valid download URL."));
        const QUrl next(request.value("url").toString(),QUrl::StrictMode),current(j->url);
        if(!downloadUrl(next) || next.adjusted(QUrl::RemoveFragment)!=current.adjusted(QUrl::RemoveFragment))return reject(tr("Request context must belong to the same download URL. Refresh its address separately."));
    }
    auto headers=j->headers;
    if(request.contains("headers")) {
        if(request.value("headers").metaType().id()!=QMetaType::QVariantMap)return reject(tr("Request headers must be a map of strings."));
        headers.clear();QString why;if(!requestHeaders(request.value("headers").toMap(),headers,why))return reject(why);
    }
    if(!httpUrl(QUrl(j->url)) && !headers.isEmpty())return reject(tr("FTP requests do not support browser headers."));
    QString username=j->username,password=j->password,pageUrl=j->pageUrl,authMethod=j->authMethod;
    for(const auto &key:QStringList{"username","password","pageUrl","authMethod"})if(request.contains(key) && request.value(key).metaType().id()!=QMetaType::QString)return reject(tr("Credentials and browser page URL must be strings."));
    if(request.contains("authMethod"))authMethod=request.value("authMethod").toString();
    if(request.contains("username"))username=request.value("username").toString();
    if(request.contains("password"))password=request.value("password").toString();
    if(request.contains("pageUrl"))pageUrl=request.value("pageUrl").toString();
    QString why;if(!advancedRequest({{"authMethod",authMethod},{"username",username},{"password",password}},why))return reject(why);
    if(!httpUrl(QUrl(j->url)) && authMethod!="auto")return reject(tr("HTTP authentication methods do not apply to FTP. Use its username and password."));
    if(!pageUrl.isEmpty() && !httpUrl(QUrl(pageUrl,QUrl::StrictMode)))return reject(tr("Invalid browser page URL."));
    const auto oldHeaders=j->headers;const auto oldUsername=j->username,oldPassword=j->password,oldPageUrl=j->pageUrl,oldAuthMethod=j->authMethod;
    j->authMethod=authMethod;j->headers=headers;j->username=username;j->password=password;j->pageUrl=pageUrl;
    if(!d->save()){j->authMethod=oldAuthMethod;j->headers=oldHeaders;j->username=oldUsername;j->password=oldPassword;j->pageUrl=oldPageUrl;return false;}
    j->access.reset();d->error.clear();emit changed();return true;
}
