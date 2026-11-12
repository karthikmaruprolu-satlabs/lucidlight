QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = lucidlight
TEMPLATE = app

HEADERS += \
    src/theme.h \
    src/translator.h \
    src/seriallink.h \
    src/portenumerator.h \
    src/dataformat.h \
    src/commlog.h \
    src/commmanager.h \
    src/channelsettings.h \
    src/sequencemodel.h \
    src/sequencematcher.h \
    src/dlscript.h \
    src/jshighlighter.h \
    src/project.h \
    src/portalpicker.h \
    src/backend.h

SOURCES += \
    src/main.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/seriallink.cpp \
    src/portenumerator.cpp \
    src/dataformat.cpp \
    src/commlog.cpp \
    src/commmanager.cpp \
    src/channelsettings.cpp \
    src/sequencemodel.cpp \
    src/sequencematcher.cpp \
    src/dlscript.cpp \
    src/jshighlighter.cpp \
    src/project.cpp \
    src/portalpicker.cpp \
    src/backend.cpp

RESOURCES += src/resources.qrc
