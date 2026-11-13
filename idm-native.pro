QT += core gui qml quick quickcontrols2 network dbus
CONFIG += c++17 release
TARGET = idm-native
TEMPLATE = app
HEADERS += src/downloadmanager.h src/theme.h src/translator.h src/portalpicker.h src/browserbridge.h src/automation.h
SOURCES += src/main.cpp src/downloadmanager.cpp src/theme.cpp src/translator.cpp src/portalpicker.cpp src/browserbridge.cpp src/automation.cpp
RESOURCES += src/resources.qrc
