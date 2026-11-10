#include "downloadmanager.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
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

struct DownloadManager::Impl {
    struct Part {
        qint64 first = 0, last = -1, bytes = 0;
        std::unique_ptr<QFile> file;
        QPointer<QNetworkReply> reply;
        bool checked = false;
    };
    struct Job {
        QString id, url, path, status = "paused", error;
        int connections = 4;
        qint64 total = -1, baseline = 0;
        QByteArray validator, validatorHeader;
        bool ranged = false;
        QElapsedTimer clock;
        QVector<std::shared_ptr<Part>> parts;
        QPointer<QNetworkReply> probe;
    };
    DownloadManager *q;
    QString root, destination, error;
    QNetworkAccessManager network;
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
        if (j->probe) { j->probe->disconnect(q); j->probe->abort(); j->probe->deleteLater(); j->probe = nullptr; }
        for (auto &p : j->parts) {
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
            if (j->status != "complete") for (auto &p : j->parts) parts.append(QJsonObject{{"first", QString::number(p->first)}, {"last", QString::number(p->last)}});
            rows.append(QJsonObject{{"id", j->id}, {"url", j->url}, {"path", j->path}, {"status", j->status}, {"error", j->error},
                        {"connections", j->connections}, {"total", QString::number(j->total)}, {"ranged", j->ranged},
                        {"validator", QString::fromLatin1(j->validator.toBase64())}, {"validatorHeader", QString::fromLatin1(j->validatorHeader)}, {"parts", parts}});
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
                !url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty() || !QFileInfo(j->path).isAbsolute()) {
                error = q->tr("Invalid entry in download queue. The saved file was left untouched."); writable = false; jobs.clear(); return;
            }
            j->connections = std::clamp(o.value("connections").toInt(4), 1, 16);
            j->validator = QByteArray::fromBase64(o.value("validator").toString().toLatin1());
            j->validatorHeader = o.value("validatorHeader").toString().toLatin1();
            j->ranged = o.value("ranged").toBool();
            j->status = o.value("status").toString() == "complete" ? "complete" : "paused";
            j->error = o.value("error").toString();
            qint64 next = 0; bool valid = true;
            const auto parts = o.value("parts").toArray();
            if (parts.size() > 16 || (j->ranged && (j->total <= 0 || j->validator.isEmpty() ||
                (j->validatorHeader != "ETag" && j->validatorHeader != "Last-Modified")))) valid = false;
            for (int i = 0; i < parts.size() && valid; ++i) {
                auto p = std::make_shared<Part>(); const auto po = parts[i].toObject(); bool a, b;
                p->first = po.value("first").toString().toLongLong(&a); p->last = po.value("last").toString().toLongLong(&b);
                if (!a || !b || p->first != next || (j->ranged && (p->last < p->first || p->last >= j->total)) || (!j->ranged && (parts.size() != 1 || p->last != -1))) { valid = false; break; }
                const QFileInfo fi(partPath(*j, i));
                if (QFileInfo(root + "/" + j->id).isSymLink() || fi.isSymLink() || (fi.exists() && !fi.isFile())) { valid = false; break; }
                p->bytes = fi.exists() ? fi.size() : 0;
                if (j->ranged && p->bytes > p->last - p->first + 1) { valid = false; break; }
                next = p->last + 1; j->parts.append(p);
            }
            if (!valid || (j->ranged && !parts.isEmpty() && next != j->total)) {
                error = q->tr("Invalid partial download metadata. The queue was left untouched."); writable = false; jobs.clear(); return;
            }
            if (j->status == "complete") { j->parts.clear(); auto p = std::make_shared<Part>(); p->bytes = std::max<qint64>(0,j->total); j->parts.append(p); }
            jobs.append(j);
        }
    }
    QNetworkRequest request(const QString &url) {
        QNetworkRequest r{QUrl(url)};
        r.setRawHeader("Accept-Encoding", "identity"); r.setRawHeader("User-Agent", "IDM-Native/0.1");
        r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        r.setTransferTimeout(30000); return r;
    }
    bool identity(QNetworkReply *r) { const auto encoding = r->rawHeader("Content-Encoding").trimmed().toLower(); return encoding.isEmpty() || encoding == "identity"; }
    QByteArray validator(QNetworkReply *r, QByteArray &header) {
        auto value = r->rawHeader("ETag");
        if (!value.isEmpty() && !value.startsWith("W/") && value.startsWith('"') && value.endsWith('"')) { header = "ETag"; return value; }
        value = r->rawHeader("Last-Modified");
        if (!value.isEmpty()) { header = "Last-Modified"; return value; }
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
    void write(const std::shared_ptr<Job> &j, const std::shared_ptr<Part> &p, QNetworkReply *r) {
        while (r->bytesAvailable() > 0) {
            const auto bytes = r->read(65536);
            if (j->ranged && bytes.size() > p->last - p->first + 1 - p->bytes) { fail(j,q->tr("Server sent more bytes than the requested range.")); return; }
            const qint64 written = p->file->write(bytes);
            if (written > 0) p->bytes += written;
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
        for (int i = 0; i < j->parts.size() && good; ++i) {
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
    void startRanges(const std::shared_ptr<Job> &j) {
        j->status = "downloading"; j->baseline = received(*j); j->clock.start();
        if (!save()) { fail(j,error); return; }
        for (int i = 0; i < j->parts.size(); ++i) {
            auto p = j->parts[i];
            if (p->bytes == p->last - p->first + 1) continue;
            if (!openPart(j,i,true)) return;
            auto req = request(j->url); const qint64 first = p->first + p->bytes;
            req.setRawHeader("Range", "bytes=" + QByteArray::number(first) + "-" + QByteArray::number(p->last));
            req.setRawHeader("If-Range", j->validator);
            auto r = network.get(req); p->reply = r; p->checked = false;
            r->setReadBufferSize(65536);
            auto consume = [this,j,p,r,first] {
                if (!p->checked) {
                    const auto range = r->rawHeader("Content-Range");
                    const auto expected = "bytes " + QByteArray::number(first) + "-" + QByteArray::number(p->last) + "/" + QByteArray::number(j->total);
                    if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206 || range != expected || !identity(r) || r->rawHeader(j->validatorHeader) != j->validator) {
                        fail(j,q->tr("Server changed the file or returned an invalid byte range.")); return;
                    }
                    p->checked = true;
                }
                write(j,p,r);
            };
            QObject::connect(r,&QIODevice::readyRead,q,consume);
            QObject::connect(r,&QNetworkReply::finished,q,[this,j,p,r,consume] {
                if (j->status != "downloading" || p->reply != r) return;
                if (r->error() != QNetworkReply::NoError) { fail(j,r->errorString()); return; }
                consume(); if (j->status != "downloading") return;
                p->file->close(); p->file.reset(); p->reply = nullptr; r->deleteLater();
                if (p->bytes != p->last-p->first+1) { fail(j,q->tr("Server returned an incomplete byte range.")); return; }
                finish(j);
            });
        }
        finish(j); emit q->changed();
    }
    void start(const std::shared_ptr<Job> &j) {
        if (!writable) return;
        if (j->status == "complete" || j->status == "downloading" || j->status == "probing") return;
        if (QFileInfo::exists(j->path) || QFileInfo(j->path).isSymLink()) { fail(j,q->tr("Destination already exists. Choose a different filename.")); return; }
        j->status = "probing"; j->error.clear(); save(); emit q->changed();
        auto req = request(j->url); req.setRawHeader("Range", "bytes=0-0");
        auto r = network.get(req); j->probe = r; r->setReadBufferSize(65536);
        auto probeBody = std::make_shared<QByteArray>();
        auto serial = std::make_shared<bool>(false);
        auto consume = [this,j,r,probeBody,serial] {
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200 && !*serial) {
                if (!identity(r)) { fail(j,q->tr("Server ignored identity transfer encoding.")); return; }
                if (j->ranged && received(*j) > 0) { fail(j,q->tr("Server no longer supports resuming this file. Remove it and start again.")); return; }
                if (!clearParts(j)) return;
                j->ranged = false; j->validator.clear(); j->validatorHeader.clear();
                bool ok; auto size = r->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok); j->total = ok ? size : -1;
                auto p = std::make_shared<Part>(); j->parts.append(p);
                if (!openPart(j,0,false)) return;
                j->status = "downloading"; j->baseline = 0; j->clock.start(); *serial = true;
                if (!save()) { fail(j,error); return; }
                emit q->changed();
            }
            if (*serial) write(j,j->parts[0],r);
            else {
                probeBody->append(r->read(2));
                if (probeBody->size() > 1 || r->bytesAvailable() > 0) fail(j,q->tr("Invalid byte-range probe response."));
            }
        };
        QObject::connect(r,&QIODevice::readyRead,q,consume);
        QObject::connect(r,&QNetworkReply::finished,q,[this,j,r,probeBody,serial,consume] {
            if (j->probe != r) return;
            if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 416 && r->rawHeader("Content-Range") == "bytes */0") {
                if (received(*j) > 0) { fail(j,q->tr("The remote file changed. Existing partial data was preserved.")); return; }
                if (!clearParts(j)) return;
                j->ranged = false; j->total = 0; j->status = "downloading";
                j->parts.append(std::make_shared<Part>());
                if (!openPart(j,0,false)) return;
                j->parts[0]->file->close(); j->parts[0]->file.reset(); j->probe = nullptr; r->deleteLater(); finish(j); return;
            }
            if (r->error() != QNetworkReply::NoError) { fail(j,r->errorString()); return; }
            consume(); if (j->probe != r) return;
            j->probe = nullptr;
            if (*serial) {
                j->parts[0]->file->close(); j->parts[0]->file.reset(); r->deleteLater(); finish(j); return;
            }
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
            j->ranged = true; j->total = total; j->validator = value; j->validatorHeader = header; startRanges(j);
        });
    }
    void startSerial(const std::shared_ptr<Job> &j) {
        auto req = request(j->url); auto r = network.get(req); j->probe = r;
        auto p = std::make_shared<Part>(); j->parts = {p};
        if (!openPart(j,0,false)) return;
        j->ranged = false; j->total = -1; j->status = "downloading"; j->baseline=0; j->clock.start();
        r->setReadBufferSize(65536);
        auto consume = [this,j,p,r] {
            if (!p->checked) {
                if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=200 || !identity(r)) { fail(j,q->tr("Server returned an invalid download response.")); return; }
                bool ok; auto total = r->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok); j->total = ok ? total : -1; p->checked=true;
            }
            write(j,p,r);
        };
        QObject::connect(r,&QIODevice::readyRead,q,consume);
        QObject::connect(r,&QNetworkReply::finished,q,[this,j,p,r,consume] {
            if (j->probe != r) return;
            if (r->error()!=QNetworkReply::NoError) { fail(j,r->errorString()); return; }
            consume(); if (j->probe != r) return;
            p->file->close(); p->file.reset(); j->probe=nullptr; r->deleteLater(); finish(j);
        });
        save(); emit q->changed();
    }
};

