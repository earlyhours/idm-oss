QT += core network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = tls_tests
INCLUDEPATH += ../src
HEADERS += ../src/downloadmanager.h
SOURCES += tls_tests.cpp ../src/downloadmanager.cpp

include(../native-io.pri)
