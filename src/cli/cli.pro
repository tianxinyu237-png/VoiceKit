QT += core network
QT -= gui

TEMPLATE = app
CONFIG += console c++14
CONFIG -= debug_and_release
CONFIG += release

TARGET  = vk
DESTDIR = $$PWD/../../bin

QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

INCLUDEPATH += $$PWD/../vk
LIBS += -L$$PWD/../../lib -lVoiceKit
PRE_TARGETDEPS += $$PWD/../../lib/libVoiceKit.a

SOURCES += main.cpp
