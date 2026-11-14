#pragma once
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace CategoryRules {
inline QString urlOrigin(const QString &input) {
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
inline bool isSiteOrigin(const QString &input) {
  const QUrl url(input.trimmed());
  return !urlOrigin(input).isEmpty() &&
         (url.path().isEmpty() || url.path() == "/") && !url.hasQuery() &&
         !url.hasFragment();
}
inline QString normalize(QVariant &value) {
  if (value.metaType().id() != QMetaType::QVariantList ||
      value.toList().size() > 100)
    return QObject::tr("Categories must be a list of at most 100 entries.");
  QVariantList result;
  QSet<QString> names{"compressed", "documents", "music",
                      "programs",   "video",     "other"};
  const QRegularExpression controls("[\\x00-\\x1f\\x7f]");
  for (const auto &entry : value.toList()) {
    if (entry.metaType().id() != QMetaType::QVariantMap)
      return QObject::tr("Each category must be a map.");
    auto rule = entry.toMap();
    for (auto i = rule.begin(); i != rule.end(); ++i)
      if (!QStringList{"name", "extensions", "origins", "directory"}.contains(
              i.key()))
        return QObject::tr("Unknown category field: %1").arg(i.key());
    const auto name = rule.value("name").toString().trimmed();
    if (rule.value("name").metaType().id() != QMetaType::QString ||
        name.isEmpty() || name.size() > 80 ||
        rule.value("name").toString().contains(controls) ||
        names.contains(name.toCaseFolded()))
      return QObject::tr("Use a unique category name of 1–80 characters, "
                         "different from built-in categories.");
    names.insert(name.toCaseFolded());
    rule["name"] = name;
    for (const auto &key : QStringList{"extensions", "origins"}) {
      const auto list = rule.value(key, QVariantList{});
      if ((list.metaType().id() != QMetaType::QVariantList &&
           list.metaType().id() != QMetaType::QStringList) ||
          list.toList().size() > 100)
        return QObject::tr("Category extensions and origins must be lists of "
                           "at most 100 strings.");
      QStringList normalized;
      for (const auto &item : list.toList()) {
        if (item.metaType().id() != QMetaType::QString ||
            item.toString().size() > 4096 || item.toString().contains(controls))
          return QObject::tr("Invalid category matcher.");
        auto text = item.toString().trimmed();
        if (key == "extensions") {
          text = text.toLower();
          if (text.startsWith("*."))
            text.remove(0, 2);
          else if (text.startsWith('.'))
            text.remove(0, 1);
          if (!QRegularExpression("^[a-z0-9][a-z0-9_+-]{0,31}$")
                   .match(text)
                   .hasMatch())
            return QObject::tr(
                "Use file extensions such as pdf, .zip or *.mp3.");
        } else {
          if (!QUrl(text, QUrl::StrictMode).isValid() || !isSiteOrigin(text))
            return QObject::tr("Use exact site origins such as "
                               "https://example.com, without paths or logins.");
          text = urlOrigin(text);
        }
        if (!normalized.contains(text))
          normalized.append(text);
      }
      rule[key] = normalized;
    }
    if (rule.value("extensions").toStringList().isEmpty() &&
        rule.value("origins").toStringList().isEmpty())
      return QObject::tr(
          "A category needs at least one extension or site origin.");
    const auto directory = rule.value("directory", "");
    if (directory.metaType().id() != QMetaType::QString ||
        directory.toString().size() > 4096 ||
        directory.toString().contains(controls) ||
        (!directory.toString().isEmpty() &&
         !QFileInfo(directory.toString()).isAbsolute()))
      return QObject::tr(
          "Category directory must be empty or an absolute path.");
    rule["directory"] = directory;
    result.append(rule);
  }
  value = result;
  return {};
}
inline QString filename(const QVariantMap &request) {
  // Match the output name before the transfer manager adds collision numbers.
  const auto media = request.value("media").toBool();
  auto name = request.value("filename").toString();
  if (name.isEmpty())
    name =
        media && !request.value("title").toString().isEmpty()
            ? request.value("title").toString()
            : QFileInfo(QUrl(request.value("url").toString().trimmed()).path())
                  .fileName();
  name.replace(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\]"), "_");
  if (name.isEmpty() || name == "." || name == "..")
    name = "download";
  while (name.toUtf8().size() > 160)
    name.chop(1);
  if (media) {
    if (request.value("format").toString() == "audio")
      name = QFileInfo(name).completeBaseName() + ".mka";
    else if (!QStringList{"mp4", "mkv", "webm", "m4a", "mp3", "ogg", "opus",
                          "wav", "flac", "aac"}
                  .contains(QFileInfo(name).suffix().toLower()))
      name = QFileInfo(name).completeBaseName() + ".mkv";
  }
  return name;
}
inline QVariantMap match(const QVariantList &rules, const QString &name,
                         const QString &url) {
  const auto extension = QFileInfo(name).suffix().toLower();
  const auto origin = urlOrigin(url);
  for (const auto &entry : rules) {
    const auto rule = entry.toMap();
    if (rule.value("extensions").toStringList().contains(extension) ||
        (!origin.isEmpty() &&
         rule.value("origins").toStringList().contains(origin)))
      return rule;
  }
  return {};
}
} // namespace CategoryRules
