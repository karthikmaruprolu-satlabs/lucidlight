QT += core gui quick quickcontrols2 qml testlib dbus
CONFIG += c++17 testcase
TARGET = lucidlight_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/theme.h ../src/translator.h ../src/backend.h \
           ../src/seriallink.h ../src/portenumerator.h ../src/dataformat.h ../src/checksum.h \
           ../src/commlog.h ../src/commmanager.h ../src/channelsettings.h \
           ../src/sequencemodel.h ../src/sequencematcher.h \
           ../src/dlscript.h ../src/jshighlighter.h ../src/project.h ../src/portalpicker.h
SOURCES += lucidlight_tests.cpp \
    ../src/theme.cpp ../src/translator.cpp ../src/backend.cpp \
    ../src/seriallink.cpp ../src/portenumerator.cpp ../src/dataformat.cpp ../src/checksum.cpp \
    ../src/commlog.cpp ../src/commmanager.cpp ../src/channelsettings.cpp \
    ../src/sequencemodel.cpp ../src/sequencematcher.cpp \
    ../src/dlscript.cpp ../src/jshighlighter.cpp ../src/project.cpp ../src/portalpicker.cpp
