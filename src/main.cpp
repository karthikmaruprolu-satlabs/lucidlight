// Lucidlight — serial monitor, terminal, and test scripting for the desktop,
// in the spirit of Docklight.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSaveFile>
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
                const QString &portA, const QString &portB, int mode, int baud, bool json,
                const QString &outFile) {
    QTextStream out(stdout);
    Backend backend;

    auto stamp = [] { return QDateTime::currentDateTime().toString("HH:mm:ss.zzz"); };
    auto emitTraffic = [&](const char *type, int channel, const QByteArray &data) {
        if (json) {
            QJsonObject event;
            event["type"] = type;
            event["channel"] = channel;
            event["hex"] = DataFormat::toHex(data);
            event["text"] = DataFormat::display(data, DataFormat::Format::Text);
            event["t"] = stamp();
            out << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
            out << "\n";
            out.flush();
        } else {
            out << stamp() << "  " << type << (channel + 1) << "  " << DataFormat::toHex(data)
                << "  " << DataFormat::display(data, DataFormat::Format::Text) << "\n";
        }
    };

    // Connect the reporting first, so load failures are visible.
    QObject::connect(backend.comm(), &CommManager::incoming,
                     [&](int channel, const QByteArray &data) { emitTraffic("rx", channel, data); });
    QObject::connect(backend.comm(), &CommManager::transmitted,
                     [&](int channel, const QByteArray &data) { emitTraffic("tx", channel, data); });
    QObject::connect(backend.comm(), &CommManager::eventLogged, [&](const QString &text) {
        if (!json)
            out << "[event] " << text << "\n";
    });
    QObject::connect(backend.scripts(), &ScriptRunner::outputLine,
                     [&](const QString &line, bool error) {
                         if (json) {
                             QJsonObject event;
                             event["type"] = QStringLiteral("script");
                             event["line"] = line;
                             event["error"] = error;
                             out << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))
                                 << "\n";
                             out.flush();
                         } else {
                             out << "[script] " << line << "\n";
                         }
                     });
    QObject::connect(&backend, &Backend::toast, [&](const QString &text) {
        if (!json)
            out << text << "\n";
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
    // A project with the run-on-open option and no explicit script runs the
    // project's own script, like Docklight's automation.
    if (scriptCode.isEmpty() && backend.runOnOpen()
        && !backend.scriptSource().trimmed().isEmpty())
        scriptCode = backend.scriptSource();

    QEventLoop loop;
    QTimer::singleShot(qMax(1, durationSec) * 1000, &loop, &QEventLoop::quit);
    if (!scriptCode.isEmpty()) {
        bool scriptDone = false;
        bool scriptOk = true;
        QString scriptError;
        QObject::connect(backend.scripts(), &ScriptRunner::finished, &loop,
                         [&](bool ok, const QString &error) {
                             scriptDone = true;
                             scriptOk = ok;
                             scriptError = error;
                             loop.quit();
                         });
        backend.scripts()->run(scriptCode);
        // Both timers race: whoever finishes first ends the run.
        loop.exec();
        backend.scripts()->stop();
        exitCode = scriptDone && !scriptOk ? 1 : 0;
        if (json) {
            QJsonObject result;
            result["type"] = QStringLiteral("result");
            result["ok"] = scriptOk || !scriptDone;
            result["error"] = scriptError;
            result["tx"] = double(backend.comm()->txBytes());
            result["rx"] = double(backend.comm()->rxBytes());
            result["duration"] = qMax(1, durationSec);
            out << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) << "\n";
            out.flush();
        } else if (!scriptError.isEmpty()) {
            out << "[script error] " << scriptError << "\n";
        }
    } else {
        loop.exec();
    }
    if (!outFile.isEmpty()) {
        QFile log(outFile);
        if (log.open(QIODevice::WriteOnly | QIODevice::Text))
            log.write(backend.log()->toText().toUtf8());
        else
            QTextStream(stderr) << "Cannot write " << outFile << "\n";
    }
    backend.comm()->stop();
    return exitCode;
}

// `lucidlight skill` prints the agent skill; `skill install` puts it where
// the pi agent harness reads skills from, following the same pattern Hype
// uses for its agents.
int runSkill(bool install) {
    QFile resource(":/skill.md");
    if (!resource.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "Bundled skill not found\n";
        return 1;
    }
    const QByteArray contents = resource.readAll();
    if (!install) {
        QTextStream(stdout) << QString::fromUtf8(contents);
        return 0;
    }
    const QString dir = QDir::homePath() + "/.pi/agent/skills/lucidlight";
    if (!QDir().mkpath(dir)) {
        QTextStream(stderr) << "Cannot create " << dir << "\n";
        return 1;
    }
    QSaveFile file(dir + "/SKILL.md");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(stderr) << "Cannot write " << dir << "/SKILL.md\n";
        return 1;
    }
    file.write(contents);
    if (!file.commit()) {
        QTextStream(stderr) << "Cannot write " << dir << "/SKILL.md\n";
        return 1;
    }
    QTextStream(stdout) << "Skill installed to " << dir << "/SKILL.md\n";
    return 0;
}
}  // namespace

int main(int argc, char *argv[]) {
    // `lucidlight run ...` needs no display at all, so it runs on a plain
    // QCoreApplication and works over SSH and in test harnesses. `skill`
    // serves coding agents the same way.
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("run")) {
            QCoreApplication app(argc, argv);
            QCommandLineParser parser;
            parser.addHelpOption();
            parser.addPositionalArgument("project", "Project file (.llproj), optional");
            QCommandLineOption scriptOption("script", "Script file to run", "file");
            QCommandLineOption durationOption("duration", "Seconds to run", "seconds", "10");
            QCommandLineOption portAOption("port-a", "Override port A device", "device");
            QCommandLineOption portBOption("port-b", "Override port B device", "device");
            QCommandLineOption modeOption("mode", "0 single, 1 redirect, 2 monitor", "mode");
            QCommandLineOption baudOption("baud", "Override baud rate", "rate");
            QCommandLineOption jsonOption("json", "Emit NDJSON events and a result line");
            QCommandLineOption outOption("out", "Save the communication log to this file", "file");
            parser.addOptions({scriptOption, durationOption, portAOption, portBOption, modeOption,
                               baudOption, jsonOption, outOption});
            parser.process(app);
            QStringList positional = parser.positionalArguments();
            positional.removeAll(QStringLiteral("run"));
            return runHeadless(positional.value(0), parser.value(scriptOption),
                               parser.value(durationOption).toInt(), parser.value(portAOption),
                               parser.value(portBOption), parser.isSet(modeOption) ? parser.value(modeOption).toInt() : -1,
                               parser.value(baudOption).toInt(), parser.isSet(jsonOption),
                               parser.value(outOption));
        }
        if (arg == QLatin1String("skill")) {
            // `skill install` may or may not be the next word.
            const bool install = i + 1 < argc && QString::fromLocal8Bit(argv[i + 1]) == QLatin1String("install");
            QCoreApplication app(argc, argv);
            return runSkill(install);
        }
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
    // A first run has no session; the welcome data QML added must not make
    // the window claim unsaved changes.
    if (!backend.loadSession())
        backend.setDirty(false);

    return app.exec();
}
