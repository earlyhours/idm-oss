#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class DesktopIntegration : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap layout READ layout NOTIFY changed)
    Q_PROPERTY(bool autostart READ autostart NOTIFY changed)
    Q_PROPERTY(bool automaticUpdateChecks READ automaticUpdateChecks NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY updateChanged)
    Q_PROPERTY(QVariantMap release READ release NOTIFY updateChanged)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY updateChanged)
    Q_PROPERTY(int dragDistance READ dragDistance CONSTANT)
public:
    explicit DesktopIntegration(const QString &stateDirectory, const QString &executable = {},
                                QNetworkAccessManager *network = nullptr, QObject *parent = nullptr);
    QVariantMap layout() const;
    bool autostart() const;
    bool automaticUpdateChecks() const { return settings.value("automaticUpdateChecks", false).toBool(); }
    Q_INVOKABLE bool setAutomaticUpdateChecks(bool enabled);
    Q_INVOKABLE void checkScheduledUpdates();
    QString error() const { return failure; }
    bool checking() const { return pending; }
    QVariantMap release() const { return latest; }
    QString updateStatus() const { return updateMessage; }
    int dragDistance() const;
    Q_INVOKABLE bool setLayout(const QString &kind, const QStringList &order, const QStringList &hidden);
    Q_INVOKABLE bool resetLayout();
    Q_INVOKABLE bool setAutostart(bool enabled);
    Q_INVOKABLE bool installUrlHandler();
    Q_INVOKABLE bool dragCompleted(const QVariantMap &job, QObject *window);
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE bool openRelease();
    static QList<QUrl> completedFileUrls(const QVariantMap &job);
    static QUrl externalUrl(QString text);
    static QString desktopExec(const QString &executable);
    static QVariantMap parseRelease(const QByteArray &json, const QString &currentVersion);
signals:
    void changed();
    void updateChanged();
    void fileDragStarted(const QList<QUrl> &urls);
private:
    QSettings settings;
    QString executable, stateDirectory, failure, updateMessage;
    QNetworkAccessManager ownedNetwork;
    QNetworkAccessManager *network;
    QTimer updateTimer;
    bool pending = false;
    QVariantMap latest;
    QString autostartPath() const;
    QByteArray desktopEntry(const QString &arguments) const;
    bool writeEntry(const QString &path, const QByteArray &contents);
    bool reject(const QString &message);
};
