#pragma once
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
class DownloadManager;
class QProcess;

class Automation : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY changed)
  Q_PROPERTY(QVariantMap jobPolicies READ jobPolicies NOTIFY changed)
  Q_PROPERTY(QVariantList queues READ queues NOTIFY changed)
  Q_PROPERTY(QVariantMap grabber READ grabber NOTIFY changed)
  Q_PROPERTY(QVariantMap quota READ quota NOTIFY changed)
  Q_PROPERTY(QVariantList syncQueue READ syncQueue NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString completionNotice READ completionNotice NOTIFY changed)
  Q_PROPERTY(
      bool hasLegacySiteSettings READ hasLegacySiteSettings NOTIFY changed)
public:
  using Executor = std::function<void(
      const QStringList &, std::function<void(int, const QString &)>)>;
  Automation(DownloadManager *manager, const QString &state,
             QObject *parent = nullptr, Executor executor = {});
  ~Automation() override;
  QVariantMap preferences() const { return settings; }
  QVariantMap jobPolicies() const { return policies; }
  QVariantList queues() const;
  QVariantMap grabber() const { return mirrorState; }
  QVariantMap quota() const;
  QVariantList syncQueue() const;
  QString error() const { return failure; }
  QString completionNotice() const { return notice; }
  bool hasLegacySiteSettings() const { return !legacySiteSettings.isEmpty(); }
  Q_INVOKABLE QString siteOrigin(const QString &url) const;
  Q_INVOKABLE QVariantMap siteSettingsForEditing(const QString &url) const;
  Q_INVOKABLE bool configure(const QVariantMap &options);
  Q_INVOKABLE QString enqueue(QVariantMap request);
  Q_INVOKABLE QVariantMap batch(const QVariantList &requests);
  Q_INVOKABLE void pause(const QString &id);
  Q_INVOKABLE void resume(const QString &id);
  Q_INVOKABLE void pauseAll();
  Q_INVOKABLE void resumeAll();
  Q_INVOKABLE void remove(const QString &id);
  Q_INVOKABLE void removeCompleted();
  Q_INVOKABLE bool setJobPolicy(const QString &id, const QVariantMap &policy);
  Q_INVOKABLE bool setQueue(const QString &name, const QVariantMap &policy);
  Q_INVOKABLE void startQueue(const QString &name = "Main download queue");
  Q_INVOKABLE void stopQueue(const QString &name = "Main download queue");
  Q_INVOKABLE bool importList(const QString &path);
  Q_INVOKABLE bool exportList(const QString &path);
  Q_INVOKABLE bool refresh(const QString &id, const QString &url);
  Q_INVOKABLE bool rename(const QString &id, const QString &filename);
  Q_INVOKABLE bool mirror(const QString &url, const QString &directory,
                          int depth = 1, const QVariantMap &filters = {});
  Q_INVOKABLE void cancelMirror();
  Q_INVOKABLE void cancelCompletionAction();
  Q_INVOKABLE void checkSchedules();
signals:
  void changed();
  void clipboardUrl(const QString &url);

private:
  DownloadManager *manager;
  QString state, failure, notice;
  QVariantMap settings, queueSettings, policies, mirrorState,
      legacySiteSettings;
  QVariantMap quotaState, projectResults, pendingMirror;
  QString workerKey, workerList;
  qint64 workerTransferred = 0;
  QSet<QString> processed, scanning, unsafe;
  QTimer tick, completionTimer;
  QProcess *mirrorProcess = nullptr;
  Executor executor;
  bool pending = false, reconciling = false;
  bool settingsWritable = true, accountingDirty = false;
  qint64 lastRate = -1;
  bool save();
  bool updateQuota();
  bool quotaBlocked() const;
  bool startWorker(QVariantMap job, const QString &list = {},
                   const QString &key = {});
  void readWorkerOutput(QProcess *process, QByteArray &buffer);
  void cancelWorker(const QString &status);
  void finishProject(const QString &status);
  void checkProjects();
  bool reject(const QString &error);
  void reconcile();
  void completed(const QVariantMap &job);
  void execute(const QStringList &command,
               std::function<void(int, const QString &)> done);
  void completionAction();
  qint64 effectiveRate() const;
};
