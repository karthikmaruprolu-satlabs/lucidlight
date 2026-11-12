// Lucidlight — serial monitor, terminal, and test scripting for the desktop,
// in the spirit of Docklight.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "jshighlighter.h"
#include "theme.h"
#include "translator.h"

namespace {
// Headless run: load a project, open the ports, optionally run a script,
// mirror the communication window to stdout, then exit.
int runHeadless(const QString &projectPath, const QString &scriptPath, int durationSec,
                const QString &portA, const QString &portB, int mode, int baud) {
    Backend backend;
    // Connect the reporting first, so load failures are visible.
    QObject::connect(backend.comm(), &CommManager::incoming,
                     [](int channel, const QByteArray &data) {
                         QTextStream out(stdout);
                         out << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << "  RX"
                             << (channel + 1) << "  " << DataFormat::toHex(data) << "  "
                             << DataFormat::display(data, DataFormat::Format::Text) << "\n";
                     });
    QObject::connect(backend.comm(), &CommManager::transmitted,
                     [](int channel, const QByteArray &data) {
                         QTextStream out(stdout);
                         out << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << "  TX"
                             << (channel + 1) << "  " << DataFormat::toHex(data) << "  "
                             << DataFormat::display(data, DataFormat::Format::Text) << "\n";
                     });
    QObject::connect(backend.comm(), &CommManager::eventLogged, [](const QString &text) {
        QTextStream(stdout) << "[event] " << text << "\n";
    });
    QObject::connect(backend.scripts(), &ScriptRunner::outputLine, [](const QString &line, bool error) {
        QTextStream(error ? stderr : stdout) << "[script] " << line << "\n";
    });
    QObject::connect(&backend, &Backend::toast, [](const QString &text) {
        QTextStream(stdout) << text << "\n";
    });

    if (!projectPath.isEmpty())
        backend.openProjectFrom(projectPath);
    if (!portA.isEmpty())
        backend.channelA()->setDevice(portA);
    if (!portB.isEmpty())
        backend.channelB()->setDevice(portB);
    if (mode >= 0)
        backend.comm()->setMode(mode);
    if (baud > 0) {
        backend.channelA()->setBaud(baud);
        backend.channelB()->setBaud(baud);
    }

    int exitCode = 0;
    if (!backend.comm()->start(backend.channelA()->config(), backend.channelB()->config()))
        return 1;

    QString scriptCode;
    if (!scriptPath.isEmpty()) {
        QFile file(scriptPath);
        if (!file.open(QIODevice::ReadOnly)) {
            QTextStream(stderr) << "Cannot read " << scriptPath << "\n";
            return 1;
        }
        scriptCode = QString::fromUtf8(file.readAll());
    }

    QEventLoop loop;
    QTimer::singleShot(qMax(1, durationSec) * 1000, &loop, &QEventLoop::quit);
    if (!scriptCode.isEmpty()) {
        bool scriptDone = false;
        bool scriptOk = true;
        QObject::connect(backend.scripts(), &ScriptRunner::finished, &loop,
                         [&](bool ok, const QString &) {
                             scriptDone = true;
                             scriptOk = ok;
                             loop.quit();
                         });
        backend.scripts()->run(scriptCode);
        // Both timers race: whoever finishes first ends the run.
        loop.exec();
        backend.scripts()->stop();
        exitCode = scriptDone && !scriptOk ? 1 : 0;
        QTextStream(stdout) << "[headless] scriptDone=" << scriptDone << " scriptOk=" << scriptOk << "\n";
    } else {
        loop.exec();
    }
    backend.comm()->stop();
    return exitCode;
}
}  // namespace

int main(int argc, char *argv[]) {
    // `lucidlight run ...` needs no display at all, so it runs on a plain
    // QCoreApplication and works over SSH and in test harnesses.
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("run")) {
            QCoreApplication app(argc, argv);
            QCommandLineParser parser;
            parser.addHelpOption();
            parser.addPositionalArgument("project", "Project file (.llproj)");
            QCommandLineOption scriptOption("script", "Script file to run", "file");
            QCommandLineOption durationOption("duration", "Seconds to run", "seconds", "10");
            QCommandLineOption portAOption("port-a", "Override port A device", "device");
            QCommandLineOption portBOption("port-b", "Override port B device", "device");
            QCommandLineOption modeOption("mode", "0 single, 1 redirect, 2 monitor", "mode");
            QCommandLineOption baudOption("baud", "Override baud rate", "rate");
            parser.addOptions({scriptOption, durationOption, portAOption, portBOption, modeOption, baudOption});
            parser.process(app);
            QStringList positional = parser.positionalArguments();
            positional.removeAll(QStringLiteral("run"));
            return runHeadless(positional.value(0), parser.value(scriptOption),
                               parser.value(durationOption).toInt(), parser.value(portAOption),
                               parser.value(portBOption), parser.isSet(modeOption) ? parser.value(modeOption).toInt() : -1,
                               parser.value(baudOption).toInt());
        }

    QGuiApplication app(argc, argv);
    app.setApplicationName("lucidlight");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("lucidlight");
    app.setDesktopFileName("lucidlight");
    app.setWindowIcon(QIcon::fromTheme("lucidlight"));

    QCommandLineParser parser;
    parser.setApplicationDescription("Serial monitor, terminal, and test scripting");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    QQmlApplicationEngine engine;
    qmlRegisterType<JsHighlighter>("Lucidlight", 1, 0, "JsHighlighter");
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&app, &QGuiApplication::aboutToQuit, &backend, &Backend::saveSession);
    engine.rootContext()->setContextProperty("translator", &translator);
    QObject::connect(&app, &QGuiApplication::aboutToQuit, &backend, &Backend::saveSession);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    backend.loadSession();

    return app.exec();
}
