#include "portalpicker.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUrl>
#include <QUuid>

void PortalPicker::open() {
    if (!path.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { emit error(tr("The desktop file chooser is unavailable. Enter the folder path instead.")); return; }
    const auto token = "idm" + QUuid::createUuid().toString(QUuid::Id128);
    auto sender = bus.baseService().mid(1); sender.replace('.', '_');
    path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    if (!bus.connect("org.freedesktop.portal.Desktop", path, "org.freedesktop.portal.Request", "Response", this, SLOT(response(uint,QVariantMap)))) {
        path.clear(); emit error(tr("Cannot connect to the desktop file chooser.")); return;
    }
    QDBusInterface chooser("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser", bus);
    QVariantMap options{{"handle_token", token}, {"directory", true}, {"multiple", false}, {"modal", true}};
    auto watcher = new QDBusPendingCallWatcher(chooser.asyncCall("OpenFile", "", tr("Choose download folder"), options), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected = path](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError() || reply.value().path() != expected) {
            QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", path, "org.freedesktop.portal.Request", "Response", this, SLOT(response(uint,QVariantMap)));
            path.clear(); emit error(tr("The desktop file chooser failed. Enter the folder path instead."));
        }
        w->deleteLater();
    });
}

void PortalPicker::response(uint code, const QVariantMap &results) {
    QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", path, "org.freedesktop.portal.Request", "Response", this, SLOT(response(uint,QVariantMap)));
    path.clear();
    if (code == 1) return;
    const auto uris = results.value("uris").toStringList();
    const QUrl url(uris.value(0));
    if (code != 0 || !url.isLocalFile()) { emit error(tr("The desktop file chooser did not return a local folder.")); return; }
    emit selected(url.toLocalFile());
}
