#include "desktopintegration.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrlQuery>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
#include <QVersionNumber>
#include <QWindow>

namespace {
const QStringList toolbarOrder{"resume", "pause", "delete", "details", "startQueue", "add", "stopAll", "stopQueue", "scheduler", "options"};
const QStringList toolbarHidden{"stopAll", "stopQueue", "scheduler", "options"};
const QStringList columnOrder{"name", "status", "total", "speed", "eta", "created", "description"};
const QStringList columnHidden{"eta", "created", "description"};
const QString releasePrefix = "https://github.com/earlyhours/idm-rust/releases/tag/";
QStringList known(QStringList saved, const QStringList &allowed) {
    saved.removeDuplicates();
    for (auto it = saved.begin(); it != saved.end();) {
        if (!allowed.contains(*it)) it = saved.erase(it); else ++it;
    }
    return saved;
}
QStringList ordered(QStringList saved, const QStringList &allowed) {
    saved = known(saved, allowed);
    for (const auto &key : allowed) if (!saved.contains(key)) saved.append(key);
    return saved;
}
}

DesktopIntegration::DesktopIntegration(const QString &stateDirectory, const QString &path,
                                       QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), settings(stateDirectory + "/desktop.ini", QSettings::IniFormat),
      executable(path.isEmpty() ? QCoreApplication::applicationFilePath() : path),
      stateDirectory(QFileInfo(stateDirectory).absoluteFilePath()), network(network ? network : &ownedNetwork) {}

QVariantMap DesktopIntegration::layout() const {
    auto hiddenColumns = known(settings.value("columnHidden", columnHidden).toStringList(), columnOrder);
    if (hiddenColumns.size() == columnOrder.size()) hiddenColumns = columnHidden;
    return {{"toolbarOrder", ordered(settings.value("toolbarOrder", toolbarOrder).toStringList(), toolbarOrder)},
            {"toolbarHidden", known(settings.value("toolbarHidden", toolbarHidden).toStringList(), toolbarOrder)},
            {"columnOrder", ordered(settings.value("columnOrder", columnOrder).toStringList(), columnOrder)},
            {"columnHidden", hiddenColumns}};
}

bool DesktopIntegration::reject(const QString &message) { failure = message; emit changed(); return false; }

bool DesktopIntegration::setLayout(const QString &kind, const QStringList &order, const QStringList &hidden) {
    const auto allowed = kind == "toolbar" ? toolbarOrder : kind == "column" ? columnOrder : QStringList{};
    auto valid = [&](const QStringList &keys) {
        QSet<QString> unique;
        for (const auto &key : keys) { if (!allowed.contains(key) || unique.contains(key)) return false; unique.insert(key); }
        return true;
    };
    if (allowed.isEmpty() || !valid(order) || !valid(hidden) || (kind == "column" && hidden.size() == allowed.size()))
        return reject(tr("Choose known, unique items and keep at least one column visible."));
    settings.setValue(kind + "Order", ordered(order, allowed));
    settings.setValue(kind + "Hidden", hidden);
    settings.sync();
    if (settings.status() != QSettings::NoError) return reject(tr("Cannot save desktop layout."));
    failure.clear(); emit changed(); return true;
}

bool DesktopIntegration::resetLayout() {
    return setLayout("toolbar", toolbarOrder, toolbarHidden) && setLayout("column", columnOrder, columnHidden);
}

QString DesktopIntegration::autostartPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart/idm-native.desktop";
}

bool DesktopIntegration::autostart() const {
    QFile file(autostartPath());
    const auto entry = desktopEntry("background");
    return !entry.isEmpty() && !QFileInfo(file).isSymLink() && file.open(QIODevice::ReadOnly) && file.readAll() == entry;
}

QString DesktopIntegration::desktopExec(const QString &path) {
    if (!QDir::isAbsolutePath(path) || path.contains('=') || path.contains('%') || path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))) return {};
    // shortcut: reject percent in executable paths, until GLib supports escaped percent there.
    QString result = path;
    result.replace('\\', "\\\\").replace('"', "\\\"").replace('`', "\\`").replace('$', "\\$");
    result.replace('\\', "\\\\"); // Desktop string escaping precedes Exec argument unquoting.
    return '"' + result + '"';
}

QByteArray DesktopIntegration::desktopEntry(const QString &arguments) const {
    const auto command = desktopExec(executable);
    if (command.isEmpty()) return {};
    const auto stateArgument = desktopExec(stateDirectory);
    if (stateArgument.isEmpty()) return {};
    return ("[Desktop Entry]\nType=Application\nName=IDM Native\nExec=" + command + " " + arguments + " --state-directory " + stateArgument +
            "\nIcon=idm-native\nTerminal=false\nX-IDM-Native-Managed=true\n").toUtf8();
}

bool DesktopIntegration::writeEntry(const QString &path, const QByteArray &contents) {
    if (contents.isEmpty() || QFileInfo(path).isSymLink()) return reject(tr("Unsafe desktop entry path or executable."));
    QFile existing(path);
    if (existing.exists() && (!existing.open(QIODevice::ReadOnly) || !existing.readAll().contains("\nX-IDM-Native-Managed=true\n")))
        return reject(tr("An unmanaged desktop entry already exists. It was left unchanged."));
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return reject(tr("Cannot create desktop entry directory."));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(contents) != contents.size() || !file.commit()) return reject(tr("Cannot save desktop entry."));
    return true;
}

