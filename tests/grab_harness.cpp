// Offscreen harness: loads the real QML, drives it, and grabs the window to
// a PNG so agents can see what the user would see.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QFile>
#include <QTimer>
#include <cstdlib>

#include "backend.h"
#include "jshighlighter.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);
    // LUCID_NO_THEME=1 falls back to the built-in palette, so generated
    // screenshots do not depend on the user's current desktop theme.
    Theme theme(qEnvironmentVariableIsSet("LUCID_NO_THEME") ? QStringLiteral("/nonexistent") : QString());
    Backend backend;

    QQmlApplicationEngine engine;
    qmlRegisterType<JsHighlighter>("Lucidlight", 1, 0, "JsHighlighter");
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    // Optional URL override, for probing single QML files.
    const QString url = argc > 1 && QString::fromLocal8Bit(argv[1]).startsWith("file:")
                            ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("qrc:/Main.qml");
    engine.load(QUrl(url));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    window->show();

    // Optional positional argument: a project (.llproj) to open before the
    // grab, so dialogs can be exercised against real sequence data.
    if (argc > 1 && !QString::fromLocal8Bit(argv[1]).startsWith("file:")) {
        const QString projectPath = QString::fromLocal8Bit(argv[1]);
        if (QFile::exists(projectPath))
            backend.openProjectFrom(projectPath);
    }

    // Let bindings settle, then grab.
    QTimer::singleShot(700, &app, [&] {
        // Simulation mode: paint a few representative entries so screenshots
        // and layout checks show real traffic.
        if (!qEnvironmentVariableIsEmpty("LUCID_SIM")) {
            backend.log()->append(CommLog::Rx, 0, "PING 12 34\r\n");
            backend.log()->append(CommLog::Tx, 0, "PONG\r\n");
            backend.log()->append(CommLog::Rx, 0, QByteArray::fromHex("48656C6C6F2C20776F726C642100"));
            backend.log()->appendComment("Script: handshake done");
            backend.log()->append(CommLog::Rx, 1, QByteArray::fromHex("0142FF00"), true);
            // Regression guard: the view must actually show the rows. A
            // broken delegate (e.g. required model properties on a nested
            // item) renders an empty window while the model has data.
            if (auto *view = window->findChild<QQuickItem *>("commView")) {
                const int viewCount = view->property("count").toInt();
                if (viewCount < backend.log()->count()) {
                    fprintf(stderr, "GRAB GUARD: view shows %d of %d rows\n",
                            viewCount, backend.log()->count());
                    std::_Exit(3);
                }
            }
        }
        // LUCID_KBD=1: exercise the keyboard console — start communication,
        // focus the communication window, and post real key events. The grab
        // then shows the typed characters as TX rows.
        if (!qEnvironmentVariableIsEmpty("LUCID_KBD")) {
            backend.setKeyboardConsole(true);
            // A UDP peer needs no partner, so TX rows appear without a device.
            backend.channelA()->setDevice("UDP:127.0.0.1:47111");
            backend.toggleComm();
            if (auto *view = window->findChild<QQuickItem *>("commView")) {
                QMetaObject::invokeMethod(view, "forceActiveFocus");
                auto sendKey = [&window](int key, const QString &text, Qt::KeyboardModifiers mods) {
                    QKeyEvent press(QEvent::KeyPress, key, mods, text);
                    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
                    QCoreApplication::sendEvent(window, &press);
                    QCoreApplication::sendEvent(window, &release);
                };
                sendKey(Qt::Key_H, "h", Qt::NoModifier);
                sendKey(Qt::Key_I, "i", Qt::NoModifier);
                sendKey(Qt::Key_Return, "\r", Qt::NoModifier);
            }
        }
        // LUCID_TAB=script shows the script workbench in the grab.
        if (qEnvironmentVariable("LUCID_TAB") == QLatin1String("script")) {
            if (auto *bar = window->findChild<QObject *>("bottomBar"))
                bar->setProperty("currentIndex", 1);
            backend.setScriptSource("// Ping the device until it answers\n"
                                    "DL.StartCommunication();\n"
                                    "DL.SendStr(\"PING\\r\\n\");\n"
                                    "if (DL.ReceiveStr(\"ACK\", 1000)) {\n"
                                    "    print(\"device answered\");\n"
                                    "} else {\n"
                                    "    print(\"no answer, retrying…\");\n"
                                    "}\n");
            backend.log()->append(CommLog::Event, -1, "Script finished");
        }
        // LUCID_OPEN=settings / sequence / help / editor opens a dialog.
        const QString openWhat = qEnvironmentVariable("LUCID_OPEN");
        if (!openWhat.isEmpty()) {
            QVariant returned;
            const bool called = QMetaObject::invokeMethod(window, "openFor", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariant, returned), Q_ARG(QVariant, openWhat));
            qInfo() << "openFor called:" << called;
            // Let the popup's enter animation finish before grabbing.
            QTimer::singleShot(450, &app, [&] {
                const QString out = qEnvironmentVariable("LUCID_GRAB", "/tmp/lucid-grab.png");
                window->grabWindow().save(out);
                qInfo() << "grabbed" << out;
                app.quit();
                QTimer::singleShot(300, [] { std::_Exit(0); });
            });
            return;
        }
        const QString out = qEnvironmentVariable("LUCID_GRAB", "/tmp/lucid-grab.png");
        window->grabWindow().save(out);
        qInfo() << "grabbed" << out;
        app.quit();
        QTimer::singleShot(300, [] { std::_Exit(0); });
    });
    return app.exec();
}
