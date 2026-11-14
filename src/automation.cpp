#include "automation.h"
#include "downloadmanager.h"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {
const QString mainQueue = QStringLiteral("Main download queue");
QVariantMap queueDefault() {
  return {{"active", false},
          {"concurrent", 2},
          {"startAt", ""},
          {"stopAt", ""},
          {"repeatMinutes", 0}};
}
QString normalizedUrl(const QString &input) {
  QUrl url(input);
  url.setFragment({});
  return url.toString(QUrl::FullyEncoded);
}
QString urlOrigin(const QString &input) {
  const QUrl url(input.trimmed());
  const auto scheme = url.scheme().toLower();
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      !QStringList{"http", "https", "ftp", "ftps"}.contains(scheme))
    return {};
  QUrl origin;
  origin.setScheme(scheme);
  origin.setHost(url.host().toLower());
  origin.setPort(url.port(scheme == "https"  ? 443
                          : scheme == "ftps" ? 990
                          : scheme == "ftp"  ? 21
                                             : 80));
  return origin.toString(QUrl::FullyEncoded);
}
bool isSiteOrigin(const QString &input) {
  const QUrl url(input.trimmed());
  return !urlOrigin(input).isEmpty() &&
         (url.path().isEmpty() || url.path() == "/") && !url.hasQuery() &&
         !url.hasFragment();
}
bool integer(const QVariant &value, qint64 low, qint64 high) {
  bool ok = false;
  const double number = value.toDouble(&ok);
  return value.metaType().id() != QMetaType::Bool && ok &&
         std::isfinite(number) && std::floor(number) == number &&
         number >= low && number <= high;
}
bool date(const QVariant &value) {
  return value.metaType().id() == QMetaType::QString &&
         (value.toString().isEmpty() ||
          QDateTime::fromString(value.toString(), Qt::ISODate).isValid());
}
QString projectKey(const QVariantMap &job) {
  return normalizedUrl(job.value("url").toString()) + "\n" +
         QDir::cleanPath(job.value("directory").toString()) + "\n" +
         job.value("filename").toString();
}
QString validateProjects(const QVariant &value, bool files) {
  if (value.metaType().id() != QMetaType::QVariantList ||
      value.toList().size() > 1000)
    return QObject::tr(
        "Synchronization projects must be a list of at most 1000 entries.");
  QSet<QString> destinations;
  for (const auto &v : value.toList()) {
    if (v.metaType().id() != QMetaType::QVariantMap)
      return QObject::tr("Synchronization project must be a map.");
    const auto job = v.toMap();
    const QStringList keys{"url",     "directory",       "filename",
                           "depth",   "intervalMinutes", "nextRun",
                           "startAt", "stopAt",          "enabled",
                           "include", "exclude"};
    for (auto i = job.begin(); i != job.end(); ++i)
      if (!keys.contains(i.key()) ||
          (files &&
           QStringList{"depth", "include", "exclude"}.contains(i.key())) ||
          (!files && i.key() == "filename"))
        return QObject::tr("Unknown synchronization project field: %1")
            .arg(i.key());
    for (const auto &field : QStringList{"include", "exclude"}) {
      if (!job.contains(field))
        continue;
      const auto patterns = job.value(field);
      if ((patterns.metaType().id() != QMetaType::QVariantList &&
           patterns.metaType().id() != QMetaType::QStringList) ||
          patterns.toList().size() > 32)
        return QObject::tr(
            "Filters must be lists of at most 32 wildcard patterns.");
      for (const auto &pattern : patterns.toList())
        if (pattern.metaType().id() != QMetaType::QString ||
            pattern.toString().isEmpty() || pattern.toString().size() > 256 ||
            pattern.toString().contains(
                QRegularExpression("[\\x00-\\x1f\\x7f]")))
          return QObject::tr("Invalid wildcard filter.");
    }
    const QUrl url(job.value("url").toString(), QUrl::StrictMode);
    if (!url.isValid() ||
        !QStringList{"http", "https"}.contains(url.scheme()) ||
        url.host().isEmpty() || !url.userInfo().isEmpty() ||
        job.value("url").toString().size() > 16384 ||
        job.value("url").toString().contains(
            QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
        !QFileInfo(job.value("directory").toString()).isAbsolute() ||
        job.value("directory").toString().size() > 4096 ||
        job.value("directory")
            .toString()
            .contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
        !integer(job.value("intervalMinutes", files ? 1 : 0), files ? 1 : 0,
                 525600) ||
        (!files && !integer(job.value("depth", 1), 0, 5)) ||
        (job.contains("enabled") &&
         job.value("enabled").metaType().id() != QMetaType::Bool))
      return QObject::tr("Invalid synchronization project settings.");
    for (const auto &key : QStringList{"nextRun", "startAt", "stopAt"})
      if (job.contains(key) && !date(job.value(key)))
        return QObject::tr("Project timestamps must use ISO format.");
    const auto start =
        QDateTime::fromString(job.value("startAt").toString(), Qt::ISODate);
    const auto stop =
        QDateTime::fromString(job.value("stopAt").toString(), Qt::ISODate);
    if (start.isValid() && stop.isValid() && stop <= start)
      return QObject::tr("Project stop must be after its start.");
    const auto filename = job.value("filename").toString();
    if (files &&
        (filename.isEmpty() || filename == "." || filename == ".." ||
         filename.startsWith(".idm-") || filename.toUtf8().size() > 180 ||
         filename.contains(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\]"))))
      return QObject::tr("Synchronization needs a plain filename without the "
                         "reserved .idm- prefix.");
    const auto destination =
        QDir::cleanPath(job.value("directory").toString()) + "/" + filename;
    if (destinations.contains(destination))
      return QObject::tr(
          "Synchronization projects cannot share an output destination.");
    destinations.insert(destination);
  }
  return {};
}
QString validatePreferences(QVariantMap &values) {
  struct Bound {
    const char *name;
    qint64 minimum;
    qint64 maximum;
  };
  const Bound bounds[] = {{"connections", 1, 16},
                          {"maxConcurrent", 1, 32},
                          {"retryCount", 0, 20},
                          {"retryDelaySeconds", 1, 86400},
                          {"timeoutSeconds", 1, 3600},
                          {"rateLimit", 0, 1000000000000LL},
                          {"quotaMB", 0, 1000000000},
                          {"quotaPeriodMinutes", 1, 525600},
                          {"completionDelaySeconds", 5, 86400}};
  for (const auto &bound : bounds) {
    bool ok = false;
    const auto number = values.value(bound.name).toLongLong(&ok);
    if (!ok ||
        !integer(values.value(bound.name), bound.minimum, bound.maximum) ||
        number < bound.minimum || number > bound.maximum)
      return QObject::tr("Invalid setting: %1").arg(bound.name);
  }
  if (!QFileInfo(values.value("directory").toString()).isAbsolute())
    return QObject::tr("Download directory must be an absolute path.");
  if (!QStringList{"none", "shutdown", "disconnect"}.contains(
          values.value("completionAction").toString()))
    return QObject::tr("Unsupported completion action.");
  if (values.value("siteSettings").metaType().id() != QMetaType::QVariantMap)
    return QObject::tr("Site settings must be a map of full origins.");
  QVariantMap sites;
  const auto savedSites = values.value("siteSettings").toMap();
  for (auto i = savedSites.begin(); i != savedSites.end(); ++i) {
    const auto origin = urlOrigin(i.key());
    if (!isSiteOrigin(i.key()) || sites.contains(origin) ||
        i.value().metaType().id() != QMetaType::QVariantMap)
      return QObject::tr("Use a full site origin such as "
                         "https://example.com:443, without a path or login.");
    const auto profile = i.value().toMap();
    if (profile.contains("authMethod") &&
        (profile.value("authMethod").metaType().id() != QMetaType::QString ||
         !QStringList{"auto", "basic", "digest", "ntlm", "negotiate", "kerberos"}.contains(profile.value("authMethod").toString())))
      return QObject::tr("Choose a supported site authentication method.");
    sites[origin] = i.value();
  }
  values["siteSettings"] = sites;
  for (const auto &value : values.value("speedSchedule").toList()) {
    auto entry = value.toMap();
    bool ok = false;
    auto rate = entry.value("rateLimit").toLongLong(&ok);
    if (!QTime::fromString(entry.value("start").toString(), "HH:mm")
             .isValid() ||
        !QTime::fromString(entry.value("end").toString(), "HH:mm").isValid() ||
        !ok || rate < 0 || rate > 1000000000000LL)
      return QObject::tr("Invalid speed schedule.");
  }
  for (const auto &key : QStringList{"syncJobs", "fileSyncJobs"}) {
    const auto error =
        validateProjects(values.value(key), key == "fileSyncJobs");
    if (!error.isEmpty())
      return error;
  }
  return {};
}
} // namespace
Automation::Automation(DownloadManager *backend, const QString &directory,
                       QObject *parent, Executor run)
    : QObject(parent), manager(backend), state(directory),
      executor(std::move(run)) {
  settings = {{"directory", manager->downloadDirectory()},
              {"connections", 4},
              {"maxConcurrent", 3},
              {"retryCount", 3},
              {"retryDelaySeconds", 5},
              {"timeoutSeconds", 30},
              {"rateLimit", 0},
              {"quotaMB", 0},
              {"quotaPeriodMinutes", 1440},
              {"proxy", ""},
              {"clipboardMonitor", false},
              {"notifications", true},
              {"sound", false},
              {"antivirus", false},
              {"scannerCommand", QStringList{"clamscan", "--no-summary", "%f"}},
              {"completionAction", "none"},
              {"completionDelaySeconds", 30},
              {"darkMode", true},
              {"typeFilter", ""},
              {"siteSettings", QVariantMap{}},
              {"speedSchedule", QVariantList{}},
              {"syncJobs", QVariantList{}},
              {"fileSyncJobs", QVariantList{}}};
  queueSettings.insert(mainQueue, queueDefault());
  QFile file(state + "/automation.json");
  if (file.exists()) {
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
      failure = tr("Cannot read automation settings.");
    else {
      QJsonParseError error;
      const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
      if (error.error != QJsonParseError::NoError || !doc.isObject())
        failure = tr("Invalid automation settings; existing file kept.");
      else {
        const auto object = doc.object();
        const auto saved =
            object.value("preferences").toObject().toVariantMap();
        for (auto i = saved.begin(); i != saved.end(); ++i)
          if (settings.contains(i.key()))
            settings[i.key()] = i.value();
        const auto savedQueues =
            object.value("queues").toObject().toVariantMap();
        if (!savedQueues.isEmpty())
          queueSettings = savedQueues;
        if (object.contains("quota") &&
            (!object.value("quota").isObject() ||
             object.value("quota").toObject().isEmpty()))
          failure = tr("Invalid saved quota accounting; existing file kept.");
        pendingMirror = object.value("pendingMirror").toObject().toVariantMap();
        if (object.contains("pendingMirror") &&
            !object.value("pendingMirror").isObject())
          failure = tr("Invalid saved grabber job; existing file kept.");
        if (!pendingMirror.isEmpty()) {
          const auto invalid =
              validateProjects(QVariantList{pendingMirror}, false);
          if (!invalid.isEmpty())
            failure = invalid;
        }
        quotaState = object.value("quota").toObject().toVariantMap();
        projectResults =
            object.value("projectResults").toObject().toVariantMap();
        policies = object.value("policies").toObject().toVariantMap();
        legacySiteSettings =
            object.value("legacySiteSettings").toObject().toVariantMap();
        auto sites = settings.value("siteSettings").toMap();
        for (auto i = sites.begin(); i != sites.end();) {
          if (!isSiteOrigin(i.key())) {
            legacySiteSettings[i.key()] = i.value();
            i = sites.erase(i);
          } else {
            ++i;
          }
        }
        settings["siteSettings"] = sites;
        for (auto i = queueSettings.begin(); i != queueSettings.end(); ++i) {
          auto q = i.value().toMap();
          bool quotaWaiting = false;
          for (const auto &p : policies)
            if (p.toMap().value("queue", mainQueue) == i.key() &&
                p.toMap().value("quotaPaused").toBool())
              quotaWaiting = true;
          q["active"] = quotaWaiting;
          const auto defaults = queueDefault();
          for (auto field = defaults.begin(); field != defaults.end(); ++field)
            if (!q.contains(field.key()))
              q[field.key()] = field.value();
          i.value() = q;
        }
        for (auto i = policies.begin(); i != policies.end(); ++i) {
          auto p = i.value().toMap();
          p["armed"] = p.value("quotaPaused", false);
          i.value() = p;
        }
      }
    }
  }
  if (failure.isEmpty())
    failure = validatePreferences(settings);
  if (!failure.isEmpty())
    settingsWritable = false;
  if (quotaState.isEmpty()) {
    QVariantMap baseline;
    for (const auto &v : manager->downloads())
      baseline[v.toMap().value("id").toString()] = v.toMap().value("received");
    quotaState = {{"usedBytes", 0},
                  {"periodStartedAt",
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                  {"lastReceived", baseline}};
  } else if (!integer(quotaState.value("usedBytes"), 0, 9007199254740991LL) ||
             !QDateTime::fromString(
                  quotaState.value("periodStartedAt").toString(), Qt::ISODate)
                  .isValid() ||
             quotaState.value("lastReceived").metaType().id() !=
                 QMetaType::QVariantMap) {
    settingsWritable = false;
    failure = tr("Invalid saved quota accounting; existing file kept.");
  }
  const auto baseline = quotaState.value("lastReceived").toMap();
  for (auto i = baseline.begin(); i != baseline.end(); ++i)
    if (i.key().isEmpty() || !integer(i.value(), 0, 9007199254740991LL)) {
      settingsWritable = false;
      failure = tr("Invalid saved quota accounting; existing file kept.");
    }
  for (const auto &value : manager->downloads()) {
    const auto job = value.toMap();
    const auto id = job.value("id").toString();
    const auto scan = policies.value(id).toMap().value("scanStatus").toString();
    if (scan == "failed")
      unsafe.insert(id);
    if (job.value("status") == "complete" && scan != "pending")
      processed.insert(id);
  }
  connect(manager, &DownloadManager::changed, this, [this] {
    if (!pending) {
      pending = true;
      QTimer::singleShot(0, this, [this] {
        pending = false;
        reconcile();
      });
    }
  });
  tick.setInterval(1000);
  connect(&tick, &QTimer::timeout, this, &Automation::checkSchedules);
  tick.start();
  completionTimer.setSingleShot(true);
  connect(&completionTimer, &QTimer::timeout, this,
          &Automation::completionAction);
  if (auto *app =
          qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
    connect(app->clipboard(), &QClipboard::dataChanged, this, [this, app] {
      if (!settings.value("clipboardMonitor").toBool())
        return;
      const auto text = app->clipboard()->text().trimmed();
      const QUrl url(text);
      if (url.isValid() &&
          (url.scheme() == "http" || url.scheme() == "https" ||
           url.scheme() == "ftp") &&
          !url.host().isEmpty())
        emit clipboardUrl(text);
    });
  }
}
Automation::~Automation() {
  tick.stop();
  completionTimer.stop();
  cancelWorker("interrupted");
  updateQuota();
  save();
}
bool Automation::reject(const QString &error) {
  failure = error;
  emit changed();
  return false;
}
bool Automation::save() {
  if (!settingsWritable)
    return false;
  QSaveFile file(state + "/automation.json");
  const auto data =
      QJsonDocument(
          QJsonObject{
              {"version", 1},
              {"quota", QJsonObject::fromVariantMap(quotaState)},
              {"projectResults", QJsonObject::fromVariantMap(projectResults)},
              {"pendingMirror", QJsonObject::fromVariantMap(pendingMirror)},
              {"preferences", QJsonObject::fromVariantMap(settings)},
              {"queues", QJsonObject::fromVariantMap(queueSettings)},
              {"policies", QJsonObject::fromVariantMap(policies)},
              {"legacySiteSettings",
               QJsonObject::fromVariantMap(legacySiteSettings)}})
          .toJson();
  if (data.size() > 4 * 1024 * 1024)
    return reject(tr("Automation settings exceed the 4 MB state limit."));
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      file.write(data) != data.size() || !file.commit())
    return reject(
        tr("Cannot save automation settings: %1").arg(file.errorString()));
  return true;
}
QVariantList Automation::queues() const {
  QVariantList result;
  for (auto i = queueSettings.begin(); i != queueSettings.end(); ++i) {
    auto q = i.value().toMap();
    q["name"] = i.key();
    result.append(q);
  }
  return result;
}
bool Automation::configure(const QVariantMap &options) {
  if (!settingsWritable)
    return reject(tr("Cannot update unreadable automation settings. Back up "
                     "and repair the saved file first."));
  for (auto i = options.begin(); i != options.end(); ++i)
    if (!settings.contains(i.key()))
      return reject(tr("Unknown setting: %1").arg(i.key()));
  auto next = settings;
  for (auto i = options.begin(); i != options.end(); ++i)
    next[i.key()] = i.value();
  const auto invalid = validatePreferences(next);
  if (!invalid.isEmpty())
    return reject(invalid);
  const auto previous = settings;
  settings = next;
  failure.clear();
  cancelCompletionAction();
  const auto previousResults = projectResults;
  for (const auto &list : QStringList{"fileSyncJobs", "syncJobs"}) {
    QMap<QString, QVariantMap> old;
    for (const auto &v : previous.value(list).toList())
      old[projectKey(v.toMap())] = v.toMap();
    for (const auto &v : settings.value(list).toList()) {
      auto job = v.toMap();
      auto before = old.value(projectKey(job));
      const auto key = list + "/" + projectKey(job);
      job.remove("nextRun");
      before.remove("nextRun");
      if (job != before)
        projectResults.remove(key);
    }
  }
  QSet<QString> retained;
  for (const auto &list : QStringList{"fileSyncJobs", "syncJobs"})
    for (const auto &v : settings.value(list).toList())
      retained.insert(list + "/" + projectKey(v.toMap()));
  for (auto i = projectResults.begin(); i != projectResults.end();)
    if (!retained.contains(i.key()))
      i = projectResults.erase(i);
    else
      ++i;
  if (!save()) {
    settings = previous;
    projectResults = previousResults;
    emit changed();
    return false;
  }
  emit changed();
  reconcile();
  return true;
}
QString Automation::siteOrigin(const QString &url) const {
  return urlOrigin(url);
}
QVariantMap Automation::siteSettingsForEditing(const QString &url) const {
  if (!isSiteOrigin(url))
    return {};
  const auto sites = settings.value("siteSettings").toMap();
  const auto origin = urlOrigin(url);
  if (sites.contains(origin))
    return sites.value(origin).toMap();
  const auto host = QUrl(url.trimmed()).host();
  for (auto i = legacySiteSettings.begin(); i != legacySiteSettings.end(); ++i)
    if (i.key().compare(host, Qt::CaseInsensitive) == 0)
      return i.value().toMap();
  return {};
}
QString Automation::enqueue(QVariantMap request) {
  if (!settingsWritable)
    return {};
  const auto url = normalizedUrl(request.value("url").toString());
  const auto site =
      settings.value("siteSettings").toMap().value(urlOrigin(url)).toMap();
  for (const auto &key :
       QStringList{"directory", "connections", "proxy", "username", "password", "authMethod"})
    if (!request.contains(key) && site.contains(key))
      request[key] = site[key];
  if (!request.contains("directory"))
    request["directory"] = settings.value("directory");
  if (!request.contains("connections"))
    request["connections"] = settings.value("connections");
  if (!request.contains("proxy"))
    request["proxy"] = settings.value("proxy");
  if (site.contains("headers") &&
      site.value("headers").metaType().id() != QMetaType::QVariantMap) {
    reject(tr("Site request headers must be a map."));
    return {};
  }
  if (!request.contains("headers") ||
      request.value("headers").metaType().id() == QMetaType::QVariantMap) {
    QVariantMap headers;
    const auto siteHeaders = site.value("headers").toMap();
    for (auto i = siteHeaders.begin(); i != siteHeaders.end(); ++i)
      headers[i.key().toLower()] = i.value();
    const auto supplied = request.value("headers").toMap();
    for (auto i = supplied.begin(); i != supplied.end(); ++i)
      headers[i.key().toLower()] = i.value();
    if (!headers.isEmpty() || request.contains("headers"))
      request["headers"] = headers;
  }
  const auto filter =
      settings.value("typeFilter").toString().trimmed().toLower();
  if (!filter.isEmpty() && filter != "*") {
    auto allowed =
        filter.split(QRegularExpression("[;,[:space:]]+"), Qt::SkipEmptyParts);
    for (auto &extension : allowed) {
      if (extension.startsWith("*."))
        extension.remove(0, 2);
      else if (extension.startsWith('.'))
        extension.remove(0, 1);
    }
    const QString name =
        request.value("filename", QFileInfo(QUrl(url).path()).fileName())
            .toString();
    if (!allowed.contains(QFileInfo(name).suffix().toLower())) {
      reject(tr("This file type is excluded by your download filter."));
      return {};
    }
  }
  if (!request.value("allowDuplicate").toBool())
    for (const auto &value : manager->downloads()) {
      auto job = value.toMap();
      if (normalizedUrl(job.value("url").toString()) == url &&
          job.value("media").toBool() == request.value("media").toBool() &&
          (!request.value("media").toBool() ||
           job.value("format", "best").toString() ==
               request.value("format", "best").toString()) &&
          QFileInfo(job.value("path").toString()).absolutePath() ==
              QDir(request.value("directory").toString()).absolutePath() &&
          (job.value("status") != "complete" ||
           QFileInfo::exists(job.value("path").toString()))) {
        const auto id = job.value("id").toString();
        if (request.value("start", true).toBool() &&
            job.value("status") != "complete") {
          if ((job.value("status") == "paused" ||
               job.value("status") == "error") &&
              (request.contains("headers") || request.contains("username") ||
               request.contains("password") || request.contains("authMethod") || request.contains("pageUrl"))) {
            bool updated = false;
            if (!QMetaObject::invokeMethod(
                    manager, "updateContext", Qt::DirectConnection,
                    Q_RETURN_ARG(bool, updated), Q_ARG(QString, id),
                    Q_ARG(QVariantMap, request)) ||
                !updated) {
              reject(
                  manager->error().isEmpty()
                      ? tr("Could not update this download's browser context.")
                      : manager->error());
              return {};
            }
          }
          failure.clear();
          resume(id);
        }
        return id;
      }
    }
  if (request.value("proxy").metaType().id() == QMetaType::QString) {
    const auto text = request.value("proxy").toString();
    if (text.isEmpty())
      request.remove("proxy");
    else {
      const QUrl proxy(text);
      if (!proxy.isValid() || proxy.host().isEmpty() ||
          !QStringList{"http", "https", "socks5"}.contains(proxy.scheme())) {
        reject(tr("Proxy must be an http://, https:// or socks5:// URL."));
        return {};
      }
      request["proxy"] = QVariantMap{
          {"type", proxy.scheme()},
          {"host", proxy.host()},
          {"port", proxy.port(proxy.scheme() == "socks5" ? 1080 : proxy.scheme() == "https" ? 443 : 8080)},
          {"username", proxy.userName()},
          {"password", proxy.password()}};
    }
  }
  request["rateLimitBytes"] = effectiveRate();
  request["timeoutSeconds"] = settings.value("timeoutSeconds");
  const bool start = request.value("start", true).toBool();
  request["start"] = false;
  const auto id = manager->addRequest(request);
  if (id.isEmpty()) {
    reject(manager->error());
    return {};
  }
  const auto queue = request.value("queue", mainQueue).toString();
  if (!queueSettings.contains(queue))
    queueSettings[queue] = queueDefault();
  policies[id] = QVariantMap{{"queue", queue},
                             {"priority", request.value("priority", 0)},
                             {"armed", start},
                             {"retries", 0},
                             {"nextTry", ""}};
  if (start) {
    auto q = queueSettings[queue].toMap();
    q["active"] = true;
    queueSettings[queue] = q;
  }
  failure.clear();
  cancelCompletionAction();
  save();
  emit changed();
  reconcile();
  return id;
}
QVariantMap Automation::batch(const QVariantList &requests) {
  if (requests.isEmpty() || requests.size() > 1000)
    return {{"ok", false}, {"error", tr("Batch must contain 1–1000 URLs.")}};
  QVariantList ids, errors;
  for (const auto &request : requests) {
    auto id = enqueue(request.toMap());
    if (id.isEmpty())
      errors.append(failure);
    else
      ids.append(id);
  }
  return {{"ok", !ids.isEmpty()}, {"ids", ids}, {"errors", errors}};
}
void Automation::pause(const QString &id) {
  auto p = policies.value(id).toMap();
  p["armed"] = false;
  p["quotaPaused"] = false;
  policies[id] = p;
  manager->pause(id);
  save();
  emit changed();
}
void Automation::resume(const QString &id) {
  if (!settingsWritable) {
    reject(
        tr("Repair unreadable automation settings before resuming downloads."));
    return;
  }
  cancelCompletionAction();
  auto p = policies.value(id).toMap();
  p["armed"] = true;
  p["manual"] = true;
  p["retries"] = 0;
  p["nextTry"] = "";
  if (!p.contains("queue"))
    p["queue"] = mainQueue;
  policies[id] = p;
  auto q = queueSettings.value(p["queue"].toString(), queueDefault()).toMap();
  q["active"] = true;
  queueSettings[p["queue"].toString()] = q;
  save();
  reconcile();
}
void Automation::pauseAll() {
  for (const auto &v : manager->downloads())
    pause(v.toMap().value("id").toString());
  for (auto i = queueSettings.begin(); i != queueSettings.end(); ++i) {
    auto q = i.value().toMap();
    q["active"] = false;
    i.value() = q;
  }
  cancelCompletionAction();
  save();
}
void Automation::resumeAll() {
  for (const auto &v : manager->downloads())
    if (v.toMap().value("status") != "complete")
      resume(v.toMap().value("id").toString());
}
void Automation::remove(const QString &id) {
  updateQuota();
  manager->remove(id);
  policies.remove(id);
  processed.remove(id);
  unsafe.remove(id);
  save();
  emit changed();
}
void Automation::removeCompleted() {
  for (const auto &v : manager->downloads())
    if (v.toMap().value("status") == "complete")
      remove(v.toMap().value("id").toString());
}
bool Automation::setJobPolicy(const QString &id, const QVariantMap &policy) {
  bool exists = false;
  for (const auto &v : manager->downloads())
    if (v.toMap().value("id") == id)
      exists = true;
  if (!exists)
    return reject(tr("Download was not found."));
  auto p = policies.value(id).toMap();
  for (const auto &key : QStringList{"queue", "priority"})
    if (policy.contains(key))
      p[key] = policy[key];
  if (p.value("queue").toString().isEmpty())
    p["queue"] = mainQueue;
  if (!queueSettings.contains(p["queue"].toString()))
    queueSettings[p["queue"].toString()] = queueDefault();
  policies[id] = p;
  save();
  emit changed();
  reconcile();
  return true;
}
bool Automation::setQueue(const QString &name, const QVariantMap &policy) {
  if (name.trimmed().isEmpty() || name.size() > 128)
    return reject(tr("Enter a queue name."));
  auto q = queueSettings.value(name, queueDefault()).toMap();
  for (auto i = policy.begin(); i != policy.end(); ++i) {
    if (!q.contains(i.key()))
      return reject(tr("Unknown queue setting."));
    q[i.key()] = i.value();
  }
  if (!integer(q["concurrent"], 1, 32) ||
      !integer(q["repeatMinutes"], 0, 525600))
    return reject(tr("Invalid queue limits."));
  for (const auto &key : QStringList{"startAt", "stopAt"})
    if (!q[key].toString().isEmpty() &&
        !QDateTime::fromString(q[key].toString(), Qt::ISODate).isValid())
      return reject(tr("Queue dates must use ISO format."));
  queueSettings[name] = q;
  failure.clear();
  save();
  emit changed();
  reconcile();
  return true;
}
void Automation::startQueue(const QString &name) {
  if (!settingsWritable) {
    reject(tr("Repair unreadable automation settings before starting queues."));
    return;
  }
  cancelCompletionAction();
  if (!queueSettings.contains(name))
    queueSettings[name] = queueDefault();
  auto q = queueSettings[name].toMap();
  q["active"] = true;
  queueSettings[name] = q;
  for (const auto &v : manager->downloads()) {
    auto job = v.toMap();
    auto p = policies.value(job["id"].toString()).toMap();
    if (p.value("queue", mainQueue) == name && job["status"] != "complete") {
      p["queue"] = name;
      p["armed"] = true;
      p["manual"] = true;
      p["retries"] = 0;
      p["nextTry"] = "";
      policies[job["id"].toString()] = p;
    }
  }
  save();
  emit changed();
  reconcile();
}
void Automation::stopQueue(const QString &name) {
  auto q = queueSettings.value(name, queueDefault()).toMap();
  q["active"] = false;
  queueSettings[name] = q;
  for (const auto &v : manager->downloads()) {
    auto job = v.toMap();
    if (policies.value(job["id"].toString())
            .toMap()
            .value("queue", mainQueue) == name)
      pause(job["id"].toString());
  }
  cancelCompletionAction();
  save();
  emit changed();
}
qint64 Automation::effectiveRate() const {
  qint64 limit = settings.value("rateLimit").toLongLong();
  const auto now = QTime::currentTime();
  for (const auto &v : settings.value("speedSchedule").toList()) {
    auto s = v.toMap();
    auto first = QTime::fromString(s["start"].toString(), "HH:mm"),
         last = QTime::fromString(s["end"].toString(), "HH:mm");
    if ((first <= last && now >= first && now < last) ||
        (first > last && (now >= first || now < last)))
      limit = s["rateLimit"].toLongLong();
  }
  return limit;
}
void Automation::checkSchedules() {
  if (!settingsWritable)
    return;
  const auto now = QDateTime::currentDateTime();
  bool dirty = false;
  for (auto i = queueSettings.begin(); i != queueSettings.end(); ++i) {
    auto q = i.value().toMap();
    auto start = QDateTime::fromString(q["startAt"].toString(), Qt::ISODate),
         stop = QDateTime::fromString(q["stopAt"].toString(), Qt::ISODate);
    if (start.isValid() && start <= now) {
      const auto name = i.key();
      q["startAt"] = q["repeatMinutes"].toInt() > 0
                         ? now.addSecs(q["repeatMinutes"].toInt() * 60)
                               .toString(Qt::ISODate)
                         : QString();
      i.value() = q;
      startQueue(name);
      dirty = true;
    }
    if (stop.isValid() && stop <= now) {
      const auto name = i.key();
      q = i.value().toMap();
      q["stopAt"] = "";
      i.value() = q;
      stopQueue(name);
      dirty = true;
    }
  }
  if (updateQuota()) {
    if (!mirrorProcess && !pendingMirror.isEmpty() && !quotaBlocked())
      startWorker(pendingMirror);
    checkProjects();
  }
  if (dirty)
    save();
  reconcile();
}
void Automation::reconcile() {
  if (!settingsWritable)
    return;
  if (reconciling)
    return;
  reconciling = true;
  const bool accountingOK = updateQuota();
  const bool blocked = !accountingOK || quotaBlocked();
  if (blocked && mirrorProcess &&
      mirrorProcess->state() != QProcess::NotRunning)
    cancelWorker("quotaPaused");
  auto jobs = manager->downloads();
  QVariantList waiting;
  QMap<QString, int> counts;
  int active = 0;
  bool dirty = false;
  const auto now = QDateTime::currentDateTime();
  const auto rate = effectiveRate();
  for (const auto &value : jobs) {
    const auto job = value.toMap();
    const auto id = job["id"].toString(), status = job["status"].toString();
    auto policy = policies.value(id).toMap();
    const auto queue = policy.value("queue", mainQueue).toString();
    if (status == "complete") {
      if (!processed.contains(id) && !scanning.contains(id)) {
        completed(job);
        dirty = true;
      }
      continue;
    }
    if (blocked) {
      if (policy.value("armed").toBool()) {
        policy["quotaPaused"] = true;
        policies[id] = policy;
        dirty = true;
      }
      if (status == "downloading" || status == "probing")
        manager->pause(id);
      continue;
    }
    if (policy.value("quotaPaused").toBool()) {
      policy["quotaPaused"] = false;
      policies[id] = policy;
      dirty = true;
    }
    if (status == "downloading" || status == "probing") {
      ++active;
      ++counts[queue];
      if (lastRate != rate)
        QMetaObject::invokeMethod(manager, "setRateLimit", Qt::DirectConnection,
                                  Q_ARG(QString, id), Q_ARG(qint64, rate));
      continue;
    }
    if (!policy.value("armed").toBool() ||
        !queueSettings.value(queue).toMap().value("active").toBool())
      continue;
    if (status == "error" && !policy.value("manual").toBool()) {
      const int code = job.value("httpStatus").toInt();
      if (code == 401 || code == 403 || code == 410) {
        policy["armed"] = false;
        policies[id] = policy;
        failure = tr("This link expired or requires a fresh browser session. "
                     "Refresh its URL.");
        dirty = true;
        continue;
      }
      auto next =
          QDateTime::fromString(policy["nextTry"].toString(), Qt::ISODate);
      if (!next.isValid()) {
        if (policy.value("retries").toInt() >= settings["retryCount"].toInt()) {
          policy["armed"] = false;
          policies[id] = policy;
          dirty = true;
          continue;
        }
        const int retry = policy.value("retries").toInt() + 1;
        policy["retries"] = retry;
        policy["nextTry"] = now.addSecs(settings["retryDelaySeconds"].toInt() *
                                        std::min(8, retry))
                                .toString(Qt::ISODate);
        policies[id] = policy;
        dirty = true;
        continue;
      }
      if (next > now)
        continue;
    }
    auto candidate = job;
    candidate["queue"] = queue;
    candidate["priority"] = policy.value("priority", 0);
    waiting.append(candidate);
  }
  std::stable_sort(waiting.begin(), waiting.end(),
                   [](const QVariant &a, const QVariant &b) {
                     return a.toMap().value("priority").toInt() >
                            b.toMap().value("priority").toInt();
                   });
  for (const auto &value : waiting) {
    auto job = value.toMap();
    const auto queue = job["queue"].toString();
    if (active >= settings["maxConcurrent"].toInt())
      break;
    if (counts[queue] >=
        queueSettings.value(queue).toMap().value("concurrent", 2).toInt())
      continue;
    const auto id = job["id"].toString();
    auto p = policies[id].toMap();
    p["nextTry"] = "";
    p["manual"] = false;
    policies[id] = p;
    QMetaObject::invokeMethod(manager, "setRateLimit", Qt::DirectConnection,
                              Q_ARG(QString, id), Q_ARG(qint64, rate));
    manager->resume(id);
    ++active;
    ++counts[queue];
    dirty = true;
  }
  lastRate = rate;
  if (dirty)
    save();
  reconciling = false;
  emit changed();
}
void Automation::execute(const QStringList &command,
                         std::function<void(int, const QString &)> done) {
  if (command.isEmpty()) {
    done(-1, tr("No command configured."));
    return;
  }
  if (executor) {
    executor(command, std::move(done));
    return;
  }
  const auto executable = QStandardPaths::findExecutable(command.first());
  if (executable.isEmpty()) {
    done(-1, tr("Required tool is missing: %1").arg(command.first()));
    return;
  }
  auto *process = new QProcess(this);
  auto finished = std::make_shared<bool>(false);
  auto complete = [process, finished, done](int code, const QString &error) {
    if (*finished)
      return;
    *finished = true;
    done(code, error);
    process->deleteLater();
  };
  connect(process, &QProcess::finished, this,
          [process, complete](int code, QProcess::ExitStatus status) {
            complete(status == QProcess::NormalExit ? code : -1,
                     QString::fromUtf8(process->readAllStandardError() +
                                       process->readAllStandardOutput())
                         .right(32768)
                         .trimmed());
          });
  connect(process, &QProcess::errorOccurred, this,
          [process, complete](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
              complete(-1, process->errorString());
          });
  process->start(executable, command.mid(1));
}
void Automation::completed(const QVariantMap &job) {
  const QString id = job["id"].toString();
  scanning.insert(id);
  auto finish = [this, job, id](int code, const QString &error) {
    scanning.remove(id);
    processed.insert(id);
    auto policy = policies.value(id).toMap();
    policy["scanStatus"] = code == 0 ? "clean" : "failed";
    policies[id] = policy;
    if (code == 0)
      unsafe.remove(id);
    else
      unsafe.insert(id);
    save();
    if (code != 0) {
      reject(tr("Antivirus scan failed for %1: %2")
                 .arg(job["name"].toString(), error));
      cancelCompletionAction();
      return;
    }
    if (settings["notifications"].toBool())
      execute({"notify-send", "--app-name=IDM Native", tr("Download complete"),
               job["name"].toString()},
              [this](int code, const QString &error) {
                if (code != 0)
                  reject(tr("Notification failed: %1").arg(error));
              });
    if (settings["sound"].toBool())
      execute({"canberra-gtk-play", "--id", "complete"},
              [this](int code, const QString &error) {
                if (code != 0)
                  reject(tr("Sound notification failed: %1").arg(error));
              });
    bool allDone = !manager->downloads().isEmpty();
    for (const auto &value : manager->downloads())
      if (value.toMap()["status"] != "complete")
        allDone = false;
    if (allDone && !mirrorProcess && scanning.isEmpty() && unsafe.isEmpty() &&
        settings["completionAction"] != "none") {
      notice = tr("%1 is scheduled after downloads. Cancel within %2 seconds.")
                   .arg(settings["completionAction"].toString())
                   .arg(settings["completionDelaySeconds"].toInt());
      completionTimer.start(settings["completionDelaySeconds"].toInt() * 1000);
    }
    save();
    emit changed();
  };
  if (settings["antivirus"].toBool()) {
    auto policy = policies.value(id).toMap();
    policy["scanStatus"] = "pending";
    policies[id] = policy;
    save();
    auto command = settings["scannerCommand"].toStringList();
    if (command.isEmpty()) {
      finish(-1, tr("No antivirus scanner configured."));
      return;
    }
    bool replaced = false;
    for (auto &arg : command)
      if (arg.contains("%f")) {
        arg.replace("%f", job["path"].toString());
        replaced = true;
      }
    if (!replaced)
      command.append(job["path"].toString());
    execute(command, finish);
  } else
    finish(0, {});
}
void Automation::cancelCompletionAction() {
  completionTimer.stop();
  notice.clear();
  emit changed();
}
void Automation::completionAction() {
  notice.clear();
  if (!settingsWritable || mirrorProcess || !scanning.isEmpty() ||
      !unsafe.isEmpty()) {
    emit changed();
    return;
  }
  for (const auto &value : manager->downloads())
    if (value.toMap().value("status") != "complete") {
      emit changed();
      return;
    }
  const auto action = settings["completionAction"].toString();
  QStringList command;
  if (action == "shutdown")
    command = {"systemctl", "poweroff"};
  else if (action == "disconnect")
    command = {"nmcli", "networking", "off"};
  else
    return;
  execute(command, [this](int code, const QString &error) {
    if (code != 0)
      reject(tr("Completion action failed: %1").arg(error));
  });
  emit changed();
}
bool Automation::importList(const QString &path) {
  QFile file(QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 10 * 1024 * 1024)
    return reject(tr("Cannot read import file."));
  auto bytes = file.readAll();
  QVariantList requests;
  QJsonParseError error;
  const auto doc = QJsonDocument::fromJson(bytes, &error);
  if (error.error == QJsonParseError::NoError) {
    auto list = doc.isArray()
                    ? doc.array().toVariantList()
                    : doc.object().value("downloads").toArray().toVariantList();
    for (const auto &v : list) {
      auto source = v.toMap();
      QVariantMap request;
      for (const auto &key :
           QStringList{"url", "filename", "description", "queue", "priority",
                       "media", "format", "connections"})
        if (source.contains(key))
          request[key] = source[key];
      request["start"] = false;
      requests.append(request);
    }
  } else {
    for (const auto &line : QString::fromUtf8(bytes).split('\n')) {
      auto url = line.trimmed();
      if (!url.isEmpty() && !url.startsWith('#'))
        requests.append(QVariantMap{{"url", url}, {"start", false}});
    }
  }
  if (requests.isEmpty())
    return reject(tr("Import contains no download URLs."));
  const auto result = batch(requests);
  if (!result.value("errors").toList().isEmpty())
    return reject(tr("Some imported URLs were rejected."));
  return result.value("ok").toBool();
}
bool Automation::exportList(const QString &path) {
  const auto destination =
      QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
  if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
    return reject(
        tr("Export destination already exists. Choose a new filename."));
  QJsonArray downloads;
  for (const auto &value : manager->downloads()) {
    const auto job = value.toMap();
    const auto policy = policies.value(job["id"].toString()).toMap();
    downloads.append(
        QJsonObject{{"url", job["url"].toString()},
                    {"filename", job["name"].toString()},
                    {"description", job["description"].toString()},
                    {"queue", policy.value("queue", mainQueue).toString()},
                    {"priority", policy.value("priority", 0).toInt()},
                    {"media", job.value("media").toBool()},
                    {"format", job.value("format", "best").toString()},
                    {"connections", job.value("connections", 4).toInt()}});
  }
  QSaveFile file(QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path);
  const auto bytes =
      QJsonDocument(QJsonObject{{"version", 1}, {"downloads", downloads}})
          .toJson();
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      file.write(bytes) != bytes.size() || !file.commit())
    return reject(
        tr("Cannot export download list: %1").arg(file.errorString()));
  failure.clear();
  emit changed();
  return true;
}
bool Automation::refresh(const QString &id, const QString &url) {
  bool ok = false;
  if (!QMetaObject::invokeMethod(manager, "refresh", Qt::DirectConnection,
                                 Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                 Q_ARG(QString, url)) ||
      !ok)
    return reject(manager->error().isEmpty()
                      ? tr("Could not refresh this download.")
                      : manager->error());
  failure.clear();
  resume(id);
  return true;
}
bool Automation::rename(const QString &id, const QString &filename) {
  bool ok = false;
  if (!QMetaObject::invokeMethod(manager, "rename", Qt::DirectConnection,
                                 Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                 Q_ARG(QString, filename)) ||
      !ok)
    return reject(manager->error().isEmpty()
                      ? tr("Could not rename this download.")
                      : manager->error());
  failure.clear();
  emit changed();
  return true;
}
QVariantMap Automation::quota() const {
  const auto start = QDateTime::fromString(
      quotaState.value("periodStartedAt").toString(), Qt::ISODate);
  return {{"limitBytes", settings.value("quotaMB").toLongLong() * 1024 * 1024},
          {"usedBytes", quotaState.value("usedBytes", 0)},
          {"periodStartedAt", start.toString(Qt::ISODate)},
          {"resumeAt",
           start.addSecs(settings.value("quotaPeriodMinutes").toLongLong() * 60)
               .toString(Qt::ISODate)},
          {"paused", quotaBlocked()},
          {"accounting", "receivedBytes"}};
}
bool Automation::quotaBlocked() const {
  const auto limit = settings.value("quotaMB").toLongLong() * 1024 * 1024;
  return limit > 0 && quotaState.value("usedBytes").toLongLong() >= limit;
}
bool Automation::updateQuota() {
  if (!settingsWritable)
    return false;
  const auto previous = quotaState;
  const auto now = QDateTime::currentDateTimeUtc();
  const auto start = QDateTime::fromString(
      quotaState.value("periodStartedAt").toString(), Qt::ISODate);
  const auto period = settings.value("quotaPeriodMinutes").toLongLong() * 60;
  if (start.addSecs(period) <= now) {
    quotaState["periodStartedAt"] =
        start.addSecs((start.secsTo(now) / period) * period)
            .toString(Qt::ISODate);
    quotaState["usedBytes"] = 0;
  }
  auto received = quotaState.value("lastReceived").toMap();
  QVariantMap current;
  qint64 used = quotaState.value("usedBytes").toLongLong();
  for (const auto &v : manager->downloads()) {
    const auto job = v.toMap();
    const auto id = job.value("id").toString();
    const auto bytes = job.value("received").toLongLong();
    const auto old = received.value(id, 0).toLongLong();
    // shortcut: manager exposes saved-byte snapshots, so retries and checkpoint
    // overshoot need a backend byte-budget hook for a strict network quota.
    used += bytes >= old ? bytes - old : bytes;
    current[id] = bytes;
  }
  quotaState["lastReceived"] = current;
  quotaState["usedBytes"] = used;
  if (quotaState != previous || accountingDirty) {
    if (!save()) {
      quotaState = previous;
      accountingDirty = true;
      return false;
    }
    accountingDirty = false;
  }
  return true;
}
QVariantList Automation::syncQueue() const {
  QVariantList result;
  for (const auto &v : settings.value("fileSyncJobs").toList()) {
    auto job = v.toMap();
    const auto key = projectKey(job);
    const auto status = projectResults.value("fileSyncJobs/" + key).toMap();
    for (auto i = status.begin(); i != status.end(); ++i)
      job[i.key()] = i.value();
    if (workerList == "fileSyncJobs" && workerKey == key)
      job["status"] = "downloading";
    else if (!job.contains("status"))
      job["status"] = "waiting";
    result.append(job);
  }
  return result;
}
void Automation::checkProjects() {
  const auto now = QDateTime::currentDateTime();
  for (const auto &list : QStringList{"fileSyncJobs", "syncJobs"}) {
    auto projects = settings.value(list).toList();
    bool runningFound = false;
    for (int n = 0; n < projects.size(); ++n) {
      auto job = projects[n].toMap();
      const auto key = projectKey(job);
      const auto stop =
          QDateTime::fromString(job.value("stopAt").toString(), Qt::ISODate);
      const bool enabled = job.value("enabled", true).toBool() &&
                           (!stop.isValid() || stop > now);
      if (workerList == list && workerKey == key) {
        runningFound = true;
        if (!enabled)
          cancelMirror();
      }
      if (!enabled || mirrorProcess || quotaBlocked())
        continue;
      const auto start =
          QDateTime::fromString(job.value("startAt").toString(), Qt::ISODate);
      const auto next =
          QDateTime::fromString(job.value("nextRun").toString(), Qt::ISODate);
      const auto previous = projectResults.value(list + "/" + key).toMap();
      const bool quotaRetry = previous.value("status") == "quotaPaused" ||
                              previous.value("status") == "downloading" ||
                              previous.value("status") == "interrupted";
      const int interval =
          job.value("intervalMinutes", list == "fileSyncJobs" ? 1 : 0).toInt();
      if ((start.isValid() && start > now) ||
          (!quotaRetry && next.isValid() && next > now) ||
          (!quotaRetry && interval == 0 && previous.contains("lastRun")))
        continue;
      if (interval > 0)
        job["nextRun"] = now.addSecs(interval * 60).toString(Qt::ISODate);
      projects[n] = job;
      settings[list] = projects;
      if (!save())
        return;
      QVariantMap request{{"url", job.value("url")},
                          {"directory", job.value("directory")}};
      if (list == "fileSyncJobs") {
        request["mode"] = "file";
        request["filename"] = job.value("filename");
      } else {
        request["depth"] = job.value("depth", 1);
        request["include"] = job.value("include", QVariantList{});
        request["exclude"] = job.value("exclude", QVariantList{});
      }
      if (startWorker(request, list, key)) {
        projectResults[list + "/" + key] = QVariantMap{
            {"status", "downloading"}, {"lastRun", now.toString(Qt::ISODate)}};
        save();
        runningFound = true;
      }
    }
    if (workerList == list && !runningFound)
      cancelMirror();
  }
}
void Automation::finishProject(const QString &status) {
  if (workerKey.isEmpty() && status != "quotaPaused" &&
      status != "interrupted") {
    pendingMirror.clear();
    save();
  }
  if (!workerKey.isEmpty()) {
    auto result = projectResults.value(workerList + "/" + workerKey).toMap();
    result["status"] = status;
    result["error"] = mirrorState.value("error");
    result["transferredBytes"] = workerTransferred;
    result["unchanged"] = mirrorState.value("unchanged", false);
    projectResults[workerList + "/" + workerKey] = result;
    workerKey.clear();
    workerList.clear();
    save();
  }
}
void Automation::readWorkerOutput(QProcess *process, QByteArray &buffer) {
  buffer.append(process->readAllStandardOutput());
  while (buffer.contains('\n')) {
    const auto pos = buffer.indexOf('\n');
    const auto event = QJsonDocument::fromJson(buffer.left(pos)).object();
    buffer.remove(0, pos + 1);
    const auto transferred =
        event.value("transferredBytes").toVariant().toLongLong();
    if (transferred > workerTransferred) {
      quotaState["usedBytes"] = quotaState.value("usedBytes").toLongLong() +
                                transferred - workerTransferred;
      workerTransferred = transferred;
      if (!save()) {
        accountingDirty = true;
        process->kill();
        mirrorState["error"] = tr("Cannot persist download quota accounting.");
      }
    }
    if (event.contains("error"))
      mirrorState["error"] = event.value("error").toString();
    if (event.contains("bytes"))
      mirrorState["received"] = event.value("bytes").toVariant();
    if (event.contains("files"))
      mirrorState["total"] = event.value("files").toVariant();
    if (event.contains("unchanged"))
      mirrorState["unchanged"] = event.value("unchanged").toBool();
    mirrorState["transferredBytes"] = workerTransferred;
    emit changed();
  }
}
bool Automation::mirror(const QString &url, const QString &directory, int depth,
                        const QVariantMap &filters) {
  if (!settingsWritable)
    return reject(
        tr("Repair unreadable automation settings before starting projects."));
  if (mirrorProcess)
    return reject(tr("A website grab is already running."));
  const QUrl parsed(url);
  if (!parsed.isValid() ||
      (parsed.scheme() != "http" && parsed.scheme() != "https") ||
      parsed.host().isEmpty() || !parsed.userInfo().isEmpty() ||
      !QFileInfo(directory).isAbsolute() || depth < 0 || depth > 5)
    return reject(tr("Choose an HTTP(S) URL, absolute destination and depth "
                     "from zero to five."));
  QVariantMap job{{"url", url}, {"directory", directory}, {"depth", depth}};
  for (auto i = filters.begin(); i != filters.end(); ++i) {
    if (i.key() != "include" && i.key() != "exclude")
      return reject(tr("Unknown grabber filter setting."));
    job[i.key()] = i.value();
  }
  const auto error = validateProjects(QVariantList{job}, false);
  if (!error.isEmpty())
    return reject(error);
  return startWorker(job);
}
bool Automation::startWorker(QVariantMap job, const QString &list,
                             const QString &key) {
  if (mirrorProcess || !settingsWritable || !updateQuota() || quotaBlocked())
    return reject(
        tr("Download quota is exhausted or accounting is unavailable."));
  const auto directory = job.value("directory").toString();
  const auto url = job.value("url").toString();
  if (!QDir().mkpath(directory))
    return reject(tr("Cannot create website destination."));
  QFile source(":/website_worker.py");
  if (!source.open(QIODevice::ReadOnly))
    return reject(tr("Website grabber worker is missing."));
  const auto script = state + "/website-worker.py",
             jobPath = state + "/website-job.json";
  QSaveFile scriptFile(script);
  const auto scriptBytes = source.readAll();
  if (!scriptFile.open(QIODevice::WriteOnly) ||
      !scriptFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      scriptFile.write(scriptBytes) != scriptBytes.size() ||
      !scriptFile.commit())
    return reject(tr("Cannot prepare website grabber."));
  const auto inputJob = job;
  const auto site = settings.value("siteSettings").toMap().value(urlOrigin(url)).toMap();
  if (site.contains("headers"))
    job["headers"] = site.value("headers");
  QSaveFile jobFile(jobPath);
  job["maxFiles"] = 500;
  job["maxBytes"] = job.value("mode") == "file" ? 1024LL * 1024 * 1024 * 1024 : 500LL * 1024 * 1024;
  job["transferLimit"] = 1024LL * 1024 * 1024 * 1024;
  if (settings.value("quotaMB").toLongLong() > 0)
    job["transferLimit"] =
        std::min<qint64>(1024LL * 1024 * 1024 * 1024,
                         settings.value("quotaMB").toLongLong() * 1024 * 1024 -
                             quotaState.value("usedBytes").toLongLong());
  const auto jobBytes =
      QJsonDocument(QJsonObject::fromVariantMap(job)).toJson();
  if (!jobFile.open(QIODevice::WriteOnly) ||
      !jobFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      jobFile.write(jobBytes) != jobBytes.size() || !jobFile.commit())
    return reject(tr("Cannot save website grab job."));
  mirrorState = {
      {"status", "downloading"}, {"url", url}, {"directory", directory},
      {"received", 0},           {"total", 0}, {"error", ""}};
  if (list.isEmpty()) {
    const auto previous = pendingMirror;
    pendingMirror = inputJob;
    if (!save()) {
      pendingMirror = previous;
      return false;
    }
  }
  workerKey = key;
  workerList = list;
  workerTransferred = 0;
  mirrorState["mode"] = job.value("mode", "website");
  mirrorState["transferredBytes"] = 0;
  mirrorProcess = new QProcess(this);
  auto *process = mirrorProcess;
  auto buffer = std::make_shared<QByteArray>();
  connect(process, &QProcess::readyReadStandardOutput, this,
          [this, process, buffer] {
            readWorkerOutput(process, *buffer);
            QTimer::singleShot(0, this, [this] { reconcile(); });
          });
  connect(
      process, &QProcess::finished, this,
      [this, process, jobPath, buffer](int code, QProcess::ExitStatus status) {
        readWorkerOutput(process, *buffer);
        mirrorState["status"] = code == 0 && status == QProcess::NormalExit
                                    ? "complete"
                                : code == 2 ? "quotaPaused"
                                            : "error";
        finishProject(mirrorState.value("status").toString());
        if (mirrorState["status"] == "error" &&
            mirrorState["error"].toString().isEmpty())
          mirrorState["error"] =
              QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (mirrorProcess == process)
          mirrorProcess = nullptr;
        process->deleteLater();
        QFile::remove(jobPath);
        emit changed();
      });
  connect(process, &QProcess::errorOccurred, this,
          [this, process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              mirrorState["status"] = "error";
              mirrorState["error"] = process->errorString();
              finishProject("error");
              if (mirrorProcess == process)
                mirrorProcess = nullptr;
              process->deleteLater();
              emit changed();
            }
          });
  cancelCompletionAction();
  process->start("/usr/bin/python3", {script, jobPath});
  failure.clear();
  emit changed();
  return true;
}
void Automation::cancelMirror() {
  pendingMirror.clear();
  cancelWorker("paused");
  save();
}
void Automation::cancelWorker(const QString &status) {
  if (!mirrorProcess)
    return;
  auto *process = mirrorProcess;
  mirrorProcess = nullptr;
  process->disconnect(this);
  process->kill();
  process->waitForFinished(2000);
  QByteArray buffer;
  readWorkerOutput(process, buffer);
  process->deleteLater();
  mirrorState["status"] = status;
  finishProject(status);
  emit changed();
}
