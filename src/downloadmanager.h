#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <memory>

// One manager owns transfers and their durable queue; QML receives snapshots.
class DownloadManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList downloads READ downloads NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory CONSTANT)
public:
    explicit DownloadManager(const QString &stateDirectory = {}, QObject *parent = nullptr);
    ~DownloadManager() override;
    QVariantList downloads() const;
    QString error() const;
    QString downloadDirectory() const;
    Q_INVOKABLE QString add(const QString &url, const QString &directory = {}, int connections = 4);
    Q_INVOKABLE QString addRequest(const QVariantMap &request);
    Q_INVOKABLE void pause(const QString &id);
    Q_INVOKABLE void resume(const QString &id);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    Q_INVOKABLE void removeCompleted();
    Q_INVOKABLE void setRateLimit(const QString &id, qint64 bytesPerSecond);
    Q_INVOKABLE bool rename(const QString &id, const QString &filename);
    Q_INVOKABLE bool refresh(const QString &id, const QString &url);
    Q_INVOKABLE bool updateContext(const QString &id, const QVariantMap &request);
signals:
    void changed();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
