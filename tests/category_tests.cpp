#include "automation.h"
#include "downloadmanager.h"
#include "portalpicker.h"
#include "theme.h"
#include "translator.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class CategoryServer : public QTcpServer {
public:
  const QByteArray body = "category destination output";
  CategoryServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          const auto input =
              socket->property("input").toByteArray() + socket->readAll();
          socket->setProperty("input", input);
          if (!input.contains("\r\n\r\n"))
            return;
          socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                        QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
    if (!listen(QHostAddress::LocalHost))
      qFatal("Cannot listen");
  }
  QString url(const QString &filename = {}) const {
    return QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(filename);
  }
};
static QObject *findObject(QObject *root, const QString &name) {
  if (root->objectName() == name)
    return root;
  for (auto *child : root->children())
    if (auto *found = findObject(child, name))
      return found;
  if (auto *item = qobject_cast<QQuickItem *>(root))
    for (auto *child : item->childItems())
      if (child->parent() != root)
        if (auto *found = findObject(child, name))
          return found;
  return nullptr;
}
class CategoryTests : public QObject {
  Q_OBJECT
  static Automation::Executor quiet() {
    return [](const QStringList &, auto done) { done(0, {}); };
  }
  static QVariantMap job(const Automation &automation, const QString &id) {
    for (const auto &v : automation.categorizedDownloads())
      if (v.toMap().value("id") == id)
        return v.toMap();
    return {};
  }
  static QVariantMap extensionRule(const QString &directory) {
    return {{"name", "Research"},
            {"extensions", QStringList{"*.PDF", ".epub"}},
            {"directory", directory}};
  }
private slots:
  void savedRulesRouteActualFilesAndExplicitDirectoriesWin() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto state = temp.path() + "/state",
               output = temp.path() + "/research";
    CategoryServer server, siteServer;
    QString pdf, site, fallback, explicitFile, siteDefault;
    QVariantList saved;
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(automation.configure(
          {{"directory", temp.path() + "/default"},
           {"notifications", false},
           {"categories",
            QVariantList{extensionRule(output),
                         QVariantMap{{"name", "Local site"},
                                     {"origins", QStringList{siteServer.url()}},
                                     {"directory", temp.path() + "/site"}}}}}));
      saved = automation.preferences().value("categories").toList();
      QCOMPARE(saved.first().toMap().value("extensions").toStringList(),
               QStringList({"pdf", "epub"}));
      pdf = automation.enqueue({{"url", server.url("paper.PDF")}});
      site = automation.enqueue({{"url", siteServer.url("page.bin")}});
      fallback = automation.enqueue({{"url", server.url("song.mp3")}});
      explicitFile =
          automation.enqueue({{"url", server.url("explicit.pdf")},
                              {"directory", temp.path() + "/chosen"}});
      QVERIFY(!pdf.isEmpty() && !site.isEmpty() && !fallback.isEmpty() &&
              !explicitFile.isEmpty());
      // A saved site destination retains its existing precedence over a
      // category default.
      QVERIFY(automation.configure(
          {{"siteSettings",
            QVariantMap{{automation.siteOrigin(server.url()),
                         QVariantMap{{"directory",
                                      temp.path() + "/site-default"}}}}}}));
      siteDefault =
          automation.enqueue({{"url", server.url("site-default.pdf")}});
      QVERIFY(!siteDefault.isEmpty());
      for (const auto &id : {pdf, site, fallback, explicitFile, siteDefault})
        QTRY_COMPARE_WITH_TIMEOUT(
            job(automation, id).value("status").toString(), "complete", 15000);
      QCOMPARE(job(automation, pdf).value("path").toString(),
               output + "/paper.PDF");
      QCOMPARE(job(automation, site).value("path").toString(),
               temp.path() + "/site/page.bin");
      QCOMPARE(job(automation, fallback).value("path").toString(),
               temp.path() + "/default/song.mp3");
      QCOMPARE(job(automation, explicitFile).value("path").toString(),
               temp.path() + "/chosen/explicit.pdf");
      QCOMPARE(job(automation, siteDefault).value("path").toString(),
               temp.path() + "/site-default/site-default.pdf");
      for (const auto &id : {pdf, site, fallback, explicitFile, siteDefault}) {
        QFile file(job(automation, id).value("path").toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), server.body);
      }
      QCOMPARE(job(automation, pdf).value("category").toString(), "Research");
      QCOMPARE(job(automation, site).value("category").toString(),
               "Local site");
      QCOMPARE(job(automation, fallback).value("category").toString(), "Music");
    }
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QCOMPARE(automation.preferences().value("categories").toList(), saved);
      QCOMPARE(job(automation, pdf).value("category").toString(), "Research");
      QCOMPARE(job(automation, site).value("category").toString(),
               "Local site");
      QCOMPARE(automation.downloadDirectoryFor(
                   {{"url", "https://elsewhere.test/new.pdf"}}),
               output);
      QVERIFY(automation.configure({{"categories", QVariantList{}}}));
      QCOMPARE(job(automation, pdf).value("category").toString(), "Documents");
      QVERIFY(QFile::exists(output + "/paper.PDF"));
    }
  }
  void collisionNamesRetainClassificationAcrossRestartAndRename() {
    QTemporaryDir temp;
    const auto state = temp.path() + "/state",
               output = temp.path() + "/research";
    CategoryServer server;
    QString id;
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(automation.configure(
          {{"categories", QVariantList{extensionRule(output)}},
           {"notifications", false}}));
      QVERIFY(QDir().mkpath(output));
      QFile existing(output + "/paper.pdf");
      QVERIFY(existing.open(QIODevice::WriteOnly));
      QCOMPARE(existing.write("existing"), 8);
      existing.close();
      id = automation.enqueue({{"url", server.url("paper.pdf")}});
      QVERIFY(!id.isEmpty());
      QTRY_COMPARE_WITH_TIMEOUT(job(automation, id).value("status").toString(),
                                "complete", 15000);
      QCOMPARE(job(automation, id).value("category").toString(), "Research");
      QVERIFY(job(automation, id).value("path").toString() !=
              existing.fileName());
      QVERIFY(existing.open(QIODevice::ReadOnly));
      QCOMPARE(existing.readAll(), "existing");
      QFile file(job(automation, id).value("path").toString());
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll(), server.body);
    }
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QCOMPARE(job(automation, id).value("category").toString(), "Research");
      QVERIFY(automation.rename(id, "renamed.txt"));
      QCOMPARE(job(automation, id).value("category").toString(), "Documents");
      QVERIFY(automation.rename(id, "renamed.epub"));
      QCOMPARE(job(automation, id).value("category").toString(), "Research");
      const QVariantMap audioRule{{"name", "Audio captures"},
                                  {"extensions", QStringList{"mka"}},
                                  {"directory", temp.path() + "/audio"}};
      QVERIFY(automation.configure({{"categories", QVariantList{audioRule}}}));
      QCOMPARE(
          automation.downloadDirectoryFor({{"url", "https://example.com/watch"},
                                           {"media", true},
                                           {"format", "audio"}}),
          temp.path() + "/audio");
    }
  }
  void originsAreExactAndFirstMatchingRuleWins() {
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/default"},
         {"categories",
          QVariantList{
              QVariantMap{{"name", "Site"},
                          {"origins", QStringList{"https://EXAMPLE.com/"}},
                          {"directory", temp.path() + "/site"}},
              extensionRule(temp.path() + "/research")}}}));
    for (const auto &url : {"https://example.com/file.bin",
                            "https://example.com:443/file.bin?x=1#part"}) {
      const auto id = automation.enqueue({{"url", url}, {"start", false}});
      QVERIFY(!id.isEmpty());
      QCOMPARE(job(automation, id).value("category").toString(), "Site");
      QCOMPARE(QFileInfo(job(automation, id).value("path").toString())
                   .absolutePath(),
               temp.path() + "/site");
    }
    for (const auto &url :
         {"http://example.com/file.bin", "https://example.com:444/file.bin",
          "https://sub.example.com/file.bin",
          "https://example.com.evil.test/file.bin"}) {
      const auto id = automation.enqueue({{"url", url}, {"start", false}});
      QVERIFY(!id.isEmpty());
      QCOMPARE(job(automation, id).value("category").toString(), "Other");
      QCOMPARE(QFileInfo(job(automation, id).value("path").toString())
                   .absolutePath(),
               temp.path() + "/default");
    }
    QCOMPARE(automation.downloadDirectoryFor(
                 {{"url", "https://elsewhere.test/file.pdf"},
                  {"name", "ignored.txt"}}),
             temp.path() + "/research");
    QCOMPARE(automation.downloadDirectoryFor(
                 {{"url", "https://example.com/file.pdf"}}),
             temp.path() + "/site");
    QCOMPARE(automation.downloadDirectoryFor(
                 {{"url", "https://elsewhere.test/noextension"},
                  {"filename", "chosen.PDF"}}),
             temp.path() + "/research");
    QCOMPARE(automation.downloadDirectoryFor(
                 {{"url", "https://elsewhere.test/file.pdf"},
                  {"filename", "chosen.txt"}}),
             temp.path() + "/default");
    QCOMPARE(automation.downloadDirectoryFor(
                 {{"url", "https://example.com/file.pdf"},
                  {"directory", temp.path() + "/explicit"}}),
             temp.path() + "/explicit");
  }
  void rejectsInvalidRulesWithoutChangingSavedPreferences() {
    QTemporaryDir temp;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    const auto valid = extensionRule(temp.path() + "/output");
    QVERIFY(automation.configure({{"categories", QVariantList{valid}}}));
    const auto saved = automation.preferences();
    const QVariantList invalid{
        QVariantMap{{"name", "Other"}, {"extensions", QStringList{"pdf"}}},
        QVariantMap{{"name", "Empty"}},
        QVariantMap{{"name", "Bad"}, {"extensions", "pdf"}},
        QVariantMap{{"name", "Bad"}, {"extensions", QStringList{"../pdf"}}},
        QVariantMap{{"name", "Bad"}, {"extensions", QStringList{"*"}}},
        QVariantMap{{"name", "Bad"}, {"origins", QStringList{"example.com"}}},
        QVariantMap{{"name", "Bad"},
                    {"origins", QStringList{"https://example.com/path"}}},
        QVariantMap{{"name", "Bad"},
                    {"origins", QStringList{"https://example.com?query"}}},
        QVariantMap{
            {"name", "Bad"},
            {"origins", QStringList{"https://user:secret@example.com"}}},
        QVariantMap{{"name", "Bad"},
                    {"origins", QStringList{"file:///tmp/file"}}},
        QVariantMap{{"name", "Bad"},
                    {"extensions", QStringList{"pdf"}},
                    {"directory", "relative"}},
        QVariantMap{{"name", "Bad"},
                    {"extensions", QStringList{"pdf"}},
                    {"directory", QString("/tmp/a\nb")}},
        QVariantMap{{"name", "Bad"},
                    {"extensions", QStringList{"pdf"}},
                    {"extra", true}},
        QString("not a map")};
    for (const auto &rule : invalid) {
      QVERIFY2(!automation.configure({{"categories", QVariantList{rule}}}),
               qPrintable(rule.toString()));
      QCOMPARE(automation.preferences(), saved);
    }
    auto duplicate = valid;
    duplicate["name"] = " research ";
    QVERIFY(!automation.configure(
        {{"categories", QVariantList{valid, duplicate}}}));
    QVERIFY(!automation.configure({{"categories", QVariantMap{}}}));
    QFile file(temp.path() + "/state/automation.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(file.readAll())
                 .object()
                 .value("preferences")
                 .toObject()
                 .value("categories"),
             QJsonValue::fromVariant(saved.value("categories")));
  }
  void savedInvalidRulesFailClosed() {
    QTemporaryDir temp;
    const auto state = temp.path() + "/state";
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(automation.configure(
          {{"categories", QVariantList{extensionRule(temp.path() + "/out")}}}));
    }
    QFile file(state + "/automation.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto preferences = root.value("preferences").toObject();
    preferences["categories"] = QJsonObject{{"bad", true}};
    root["preferences"] = preferences;
    const auto corrupt = QJsonDocument(root).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(corrupt), corrupt.size());
    file.close();
    {
      DownloadManager manager(state);
      Automation automation(&manager, state, nullptr, quiet());
      QVERIFY(!automation.error().isEmpty());
      QVERIFY(automation.preferences().value("categories").toList().isEmpty());
      QVERIFY(automation.enqueue({{"url", "https://example.com/file.pdf"}})
                  .isEmpty());
      QVERIFY(!automation.configure({{"categories", QVariantList{}}}));
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), corrupt);
  }
  void qmlControlsManageRulesAndKeepRowsAndCountsConsistent() {
    QTemporaryDir temp;
    CategoryServer server;
    DownloadManager manager(temp.path() + "/state");
    Automation automation(&manager, temp.path() + "/state", nullptr, quiet());
    QVERIFY(automation.configure(
        {{"directory", temp.path() + "/default"}, {"notifications", false}}));
    Theme theme(temp.path());
    Translator translator(temp.path() + "/language");
    PortalPicker picker;
    QQmlApplicationEngine engine;
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, this,
            [&](const QList<QQmlError> &errors) {
              for (const auto &e : errors)
                warnings << e.toString();
            });
    engine.rootContext()->setContextProperty("backend", &manager);
    engine.rootContext()->setContextProperty("automation", &automation);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("picker", &picker);
    engine.load(QUrl("qrc:/Main.qml"));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto find = [&](const char *name) {
      return findObject(window, QString::fromUtf8(name));
    };
    auto click = [&](const char *name) {
      auto *item = qobject_cast<QQuickItem *>(find(name));
      if (!item || !item->isVisible())
        return false;
      QTest::mouseClick(
          window, Qt::LeftButton, Qt::NoModifier,
          item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
              .toPoint());
      return true;
    };
    QVERIFY(find("manageCategoriesAction"));
    QVERIFY(
        QMetaObject::invokeMethod(find("manageCategoriesAction"), "triggered"));
    auto *dialog = find("categoryDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    find("categoryName")->setProperty("text", "Research");
    find("categoryExtensions")->setProperty("text", "*.pdf, .epub");
    find("categoryDirectory")->setProperty("text", temp.path() + "/research");
    QVERIFY(click("saveCategoryButton"));
    QCOMPARE(automation.preferences().value("categories").toList().size(), 1);
    find("categoryOrigins")->setProperty("text", "https://example.com/path");
    QVERIFY(click("saveCategoryButton"));
    QVERIFY(!dialog->property("failure").toString().isEmpty());
    find("categoryOrigins")->setProperty("text", "https://example.com");
    QVERIFY(click("saveCategoryButton"));
    QVERIFY(dialog->property("failure").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    find("urlInput")->setProperty("text", server.url("paper.pdf"));
    QVERIFY(QMetaObject::invokeMethod(window, "prepareInformation"));
    QTRY_VERIFY(find("infoDialog")->property("visible").toBool());
    QCOMPARE(find("directoryInput")->property("text").toString(),
             temp.path() + "/research");
    find("filenameInput")->setProperty("text", "renamed.txt");
    QCOMPARE(find("directoryInput")->property("text").toString(),
             temp.path() + "/default");
    find("filenameInput")->setProperty("text", "paper.pdf");
    QVERIFY(click("startDownloadButton"));
    QTRY_COMPARE(manager.downloads().size(), 1);
    const auto id = manager.downloads().first().toMap().value("id").toString();
    QTRY_COMPARE_WITH_TIMEOUT(job(automation, id).value("status").toString(),
                              "complete", 15000);
    QFile output(temp.path() + "/research/paper.pdf");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), server.body);
    window->setProperty("filesExpanded", true);
    QTRY_VERIFY(find("navigation-category:Research"));
    QTest::qWait(50);
    QVERIFY(click("navigation-category:Research"));
    QCOMPARE(window->property("filter").toString(), "category:Research");
    QTRY_COMPARE(
        window->property("shown").value<QJSValue>().toVariant().toList().size(),
        1);
    QVariant count;
    QVERIFY(QMetaObject::invokeMethod(window, "navigationCount",
                                      Q_RETURN_ARG(QVariant, count),
                                      Q_ARG(QVariant, "category:Research")));
    QCOMPARE(count.toString(), "1");
    const auto labelName = "downloadCategory-" + id;
    QTRY_VERIFY(find(qPrintable(labelName)));
    QCOMPARE(find(qPrintable(labelName))->property("text").toString(),
             "Research");
    find("urlInput")->setProperty("text", server.url("second.pdf"));
    QVERIFY(QMetaObject::invokeMethod(window, "prepareInformation"));
    QTRY_VERIFY(find("infoDialog")->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY(click("directoryInput"));
    auto *directoryInput = qobject_cast<QQuickItem *>(find("directoryInput"));
    directoryInput->forceActiveFocus();
    QTRY_VERIFY(directoryInput->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const auto character : (temp.path() + "/chosen").toLatin1())
      QTest::keyClick(window, character);
    find("filenameInput")->setProperty("text", "second.epub");
    QCOMPARE(find("directoryInput")->property("text").toString(),
             temp.path() + "/chosen");
    QVERIFY(click("startDownloadButton"));
    QTRY_VERIFY(QFile::exists(temp.path() + "/chosen/second.epub"));
    QFile chosen(temp.path() + "/chosen/second.epub");
    QVERIFY(chosen.open(QIODevice::ReadOnly));
    QCOMPARE(chosen.readAll(), server.body);
    QVERIFY(
        QMetaObject::invokeMethod(find("manageCategoriesAction"), "triggered"));
    QVERIFY(
        QMetaObject::invokeMethod(dialog, "loadCategory", Q_ARG(QVariant, 0)));
    find("categoryName")->setProperty("text", "Papers");
    QVERIFY(click("saveCategoryButton"));
    QCOMPARE(job(automation, id).value("category").toString(), "Papers");
    QCOMPARE(window->property("filter").toString(), "all");
    QVERIFY(click("removeCategoryButton"));
    QVERIFY(automation.preferences().value("categories").toList().isEmpty());
    QCOMPARE(job(automation, id).value("category").toString(), "Documents");
    QVERIFY(QFile::exists(output.fileName()));
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }
};
int main(int argc, char **argv) {
  QStandardPaths::setTestModeEnabled(true);
  QGuiApplication app(argc, argv);
  QQuickStyle::setStyle("Basic");
  CategoryTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "category_tests.moc"
