QT += core gui qml quick quickcontrols2 network testlib dbus
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = category_tests
INCLUDEPATH += ../src
HEADERS += ../src/automation.h ../src/categoryrules.h ../src/downloadmanager.h ../src/theme.h ../src/translator.h ../src/portalpicker.h
SOURCES += category_tests.cpp ../src/automation.cpp ../src/downloadmanager.cpp ../src/theme.cpp ../src/translator.cpp ../src/portalpicker.cpp
RESOURCES += ../src/resources.qrc

include(../native-io.pri)
