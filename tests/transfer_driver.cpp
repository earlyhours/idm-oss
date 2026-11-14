// Headless manager process for the real-protocol fixtures in transfer_features_test.py.
#include "downloadmanager.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>
#include <cstdio>
#include <unistd.h>

int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2)return 2;
    DownloadManager manager(QString::fromLocal8Bit(argv[1]));
    auto snapshot=[&] {
        const auto json=QJsonDocument::fromVariant(QVariantMap{{"downloads",manager.downloads()},{"error",manager.error()}}).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(),1,size_t(json.size()),stdout);std::fputc('\n',stdout);std::fflush(stdout);
    };
    QObject::connect(&manager,&DownloadManager::changed,&app,snapshot);
    QByteArray input;
    QSocketNotifier notifier(STDIN_FILENO,QSocketNotifier::Read);
    QObject::connect(&notifier,&QSocketNotifier::activated,&app,[&] {
        char buffer[65536];const auto count=::read(STDIN_FILENO,buffer,sizeof(buffer));
        if(count<=0){app.quit();return;}input.append(buffer,count);
        while(input.contains('\n')) {
            const auto end=input.indexOf('\n');const auto command=QJsonDocument::fromJson(input.left(end)).object();input.remove(0,end+1);
            const auto action=command.value("action").toString(),id=command.value("id").toString();
            if(action=="add")manager.addRequest(command.value("request").toObject().toVariantMap());
            else if(action=="pause")manager.pause(id);
            else if(action=="resume")manager.resume(id);
            else if(action=="context")manager.updateContext(id,command.value("request").toObject().toVariantMap());
            else if(action=="quit"){app.quit();return;}
            snapshot();
        }
    });
    QTimer::singleShot(0,&app,snapshot);
    return app.exec();
}
