QT += core gui qml quick quickcontrols2 network testlib dbus
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = desktop_tests
INCLUDEPATH += ../src
HEADERS += ../src/desktopintegration.h ../src/downloadmanager.h ../src/theme.h ../src/translator.h ../src/portalpicker.h
SOURCES += desktop_tests.cpp ../src/desktopintegration.cpp ../src/downloadmanager.cpp ../src/theme.cpp ../src/translator.cpp ../src/portalpicker.cpp
RESOURCES += ../src/resources.qrc

include(../native-io.pri)
