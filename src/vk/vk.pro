QT += core network
QT -= gui

TEMPLATE = lib
CONFIG += staticlib c++14
CONFIG -= debug_and_release
CONFIG += release

TARGET  = VoiceKit
DESTDIR = $$PWD/../../lib

# 源码里允许直接写中文：输入/执行字符集都锁 UTF-8（MinGW 默认执行字符集不是 UTF-8，不加会乱码）
QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
QMAKE_CXXFLAGS += -Wall -Wextra

INCLUDEPATH += $$PWD

HEADERS += \
    VoiceKit.h \
    vk_types.h \
    vk_config.h \
    vk_apiclient.h \
    vk_engine.h \
    vk_charindex.h \
    vk_script.h \
    vk_pack.h \
    vk_join.h \
    vk_kit.h

SOURCES += \
    vk_config.cpp \
    vk_apiclient.cpp \
    vk_engine.cpp \
    vk_charindex.cpp \
    vk_script.cpp \
    vk_pack.cpp \
    vk_join.cpp \
    vk_kit.cpp
