TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = vk cli gui

vk.file  = src/vk/vk.pro
cli.file = src/cli/cli.pro
gui.file = src/gui/vk_gui.pro
