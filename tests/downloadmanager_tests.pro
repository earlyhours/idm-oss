QT += core network testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = downloadmanager_tests
INCLUDEPATH += ../src
HEADERS += ../src/downloadmanager.h
SOURCES += downloadmanager_tests.cpp ../src/downloadmanager.cpp

RESOURCES += ../src/resources.qrc

include(../native-io.pri)
