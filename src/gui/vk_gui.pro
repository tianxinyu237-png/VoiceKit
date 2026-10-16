QT += core gui widgets

# mciSendString：播 wav 用。
# 为什么不用 QMediaPlayer：MinGW 上它走 DirectShow 后端插件，
# 出问题是"点了没反应、也不报错"，很难查（实测踩过）。MCI 是 Win32 自带，稳。
LIBS += -lwinmm

TARGET   = vk_gui
TEMPLATE = app
CONFIG  += c++11
CONFIG  -= console

DESTDIR = $$PWD/../../bin

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    vkrunner.cpp

HEADERS += \
    mainwindow.h \
    vkrunner.h
