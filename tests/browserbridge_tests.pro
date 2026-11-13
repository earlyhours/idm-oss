QT += core network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = browserbridge_tests
INCLUDEPATH += ../src
HEADERS += ../src/browserbridge.h
SOURCES += browserbridge_tests.cpp ../src/browserbridge.cpp
