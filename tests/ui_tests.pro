QT += core gui qml quick quickcontrols2 network testlib dbus
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = ui_tests
INCLUDEPATH += ../src
HEADERS += ../src/downloadmanager.h ../src/theme.h ../src/translator.h
SOURCES += ui_tests.cpp ../src/downloadmanager.cpp ../src/theme.cpp ../src/translator.cpp
RESOURCES += ../src/resources.qrc

HEADERS += ../src/portalpicker.h
SOURCES += ../src/portalpicker.cpp

HEADERS += ../src/automation.h
SOURCES += ../src/automation.cpp
