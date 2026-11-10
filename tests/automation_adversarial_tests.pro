QT += core gui network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = automation_adversarial_tests
INCLUDEPATH += ../src
HEADERS += ../src/automation.h ../src/downloadmanager.h ../src/browserbridge.h
SOURCES += automation_adversarial_tests.cpp ../src/automation.cpp ../src/downloadmanager.cpp ../src/browserbridge.cpp
RESOURCES += ../src/resources.qrc
