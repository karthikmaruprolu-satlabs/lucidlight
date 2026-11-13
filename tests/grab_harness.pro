QT += core gui quick quickcontrols2 qml dbus network
CONFIG += c++17 console
TARGET = grab_harness
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/theme.h ../src/translator.h ../src/backend.h \
           ../src/seriallink.h ../src/commlink.h ../src/netlink.h ../src/portenumerator.h ../src/dataformat.h ../src/checksum.h ../src/filelogger.h \
           ../src/commlog.h ../src/commmanager.h ../src/channelsettings.h \
           ../src/sequencemodel.h ../src/sequencematcher.h \
           ../src/dlscript.h ../src/jshighlighter.h ../src/project.h ../src/portalpicker.h
SOURCES += grab_harness.cpp \
    ../src/theme.cpp ../src/translator.cpp ../src/backend.cpp \
    ../src/seriallink.cpp ../src/commlink.cpp ../src/netlink.cpp ../src/portenumerator.cpp ../src/dataformat.cpp ../src/checksum.cpp ../src/filelogger.cpp \
    ../src/commlog.cpp ../src/commmanager.cpp ../src/channelsettings.cpp \
    ../src/sequencemodel.cpp ../src/sequencematcher.cpp \
    ../src/dlscript.cpp ../src/vbstrans.cpp ../src/jshighlighter.cpp ../src/project.cpp ../src/portalpicker.cpp
RESOURCES += ../src/resources.qrc