DownloadManager::DownloadManager(const QString &stateDirectory, QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this,stateDirectory)) {}
DownloadManager::~DownloadManager() = default;
QVariantList DownloadManager::downloads() const {
    QVariantList result;
    for (const auto &j : d->jobs) {
        const auto bytes = d->received(*j);
        const double speed = j->status == "downloading" && j->clock.isValid() ? (bytes-j->baseline)*1000.0/std::max<qint64>(1,j->clock.elapsed()) : 0;
        result.append(QVariantMap{{"id",j->id},{"url",j->url},{"path",j->path},{"name",QFileInfo(j->path).fileName()},{"status",j->status},{"received",bytes},{"total",j->total},{"speed",speed},{"connections",j->ranged ? j->parts.size() : 1},{"error",j->error}});
    }
    return result;
}
QString DownloadManager::error() const { return d->error; }
QString DownloadManager::downloadDirectory() const { return d->destination; }
QString DownloadManager::add(const QString &input, const QString &directory, int connections) {
    auto reject = [this](const QString &why) { d->error=why; emit changed(); return QString(); };
    if (!d->writable) return reject(d->error);
    QUrl url(input.trimmed(),QUrl::StrictMode);
    if (!url.isValid() || (url.scheme()!="http" && url.scheme()!="https") || url.host().isEmpty() || !url.userInfo().isEmpty()) return reject(tr("Enter an HTTP or HTTPS URL without embedded credentials."));
    QString name = QFileInfo(url.path()).fileName();
    name.replace(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\]"),"_");
    if (name.isEmpty() || name=="." || name=="..") name="download";
    QString dir = directory.isEmpty() ? d->destination : directory;
    if (dir.startsWith("file:")) dir=QUrl(dir).toLocalFile();
    if (!QFileInfo(dir).isAbsolute() || !QDir().mkpath(dir)) return reject(tr("Choose a writable absolute download directory."));
    QString path = QDir(dir).absoluteFilePath(name);
    auto occupied = [this](const QString &path) { if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return true; for (auto &j:d->jobs) if(j->path==path) return true; return false; };
    const QString original = path;
    for (int i=1; occupied(path); ++i) { if(i>10000) return reject(tr("Cannot choose an unused destination filename.")); path=original+"."+QString::number(i); }
    auto j=std::make_shared<Impl::Job>(); j->id=QUuid::createUuid().toString(QUuid::Id128); j->url=url.toString(QUrl::FullyEncoded); j->path=path; j->connections=std::clamp(connections,1,16);
    d->jobs.append(j); d->error.clear();
    if (!d->save()) { d->jobs.removeLast(); return {}; }
    d->start(j); return j->id;
}
void DownloadManager::pause(const QString &id) { if(auto j=d->find(id)) { d->stop(j); d->save(); emit changed(); } }
void DownloadManager::resume(const QString &id) { if(auto j=d->find(id)) d->start(j); }
void DownloadManager::remove(const QString &id) {
    auto j=d->find(id); if(!j || !d->writable) return;
    d->stop(j);
    if (QFileInfo(d->root+"/"+j->id).isSymLink()) { d->fail(j,tr("Unsafe partial download path.")); return; }
    if(j->status!="complete") for(int i=0;i<j->parts.size();++i) QFile::remove(d->partPath(*j,i));
    QDir(d->root+"/"+j->id).rmdir("."); d->jobs.removeOne(j); d->save(); emit changed();
}
void DownloadManager::pauseAll() { for(auto &j:d->jobs) d->stop(j); d->save(); emit changed(); }