bool DesktopIntegration::setAutostart(bool enabled) {
    const auto path = autostartPath();
    if (enabled) {
        if (!QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable()) return reject(tr("The application executable is unavailable."));
        if (!writeEntry(path, desktopEntry("background"))) return false;
    } else if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        QFile file(path);
        if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || !file.readAll().contains("\nX-IDM-Native-Managed=true\n"))
            return reject(tr("An unmanaged desktop entry was left unchanged."));
        file.close();
        if (!file.remove()) return reject(tr("Cannot remove login autostart entry."));
    }
    failure.clear(); emit changed(); return true;
}

bool DesktopIntegration::installUrlHandler() {
    const auto path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications/idm-native-url.desktop";
    const auto base = desktopEntry("open %u");
    if (base.isEmpty()) return reject(tr("Unsafe desktop entry executable or state path."));
    const auto entry = base + "MimeType=x-scheme-handler/idm-native;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/ftp;\nNoDisplay=true\n";
    if (!QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable()) return reject(tr("The application executable cannot be registered with the desktop."));
    if (!writeEntry(path, entry)) return false;
    QProcess process;
    process.start("xdg-mime", {"default", "idm-native-url.desktop", "x-scheme-handler/idm-native"});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill(); process.waitForFinished();
        return reject(tr("Could not register the URL handler with xdg-mime."));
    }
    failure.clear(); emit changed(); return true;
}

QUrl DesktopIntegration::externalUrl(QString text) {
    if (text.startsWith("idm-native:", Qt::CaseInsensitive)) {
        const QUrl wrapper(text, QUrl::StrictMode);
        const auto query = QUrlQuery(wrapper).queryItems(QUrl::FullyDecoded);
        if (!wrapper.isValid() || wrapper.host() != "download" || (!wrapper.path().isEmpty() && wrapper.path() != "/") ||
            !wrapper.userInfo().isEmpty() || wrapper.port() != -1 || wrapper.hasFragment() ||
            query.size() != 1 || query.first().first != "url") return {};
        text = query.first().second;
    }
    if (text.size() > 16384 || text.contains(QRegularExpression("[\\x00-\\x20\\x7f]"))) return {};
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() && QStringList{"http", "https", "ftp"}.contains(url.scheme()) ? url : QUrl{};
}

QList<QUrl> DesktopIntegration::completedFileUrls(const QVariantMap &job) {
    const QFileInfo file(job.value("path").toString());
    if (job.value("status") != "complete" || !file.isAbsolute() || !file.isFile() || file.isSymLink()) return {};
    return {QUrl::fromLocalFile(file.absoluteFilePath())};
}

int DesktopIntegration::dragDistance() const { return QGuiApplication::styleHints()->startDragDistance(); }

bool DesktopIntegration::dragCompleted(const QVariantMap &job, QObject *window) {
    const auto urls = completedFileUrls(job);
    auto *source = qobject_cast<QWindow *>(window);
    if (urls.isEmpty() || !source) return false;
    QDrag drag(source);
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    drag.setMimeData(mime);
    emit fileDragStarted(urls);
    drag.exec(Qt::CopyAction, Qt::CopyAction);
    return true;
}

QVariantMap DesktopIntegration::parseRelease(const QByteArray &json, const QString &currentVersion) {
    if (json.size() > 512 * 1024) return {};
    const auto object = QJsonDocument::fromJson(json).object();
    const QString tag = object.value("tag_name").toString();
    static const QRegularExpression version("^v?(\\d+\\.\\d+\\.\\d+)$");
    const auto match = version.match(tag);
    if (!match.hasMatch() || object.value("draft").toBool() || object.value("prerelease").toBool()) return {};
    const auto remote = QVersionNumber::fromString(match.captured(1));
    const auto current = QVersionNumber::fromString(currentVersion);
    if (remote.isNull() || current.isNull()) return {};
    return {{"tag", tag}, {"name", object.value("name").toString().left(256)}, {"notes", object.value("body").toString().left(32768)},
            {"published", object.value("published_at").toString().left(64)}, {"url", releasePrefix + tag}, {"newer", remote > current}};
}

void DesktopIntegration::checkForUpdates() {
    if (pending) return;
    pending = true; latest.clear(); updateMessage = tr("Checking GitHub releases..."); emit updateChanged();
    QNetworkRequest request(QUrl("https://api.github.com/repos/earlyhours/idm-rust/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "idm-native/" + QCoreApplication::applicationVersion().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setTransferTimeout(10000);
    auto *reply = network->get(request);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(15000);
    connect(reply, &QNetworkReply::readyRead, this, [reply] { if (reply->bytesAvailable() > 512 * 1024) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) updateMessage = tr("No published release is available yet.");
        else if (status == 403 || status == 429) updateMessage = tr("GitHub rate limit reached. Try again later.");
        else if (reply->error() != QNetworkReply::NoError || status != 200) updateMessage = tr("Could not check for updates. Check your connection and try again.");
        else {
            latest = parseRelease(reply->readAll(), QCoreApplication::applicationVersion());
            updateMessage = latest.isEmpty() ? tr("GitHub returned unsupported release information.") :
                latest.value("newer").toBool() ? tr("A newer release is available.") : tr("You are running the latest release or a newer build.");
        }
        pending = false; emit updateChanged(); reply->deleteLater();
    });
}

bool DesktopIntegration::openRelease() {
    const auto url = latest.value("url").toString();
    return url.startsWith(releasePrefix) && QDesktopServices::openUrl(QUrl(url));
}
