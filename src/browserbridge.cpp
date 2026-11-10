#include "browserbridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <memory>

namespace {
constexpr quint32 limit = 1024 * 1024;
QByteArray frame(const QVariantMap &object) {
    const auto json = QJsonDocument(QJsonObject::fromVariantMap(object)).toJson(QJsonDocument::Compact);
    QByteArray result(4, '\0');
    qToLittleEndian<quint32>(quint32(json.size()), result.data());
    return result + json;
}
QVariantMap parse(const QByteArray &bytes) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object().toVariantMap();
}
QString shareDirectory() {
    const QString beside = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    if (QFileInfo::exists(beside + "/browser/chromium/manifest.json")) return beside;
    return "/usr/share/idm-native";
}
QVariantMap failure(const QString &message) { return {{"ok",false},{"error",message}}; }
QString shellQuote(QString value) { return "'" + value.replace("'", "'\"'\"'") + "'"; }
}

QString BrowserBridge::socketPath(const QString &state) { return QDir(state).absoluteFilePath("browser.sock"); }
BrowserBridge::BrowserBridge(const QString &state, Handler callback, bool ownQueue, QObject *parent)
    : QObject(parent), handler(std::move(callback)) {
    if (!ownQueue) { failure = tr("Another app owns this queue; browser integration is handled there."); return; }
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketPath(state));
    if (!server.listen(socketPath(state))) { failure = server.errorString(); return; }
    connect(&server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            QTimer::singleShot(10000,socket,[socket] { socket->disconnectFromServer(); });
            connect(socket,&QLocalSocket::readyRead,this,[this,socket,buffer] {
                buffer->append(socket->readAll());
                if (buffer->size() > limit + 4) { socket->abort(); return; }
                if (buffer->size() < 4) return;
                const auto size = qFromLittleEndian<quint32>(buffer->constData());
                if (!size || size > limit) { socket->write(frame(::failure(tr("Invalid browser message size.")))); socket->disconnectFromServer(); return; }
                if (buffer->size() < size + 4) return;
                const auto request = parse(buffer->mid(4, size));
                buffer->remove(0, size + 4);
                const auto command = request.value("command").toString();
                QVariantMap result;
                if (command == "ping") result = {{"ok", true}, {"name", "IDM Native"}, {"version", QCoreApplication::applicationVersion()}};
                else if (command == "download" || command == "batch") result = handler(request);
                else result = ::failure(tr("Unsupported browser command."));
                socket->write(frame(result));
                socket->disconnectFromServer();
            });
        }
    });
}
QString BrowserBridge::extensionDirectory() const { return shareDirectory() + "/browser/chromium"; }
QString BrowserBridge::install() {
    const QString script = shareDirectory() + "/install-browser.py";
    QProcess process;
    process.start("/usr/bin/python3", {script, "--binary", QCoreApplication::applicationFilePath(), "--share", shareDirectory()});
    if (!process.waitForFinished(5000) || process.exitCode() != 0) {
        process.kill(); process.waitForFinished();
        failure = tr("Browser integration installation failed: %1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        emit changed(); return {};
    }
    failure.clear(); emit changed(); return extensionDirectory();
}

int BrowserBridge::nativeHost(const QString &state, const QString &executable) {
    auto reply = [](const QVariantMap &message) {
        const auto bytes = frame(message);
        return std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout)==size_t(bytes.size()) && std::fflush(stdout)==0;
    };
    for (;;) {
        char header[4]; const auto count = std::fread(header,1,4,stdin);
        if (!count && std::feof(stdin)) return 0;
        if (count != 4) { reply(::failure("Truncated native message header.")); return 1; }
        const quint32 size = qFromLittleEndian<quint32>(header);
        if (!size || size > limit) { reply(::failure("Native message exceeds the 1 MiB limit.")); return 1; }
        QByteArray bytes(int(size),Qt::Uninitialized);
        if (std::fread(bytes.data(),1,size,stdin) != size) { reply(::failure("Truncated native message body.")); return 1; }
        const auto request = parse(bytes);
        if (request.isEmpty()) { if (!reply(::failure("Expected a JSON object."))) return 1;
                continue; }
        QLocalSocket socket;
        socket.connectToServer(socketPath(state));
        if (!socket.waitForConnected(300)) {
            socket.abort();
            const auto hyprctl = QStandardPaths::findExecutable("hyprctl");
            if (hyprctl.isEmpty() || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
                if (!reply(::failure("Open IDM Native before sending browser downloads."))) return 1;
                continue;
            }
            const QString cmd = "[workspace 5 silent] " + shellQuote(executable) + " open --state-directory " + shellQuote(state);
            auto quoted = QString::fromUtf8(QJsonDocument(QJsonArray{cmd}).toJson(QJsonDocument::Compact));
            quoted = quoted.mid(1,quoted.size()-2);
            if (!QProcess::startDetached(hyprctl,{"dispatch", "hl.dsp.exec_cmd(" + quoted + ")"})) {
                if (!reply(::failure("Could not start IDM Native."))) return 1;
                continue;
            }
            for (int i=0;i<40 && socket.state()!=QLocalSocket::ConnectedState;++i) {
                socket.abort();socket.connectToServer(socketPath(state));
                if (!socket.waitForConnected(100)) QThread::msleep(50);
            }
        }
        if (socket.state()!=QLocalSocket::ConnectedState) { if (!reply(::failure("IDM Native did not become available. Open the app and retry."))) return 1;
                continue; }
        socket.write(frame(request));
        if (socket.bytesToWrite()>0 && !socket.waitForBytesWritten(5000)) { if (!reply(::failure("Could not send browser request."))) return 1;
                continue; }
        QByteArray response = socket.readAll();
        while (response.size()<4 || (response.size()>=4 && response.size()<4+qFromLittleEndian<quint32>(response.constData()))) {
            if (response.size()>=4 && qFromLittleEndian<quint32>(response.constData())>limit) break;
            if (socket.bytesAvailable()==0 && !socket.waitForReadyRead(10000)) break;
            response += socket.readAll();
            if (response.size()>limit+4) break;
        }
        QVariantMap result;
        if (response.size()>=4) {
            const auto length = qFromLittleEndian<quint32>(response.constData());
            if (length>0 && length<=limit && response.size()==length+4) result=parse(response.mid(4));
        }
        if (result.isEmpty()) result=::failure("IDM Native returned an incomplete response.");
        if (!reply(result)) return 1;
    }
}
