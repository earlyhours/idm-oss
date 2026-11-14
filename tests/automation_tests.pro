QT += core gui network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = automation_tests
INCLUDEPATH += ../src
HEADERS += ../src/automation.h ../src/downloadmanager.h
SOURCES += automation_tests.cpp ../src/automation.cpp ../src/downloadmanager.cpp
RESOURCES += ../src/resources.qrc

include(../native-io.pri)
