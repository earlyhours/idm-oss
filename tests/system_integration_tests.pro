QT += core gui network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = system_integration_tests
INCLUDEPATH += ../src
HEADERS += ../src/automation.h ../src/downloadmanager.h
SOURCES += system_integration_tests.cpp ../src/automation.cpp ../src/downloadmanager.cpp
RESOURCES += ../src/resources.qrc
