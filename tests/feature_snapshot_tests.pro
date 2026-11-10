QT += core gui network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = feature_snapshot_tests
INCLUDEPATH += ../src
HEADERS += ../src/downloadmanager.h ../src/automation.h
SOURCES += feature_snapshot_tests.cpp ../src/downloadmanager.cpp ../src/automation.cpp
RESOURCES += ../src/resources.qrc
