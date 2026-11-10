#pragma once
#include <QObject>
#include <QLocalServer>
#include <QVariantMap>
#include <functional>

class BrowserBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString extensionDirectory READ extensionDirectory CONSTANT)
public:
    using Handler = std::function<QVariantMap(const QVariantMap &)>;
    BrowserBridge(const QString &state, Handler handler, bool ownQueue, QObject *parent = nullptr);
    bool available() const { return server.isListening(); }
    QString error() const { return failure; }
    QString extensionDirectory() const;
    Q_INVOKABLE QString install();
    static QString socketPath(const QString &state);
    static int nativeHost(const QString &state, const QString &executable);
signals:
    void changed();
private:
    QLocalServer server;
    Handler handler;
    QString failure;
};
