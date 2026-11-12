// Offscreen harness: loads the real QML, drives it, and grabs the window to
// a PNG so agents can see what the user would see.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

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
        const QString out = qEnvironmentVariable("LUCID_GRAB", "/tmp/lucid-grab.png");
        window->grabWindow().save(out);
        qInfo() << "grabbed" << out;
        app.quit();
    });
    return app.exec();
}
