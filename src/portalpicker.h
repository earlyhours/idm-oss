#pragma once
#include <QObject>
#include <QVariantMap>
class PortalPicker : public QObject {
    Q_OBJECT
public:
    explicit PortalPicker(QObject *parent = nullptr) : QObject(parent) {}
    Q_INVOKABLE void open();
signals:
    void selected(const QString &directory);
    void error(const QString &message);
private slots:
    void response(uint code, const QVariantMap &results);
private:
    QString path;
};
