QT += core gui network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = final_parity_tests
INCLUDEPATH += ../src
HEADERS += ../src/downloadmanager.h ../src/automation.h
SOURCES += final_parity_tests.cpp ../src/downloadmanager.cpp ../src/automation.cpp
RESOURCES += ../src/resources.qrc
