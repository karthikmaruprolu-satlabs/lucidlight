#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <QRegularExpression>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "backend.h"
#include "checksum.h"
#include "vbstrans.h"
#include "commmanager.h"
#include "dataformat.h"
#include "dlscript.h"
#include "project.h"
#include "sequencemodel.h"
#include "sequencematcher.h"
#include "seriallink.h"
#include "netlink.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include "theme.h"
#include "translator.h"

class LucidlightTests : public QObject {
    Q_OBJECT

private slots:
    // ---- data notation ---------------------------------------------------
    void textNotationParsesHexEscapes()
    {
        const auto parsed = DataFormat::parse("Hi[0D][0A]", DataFormat::Format::Text);
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.bytes(), QByteArray("Hi\r\n"));
        QCOMPARE(parsed.wildcard().size(), 4);
        QVERIFY(!parsed.wildcard().contains(true));
    }

    void textNotationLiteralBracketAndWildcards()
    {
        const auto parsed = DataFormat::parse("[[AB??", DataFormat::Format::Text);
        QVERIFY(parsed.ok());
        QByteArray expected("[AB");
        expected.append(char(0));
        expected.append(char(0));   // text mode: each '?' is one character
        QCOMPARE(parsed.bytes(), expected);
        QCOMPARE(parsed.wildcard().at(3), true);
        QCOMPARE(parsed.wildcard().at(4), true);
    }

    void hexNotationParsesPairsAndWildcards()
    {
        const auto parsed = DataFormat::parse("48 65 ?? 6C", DataFormat::Format::Hex);
        QVERIFY(parsed.ok());
        QByteArray expected("He");
        expected.append(char(0));   // '??' is one wildcard byte, like Docklight
        expected.append('l');
        QCOMPARE(parsed.bytes(), expected);
        QCOMPARE(parsed.wildcard().at(2), true);
    }

    void hexNotationRejectsOddDigits()
    {
        QVERIFY(!DataFormat::parse("486", DataFormat::Format::Hex).ok());
    }

    void decNotationParsesValues()
    {
        const auto parsed = DataFormat::parse("72 101, 108", DataFormat::Format::Dec);
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.bytes(), QByteArray("Hel"));
    }

    void decNotationRejectsOutOfRange()
    {
        QVERIFY(!DataFormat::parse("256", DataFormat::Format::Dec).ok());
    }

    void binaryViewAndControlNames()
    {
        // Binary view: eight bits per byte, both directions round-trip.
        QCOMPARE(DataFormat::display(QByteArray("AB"), DataFormat::Format::Bin),
                 QString("01000001 01000010"));
        const auto parsed = DataFormat::parse("01000001 01000010", DataFormat::Format::Bin);
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.bytes(), QByteArray("AB"));

        // Control-character mnemonics, with the CRLF pair folded.
        QCOMPARE(DataFormat::controlName(0x0D), QString("CR"));
        QCOMPARE(DataFormat::controlName(0x41), QString());
        QCOMPARE(DataFormat::displayWithControlNames(QByteArray("A\r\nB\x1B")),
                 QString("A<CR><LF>B<ESC>"));
    }

    void displayFormats()
    {
        const QByteArray data("Hi\r");
        QCOMPARE(DataFormat::display(data, DataFormat::Format::Text), QString("Hi") + QChar(0xB7));
        QCOMPARE(DataFormat::display(data, DataFormat::Format::Hex), QString("48 69 0D"));
        QCOMPARE(DataFormat::display(data, DataFormat::Format::Dec), QString("72 105 13"));
        QCOMPARE(DataFormat::toHex(QByteArray(1, '\x0F')), QString("0F"));
    }

    void textNotationRoundTrips()
    {
        const QByteArray raw = QByteArray("Text with [bracket] and ") + char(0x0D) + char(0xFF);
        const QString notation = DataFormat::toTextNotation(raw);
        const auto parsed = DataFormat::parse(notation, DataFormat::Format::Text);
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.bytes(), raw);
    }

    // ---- receive matching --------------------------------------------------
    void matcherFiresOnExactPattern()
    {
        SequenceMatcher matcher;
        QVector<SequenceMatcher::Pattern> patterns;
        SequenceMatcher::Pattern pattern;
        pattern.sequenceId = 7;
        pattern.elements = DataFormat::parse("ACK", DataFormat::Format::Text).elements;
        patterns.append(pattern);
        matcher.setPatterns(patterns);

        int firedId = 0;
        QByteArray firedBytes;
        connect(&matcher, &SequenceMatcher::matched, [&](int id, const QVector<SequenceMatcher::Token> &tokens) {
            firedId = id;
            for (const auto &t : tokens)
                if (t.kind == SequenceMatcher::Token::Data)
                    firedBytes.append(char(t.value));
        });
        matcher.feed("xxACK", 1000);
        QCOMPARE(firedId, 7);
        // Docklight reports the matched window itself, not the discarded
        // bytes before it.
        QCOMPARE(firedBytes, QByteArray("ACK"));
    }

    void matcherHonorsWildcardsAndSplits()
    {
        SequenceMatcher matcher;
        QVector<SequenceMatcher::Pattern> patterns;
        {
            SequenceMatcher::Pattern pattern;
            pattern.sequenceId = 1;
            pattern.elements =
                DataFormat::parse("01 02 ? 03", DataFormat::Format::Hex).elements;
            patterns.append(pattern);
        }
        matcher.setPatterns(patterns);

        int fires = 0;
        connect(&matcher, &SequenceMatcher::matched, [&](int, const QVector<SequenceMatcher::Token> &) { ++fires; });
        matcher.feed(QByteArray::fromHex("0102AA03"), 1000);
        QCOMPARE(fires, 1);
        matcher.feed(QByteArray::fromHex("01020103"), 1100);
        QCOMPARE(fires, 2);
    }

    void matcherIgnoresWhenNoPatterns()
    {
        SequenceMatcher matcher;
        matcher.feed("anything", 1000);
        // No crash, no signals — nothing connected anyway.
    }

    // ---- sequence model ------------------------------------------------------
    void sequenceModelAddUpdateRemove()
    {
        SequenceModel model;
        const int row = model.add({{"name", "Ping"}, {"isSend", true}, {"format", 0},
                                   {"data", "PING"}, {"hotkey", 3}});
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.sequences().at(row).name, QString("Ping"));
        QCOMPARE(model.findSendByHotkey(3), row);
        QCOMPARE(model.findSendByHotkey(4), -1);
        model.update(row, {{"name", "Pong"}});
        QCOMPARE(model.sequences().at(row).name, QString("Pong"));
        model.remove(row);
        QCOMPARE(model.count(), 0);
    }

    void projectAutoRunOption()
    {
        // runOnOpen survives save/load, and the headless runner executes the
        // project script when no explicit script is given.
        Backend backend;
        backend.setScriptSource("DL.Comment('auto-ran'); 1;");
        backend.setRunOnOpen(true);
        QTemporaryDir dir;
        const QString path = dir.path() + "/autorun.llproj";
        backend.saveProjectTo(path);
        QVERIFY(QFile::exists(path));

        Backend loaded;
        loaded.openProjectFrom(path);
        QVERIFY(loaded.runOnOpen());
        QCOMPARE(loaded.scriptSource().contains("auto-ran"), true);

        // Round 2: option off stays off.
        backend.setRunOnOpen(false);
        backend.saveProjectTo(path);
        Backend reloaded;
        reloaded.openProjectFrom(path);
        QVERIFY(!reloaded.runOnOpen());
    }

    // ---- communication manager -------------------------------------------------
    void managerRequiresAPort()
    {
        CommManager manager;
        bool eventFired = false;
        connect(&manager, &CommManager::eventLogged, [&](const QString &) { eventFired = true; });
        SerialConfig empty;
        QVERIFY(!manager.start(empty, empty));
        QVERIFY(eventFired);
        QCOMPARE(manager.running(), false);
    }

    void managerRejectsMonitorSends()
    {
        CommManager manager;
        manager.setMode(CommManager::Monitor);
        QCOMPARE(manager.send("x"), false);
    }

    // ---- serial over a pseudo terminal ----------------------------------------
    void serialLoopbackThroughPty()
    {
        // A real end-to-end check: the link on the slave side receives what
        // another process writes to the master, and what the link sends shows
        // up on the master.
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QVERIFY(grantpt(master) == 0);
        QVERIFY(unlockpt(master) == 0);
        char slaveName[128];
        QVERIFY(ptsname_r(master, slaveName, sizeof(slaveName)) == 0);

        SerialLink link;
        SerialConfig config;
        config.device = slaveName;
        QVERIFY(link.open(config));

        // Inbound: master writes, the link's reader picks it up.
        QByteArray received;
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        connect(&link, &SerialLink::received, [&](const QByteArray &data) {
            received += data;
            if (received.contains("hello"))
                loop.quit();
        });
        QCOMPARE(::write(master, "hello", 5), 5);
        loop.exec();
        QVERIFY(received.contains("hello"));

        // Outbound: the link sends, the master reads it back.
        QCOMPARE(link.write(QByteArray("bye")), 3);
        char buffer[64];
        QByteArray echoed;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500 && !echoed.contains("bye")) {
            QCoreApplication::processEvents();
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                echoed.append(buffer, int(n));
        }
        QVERIFY(echoed.contains("bye"));
        ::close(master);
        link.close();
    }

    void serialRejectsUnknownBaud()
    {
        SerialLink link;
        SerialConfig config;
        config.device = "/dev/null";
        config.baud = 1234567;
        QVERIFY(!link.open(config));
        QVERIFY(!link.lastError().isEmpty());
    }

    // ---- scripting -------------------------------------------------------------
    void dlScriptRunsAndPrints()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        QStringList lines;
        bool finishedOk = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::outputLine, [&](const QString &line, bool error) {
            if (!error)
                lines.append(line);
        });
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &) {
            finishedOk = ok;
            loop.quit();
        });
        runner.run("print(\"a\" + 1); print(\"done\");");
        loop.exec();
        QVERIFY(finishedOk);
        const QStringList expected{QStringLiteral("a1"), QStringLiteral("done")};
        QCOMPARE(lines, expected);
    }

    void dlScriptReportsErrorsAndLine()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &message) {
            error = message;
            Q_UNUSED(ok)
            loop.quit();
        });
        runner.run("var x = ;");
        loop.exec();
        QVERIFY(!error.isEmpty());
        // The error points at line 1 of the script.
        QVERIFY(error.contains("line 1") || error.contains("SyntaxError"));
    }

    void dlScriptBlockingWaitAndStop()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        bool finished = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool, const QString &) {
            finished = true;
            loop.quit();
        });
        runner.run("DL.Wait(3000);");
        // Wait a moment, then stop: the Wait must return early, not after 3 s.
        QTest::qWait(150);
        runner.stop();
        loop.exec();
        QVERIFY(finished);
    }

    void dlConversions()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        QString hexResult;
        QString strResult;
        int decResult = 0;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &) {
            QVERIFY(ok);
            loop.quit();
        });
        connect(&runner, &ScriptRunner::outputLine, [&](const QString &line, bool) {
            if (line.startsWith("hex="))
                hexResult = line.mid(4);
            else if (line.startsWith("str="))
                strResult = line.mid(4);
            else if (line.startsWith("dec="))
                decResult = line.mid(4).toInt();
        });
        runner.run("print(\"hex=\" + DL.StrToHex(\"AB\"));"
                   "print(\"str=\" + DL.HexToStr(\"41 42\"));"
                   "print(\"dec=\" + DL.HexToDec(\"FF\"));");
        loop.exec();
        QCOMPARE(hexResult, QString("41 42"));
        QCOMPARE(strResult, QString("AB"));
        QCOMPARE(decResult, 255);
    }

    void dlReceiveTimesOut()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        bool finished = false;
        bool ok = true;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &) {
            finished = true;
            ok = success;
            loop.quit();
        });
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        runner.run("if (!DL.ReceiveStr(\"NEVER\", 300)) print(\"timed out ok\");");
        loop.exec();
        QVERIFY(finished);
        QVERIFY(ok);  // the script handled the timeout itself
        QVERIFY(QDateTime::currentMSecsSinceEpoch() - started < 2500);
    }

    void dlSendRequiresCommButParsesFirst()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        QString errorText;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &error) {
            errorText = error;
            Q_UNUSED(ok)
            loop.quit();
        });
        // Send returns false when communication is off; the script continues.
        runner.run("var ok = DL.SendHex(\"48 65\"); print(ok);");
        loop.exec();
        QVERIFY(errorText.isEmpty());
    }

    void dlFailFailsTheRun()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        bool ok = true;
        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &message) {
            ok = success;
            error = message;
            loop.quit();
        });
        runner.run("print(\"before\"); DL.Fail(\"sensor not responding\"); print(\"after\");");
        loop.exec();
        QVERIFY(!ok);
        QCOMPARE(error, QString("sensor not responding"));
    }

    void dlAssertPassesAndFails()
    {
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        bool ok = false;
        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &message) {
            ok = success;
            error = message;
            loop.quit();
        });
        runner.run("DL.Assert(1 + 1 === 2, \"math is broken\");");
        loop.exec();
        QVERIFY(ok);
        QVERIFY(error.isEmpty());

        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &message) {
            ok = success;
            error = message;
            loop.quit();
        });
        runner.run("DL.Assert(false, \"expected 4 bytes\");");
        loop.exec();
        QVERIFY(!ok);
        QCOMPARE(error, QString("expected 4 bytes"));
    }

    void dlEnvReadsVariables()
    {
        qputenv("LUCID_TEST_RATE", "9600");
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);

        QString reported;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &) {
            QVERIFY(ok);
            loop.quit();
        });
        connect(&runner, &ScriptRunner::outputLine, [&](const QString &line, bool) { reported = line; });
        runner.run("print(DL.Env(\"LUCID_TEST_RATE\") + \"/\" + DL.Env(\"LUCID_TEST_MISSING\", \"fallback\"));");
        loop.exec();
        QCOMPARE(reported, QString("9600/fallback"));
    }

    void dlWaitForSequenceMatchesByName()
    {
        // A receive sequence named Ready with the pattern RDY must fire when
        // the bytes show up in the stream.
        CommManager manager;
        SequenceModel sequences;
        sequences.add({{"name", "Ready"}, {"isSend", false}, {"format", 0}, {"data", "RDY"}});
        ScriptRunner runner(&manager, &sequences, nullptr);

        bool finished = false;
        bool ok = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &) {
            finished = true;
            ok = success;
            loop.quit();
        });
        runner.run("DL.Assert(DL.WaitForSequence(\"Ready\", 1, 2000) >= 1, \"no RDY\");");
        QTest::qWait(150);  // let the worker spin up
        QVERIFY(runner.engine());
        // Same route the CommManager uses: queued into the worker thread.
        QMetaObject::invokeMethod(runner.engine(), "onIncoming", Qt::QueuedConnection,
                                  Q_ARG(int, 0), Q_ARG(QByteArray, QByteArray("xxRDYyy")));
        loop.exec();
        QVERIFY(finished);
        QVERIFY(ok);
    }

    // ---- project save / load round trip -----------------------------------------
    void projectRoundTrip()
    {
        CommManager manager;
        ChannelSettings a, b;
        a.setDevice("/dev/ttyUSB0");
        a.setBaud(9600);
        a.setParity(2);
        b.setDevice("/dev/ttyUSB1");
        SequenceModel sequences;
        sequences.add({{"name", "Boot"}, {"isSend", true}, {"format", 1}, {"data", "01 02 03"}, {"hotkey", 5}});
        CommLog log;

        const QJsonObject saved = Project::save(&manager, &a, &b, &sequences, "DL.Send(1);", &log);
        QCOMPARE(saved.value("mode").toInt(), 0);
        QCOMPARE(saved.value("channelA").toObject().value("device").toString(), QString("/dev/ttyUSB0"));

        // Load into a fresh set of objects.
        CommManager manager2;
        ChannelSettings a2, b2;
        SequenceModel sequences2;
        CommLog log2;
        QString script;
        QVERIFY(Project::load(saved, &manager2, &a2, &b2, &sequences2, &script, &log2).isEmpty());
        QCOMPARE(a2.device(), QString("/dev/ttyUSB0"));
        QCOMPARE(a2.baud(), 9600);
        QCOMPARE(a2.parity(), 2);
        QCOMPARE(b2.device(), QString("/dev/ttyUSB1"));
        QCOMPARE(sequences2.count(), 1);
        QCOMPARE(sequences2.sequences().at(0).data, QString("01 02 03"));
        QCOMPARE(sequences2.sequences().at(0).format, DataFormat::Format::Hex);
        QCOMPARE(sequences2.sequences().at(0).hotkey, 5);
        QCOMPARE(script, QString("DL.Send(1);"));
    }

    void projectRejectsForeignFiles()
    {
        CommManager manager;
        ChannelSettings a, b;
        SequenceModel sequences;
        CommLog log;
        QString script;
        QVERIFY(!Project::load(QJsonObject(), &manager, &a, &b, &sequences, &script, &log).isEmpty());
    }

    // ---- log model -----------------------------------------------------------------
    void logAppendsFormatsAndTrims()
    {
        CommLog log;
        log.append(CommLog::Rx, 0, "abc");
        log.append(CommLog::Tx, 0, QByteArray(1, 0x0D));
        log.appendComment("note");
        QCOMPARE(log.rowCount(), 3);
        QCOMPARE(log.data(log.index(0, 0), CommLog::DirectionRole).toInt(), int(CommLog::Rx));
        QCOMPARE(log.data(log.index(1, 0), CommLog::TextRole).toString(), QString(QChar(0xB7)));
        QCOMPARE(log.data(log.index(2, 0), CommLog::CommentRole).toBool(), true);
        log.clear();
        QCOMPARE(log.rowCount(), 0);
    }

    void logSearchHighlights()
    {
        CommLog log;
        log.append(CommLog::Rx, 0, "needle here");
        log.append(CommLog::Rx, 0, "nothing");
        log.setSearchText("NEEDLE");  // case-insensitive
        QCOMPARE(log.data(log.index(0, 0), CommLog::SearchHitRole).toBool(), true);
        QCOMPARE(log.data(log.index(1, 0), CommLog::SearchHitRole).toBool(), false);
    }

    void logFreezeBuffersArrivals()
    {
        CommLog log;
        log.setFrozen(true);
        log.append(CommLog::Rx, 0, "one");
        log.append(CommLog::Rx, 0, "two");
        QCOMPARE(log.rowCount(), 0);
        log.setFrozen(false);
        QCOMPARE(log.rowCount(), 2);
    }

    // ---- theme and language ----------------------------------------------------------
    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QCOMPARE(theme.bg(), QString("#1e1e2e"));
    }

    void themeReadsFullPalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\nred = \"#FF0000\"\nmode = \"dark\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.red(), QString("#FF0000"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json",
              R"JSON({"%n byte(s)": {"one": "%n byte", "other": "%n bytes"}})JSON");
        write(dir.path() + "/ru.json",
              R"JSON({"%n byte(s)": {"one": "%n байт", "few": "%n байта", "many": "%n байтов", "other": "%n байта"}})JSON");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n byte(s)", nullptr, 1), QString("%n байт"));
        QCOMPARE(translator.translate("Main", "%n byte(s)", nullptr, 3), QString("%n байта"));
        QCOMPARE(translator.translate("Main", "%n byte(s)", nullptr, 5), QString("%n байтов"));
    }

    // ---- full-stack mode tests over pseudo terminals -----------------------
    // Creates a pty pair; returns the master fd and the slave path.
    static int makePty(char *slaveName, size_t size)
    {
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0)
            return -1;
        if (grantpt(master) != 0 || unlockpt(master) != 0) {
            ::close(master);
            return -1;
        }
        if (ptsname_r(master, slaveName, size) != 0) {
            ::close(master);
            return -1;
        }
        // Non-blocking: the tests poll the master and must not park in read()
        // while no data has arrived yet.
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        return master;
    }

    void modeRedirectRoutesTxToAAndRxFromB()
    {
        char slaveA[128], slaveB[128];
        int masterA = makePty(slaveA, sizeof(slaveA));
        int masterB = makePty(slaveB, sizeof(slaveB));
        QVERIFY(masterA >= 0 && masterB >= 0);

        Backend backend;
        backend.comm()->setMode(CommManager::Redirect);
        backend.channelA()->setDevice(slaveA);
        backend.channelB()->setDevice(slaveB);
        QVERIFY(backend.comm()->start(backend.channelA()->config(), backend.channelB()->config()));

        // What Lucidlight sends must leave on port A.
        QVERIFY(backend.sendText("GO", 0));
        char buffer[64];
        QByteArray onA;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500 && !onA.contains("GO")) {
            QCoreApplication::processEvents();
            const ssize_t n = ::read(masterA, buffer, sizeof(buffer));
            if (n > 0)
                onA.append(buffer, int(n));
        }
        QVERIFY(onA.contains("GO"));

        // A device answering on port B must arrive as RX.
        QByteArray received;
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        QObject::connect(backend.comm(), &CommManager::incoming, [&](int channel, const QByteArray &data) {
            if (channel == 1)
                received.append(data);
        });
        QCOMPARE(::write(masterB, "OK", 2), 2);
        loop.exec();
        QVERIFY(received.contains("OK"));

        backend.comm()->stop();
        ::close(masterA);
        ::close(masterB);
    }

    void modeMonitorTapsBothSidesAndBlocksSends()
    {
        char slaveA[128], slaveB[128];
        int masterA = makePty(slaveA, sizeof(slaveA));
        int masterB = makePty(slaveB, sizeof(slaveB));
        QVERIFY(masterA >= 0 && masterB >= 0);

        Backend backend;
        backend.comm()->setMode(CommManager::Monitor);
        backend.channelA()->setDevice(slaveA);
        backend.channelB()->setDevice(slaveB);
        QVERIFY(backend.comm()->start(backend.channelA()->config(), backend.channelB()->config()));

        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        int fromA = 0, fromB = 0;
        QObject::connect(backend.comm(), &CommManager::incoming, [&](int channel, const QByteArray &) {
            if (channel == 0)
                fromA++;
            else
                fromB++;
        });
        QCOMPARE(::write(masterA, "one", 3), 3);
        QCOMPARE(::write(masterB, "two", 3), 3);
        loop.exec();
        QCOMPARE(fromA, 1);
        QCOMPARE(fromB, 1);
        QVERIFY(!backend.sendText("nope", 0));  // monitor is read-only

        backend.comm()->stop();
        ::close(masterA);
        ::close(masterB);
    }

    void checksumSpecParsing()
    {
        // Predefined names resolve to their documented generic definitions.
        const Checksum::Spec modbus = Checksum::parse("CRC-MODBUS");
        QVERIFY(modbus.valid);
        QCOMPARE(modbus.width, 16);
        QCOMPARE(modbus.polynomial, 0x8005u);
        QCOMPARE(modbus.init, 0xFFFFu);
        QVERIFY(modbus.reflectIn && modbus.reflectOut);

        // Same algorithm, spelled out in the generic syntax.
        const Checksum::Spec generic =
            Checksum::parse("CRC:16,8005,FFFF,0000,Yes,Yes");
        QVERIFY(generic.valid);
        QCOMPARE(generic.width, 16);
        QCOMPARE(generic.polynomial, 0x8005u);

        QVERIFY(!Checksum::parse("NOPE").valid);
        QVERIFY(!Checksum::parse("").valid);
        QVERIFY(!Checksum::parse("CRC:33,1,2,3,Yes,Yes").valid);
        QVERIFY(!Checksum::parse("CRC:16,zz,0,0,No,No").valid);
    }

    void checksumVerifyAgainstManual()
    {
        // All expected values are quoted in the Docklight manual for the
        // ASCII test string "123456789" (Checksum Specification reference).
        const QByteArray data = "123456789";
        QCOMPARE(Checksum::compute("MOD256", data).toHex().toUpper(),
                 QByteArray("DD"));
        QCOMPARE(Checksum::compute("CRC-DOW", data).toHex().toUpper(),
                 QByteArray("A1"));
        QCOMPARE(Checksum::compute("CRC-16", data).toHex().toUpper(),
                 QByteArray("BB3D"));
        QCOMPARE(Checksum::compute("CRC-MODBUS", data).toHex().toUpper(),
                 QByteArray("4B37"));   // little-endian byte order in the result
        QCOMPARE(Checksum::compute("CRC-CCITT", data).toHex().toUpper(),
                 QByteArray("29B1"));
        QCOMPARE(Checksum::compute("CRC-32", data).toHex().toUpper(),
                 QByteArray("CBF43926"));
        // CRC-8 for "123456789" is the classic check value F4.
        QCOMPARE(Checksum::compute("CRC-8", data).toHex().toUpper(),
                 QByteArray("F4"));
        // MOD65536 is a 16-bit sum: 0x1DD, big-endian bytes 01 DD.
        QCOMPARE(Checksum::compute("MOD65536", data).toHex().toUpper(),
                 QByteArray("01DD"));
        // CRC-7 and CRC-XMODEM check values for "123456789".
        QCOMPARE(Checksum::compute("CRC-7", data).toHex().toUpper(), QByteArray("00"));
        QCOMPARE(Checksum::compute("CRC-XMODEM", data).toHex().toUpper(), QByteArray("31C3"));
        // XOR of "123456789": 31^32^...^39
        QCOMPARE(Checksum::compute("XOR", data).at(0), char(0x31 ^ 0x32 ^ 0x33
                     ^ 0x34 ^ 0x35 ^ 0x36 ^ 0x37 ^ 0x38 ^ 0x39));
        // LRC: two's complement of the MOD256 sum, so total = 0.
        const QByteArray lrc = Checksum::compute("LRC", data);
        const int sum = 0x31 + 0x32 + 0x33 + 0x34 + 0x35 + 0x36 + 0x37 + 0x38
                        + 0x39 + uchar(lrc.at(0));
        QCOMPARE(sum % 256, 0);
    }

    void convertSequenceDataMatchesManualExamples()
    {
        // Every expected value is quoted in the Docklight manual page
        // "DL.ConvertSequenceData".
        DLScriptEngine host;
        DLObject dl(&host);
        QCOMPARE(dl.ConvertSequenceData("toSingle", "3F 9E 06 4B"), QStringLiteral("1.234567"));
        QCOMPARE(dl.ConvertSequenceData("fromSingle", "1.234567"), QStringLiteral("3F 9E 06 4B"));
        QCOMPARE(dl.ConvertSequenceData("toDouble", "103 154 149 160 081 161 036 075",
                                        "D", false),
                 QStringLiteral("9.87987987987E+53"));
        QCOMPARE(dl.ConvertSequenceData("fromDouble", "9.87987987987E+53", "D", false),
                 QStringLiteral("103 154 149 160 081 161 036 075"));
        QCOMPARE(dl.ConvertSequenceData("toInteger16", "80 00"), QStringLiteral("-32768"));
        QCOMPARE(dl.ConvertSequenceData("fromInteger16", "-1"), QStringLiteral("FF FF"));
        QCOMPARE(dl.ConvertSequenceData("toUnsigned16", "80 00"), QStringLiteral("32768"));
        QCOMPARE(dl.ConvertSequenceData("fromUnsigned16", "65535", "D"),
                 QStringLiteral("255 255"));
        QCOMPARE(dl.ConvertSequenceData("toInteger32", "00 00 00 80", "H", false),
                 QStringLiteral("-2147483648"));
        QCOMPARE(dl.ConvertSequenceData("fromInteger32", "-2", "H", false),
                 QStringLiteral("FE FF FF FF"));
        QCOMPARE(dl.ConvertSequenceData("toUnsigned32", "FF 00 FF 00"),
                 QStringLiteral("4278255360"));
        QCOMPARE(dl.ConvertSequenceData("fromUnsigned32", "21121977", "D"),
                 QStringLiteral("001 066 075 185"));
        QCOMPARE(dl.ConvertSequenceData("toBool", "00"), QStringLiteral("False"));
        QCOMPARE(dl.ConvertSequenceData("toBool", "01 00"), QStringLiteral("True"));
        QCOMPARE(dl.ConvertSequenceData("toText", "FF 48 65 6C 6C 6F 21 0D 00 00"),
                 QStringLiteral("Hello!"));
        QCOMPARE(dl.ConvertSequenceData("fromText", "Hello World"),
                 QStringLiteral("48 65 6C 6C 6F 20 57 6F 72 6C 64"));
        // Binary representation is honored ("B" was once mapped to decimal).
        QCOMPARE(dl.ConvertSequenceData("toInteger16", "10000000 00000000", "B"),
                 QStringLiteral("-32768"));
        // MOD65536 is a 16-bit result (was one byte due to a Qt constructor trap).
        QCOMPARE(Checksum::compute("MOD65536", QByteArray("123456789")).toHex().toUpper(),
                 QByteArray("01DD"));
    }

    void checksumLittleEndian()
    {
        // CRC-MODBUS with the L modifier stores the lower byte first.
        Checksum::Area area = Checksum::parseArea("CRC-MODBUS L # Modbus RTU");
        QVERIFY(area.ok);
        QVERIFY(area.littleEndian);
        QByteArray sequence = QByteArray::fromHex("010601020007FFFF");
        QString err;
        QVERIFY(Checksum::applyToSequence(area, sequence, &err));
        // The manual documents the resulting Modbus RTU frame: 01 06 01 02
        // 00 07 68 34.
        QCOMPARE(sequence.toHex().toUpper(), QByteArray("0106010200076834"));
        QVERIFY(Checksum::validateSequence(area, sequence, nullptr));
    }

    void checksumAsciiResult()
    {
        // MODBUS ASCII: LRC-ASCII over the readable hex payload, result
        // rendered as two ASCII hex characters placed before CR/LF.
        Checksum::Area area = Checksum::parseArea("(2,-5) LRC-ASCII A @ -4 # modbus ascii");
        QVERIFY(area.ok);
        // ":1103006B0003" + result + CR LF, where the manual shows 7E.
        QByteArray sequence = ":1103006b0003" + QByteArray("XX") + "\r\n";
        QVERIFY(Checksum::applyToSequence(area, sequence, nullptr));
        QCOMPARE(QString::fromLatin1(sequence), QString(":1103006b00037E\r\n"));
        QVERIFY(Checksum::validateSequence(area, sequence, nullptr));
    }

    void checksumGenericCrc()
    {
        // The generic CRC syntax must reproduce the predefined names.
        QByteArray data = "123456789";
        QCOMPARE(Checksum::compute("CRC:16,1021,FFFF,0000,No,No", data),
                 Checksum::compute("CRC-CCITT", data));
        QCOMPARE(Checksum::compute("CRC:8,31,00,00,Yes,Yes", data),
                 Checksum::compute("CRC-DOW", data));
        QCOMPARE(Checksum::compute("CRC:32,04C11DB7,FFFFFFFF,FFFFFFFF,Yes,Yes", data),
                 Checksum::compute("CRC-32", data));
    }

    void textNotationWildcardsOneAndZeroOrOne()
    {
        // '?' matches exactly one byte, '#' matches zero or one (Docklight
        // F7/F8 wildcards).
        const auto parsed = DataFormat::parse("A?B#C", DataFormat::Format::Text);
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.elements.size(), 5);
        QCOMPARE(parsed.elements.at(0).kind, ParsedBytes::Element::Data);
        QCOMPARE(parsed.elements.at(1).kind, ParsedBytes::Element::WildOne);
        QCOMPARE(parsed.elements.at(2).kind, ParsedBytes::Element::Data);
        QCOMPARE(parsed.elements.at(3).kind, ParsedBytes::Element::WildZeroOrOne);
        QCOMPARE(parsed.elements.at(4).kind, ParsedBytes::Element::Data);
    }

    void functionCharacterNotation()
    {
        // [&NN] delay in 10 ms units, [%] break, [!V] handshake/parity,
        // [^MM VV] bitwise comparison — accepted in every representation.
        const auto text = DataFormat::parse("A[&05]B[%]C[!03]D[^0F 05]",
                                            DataFormat::Format::Text);
        QVERIFY(text.ok());
        QCOMPARE(text.elements.at(1).kind, ParsedBytes::Element::Delay);
        QCOMPARE(text.elements.at(1).value, 5);
        QCOMPARE(text.elements.at(3).kind, ParsedBytes::Element::Break);
        QCOMPARE(text.elements.at(5).kind, ParsedBytes::Element::Handshake);
        QCOMPARE(text.elements.at(5).value, 3);
        QCOMPARE(text.elements.at(7).kind, ParsedBytes::Element::Bitwise);
        QCOMPARE(text.elements.at(7).mask, 0x0F);
        QCOMPARE(text.elements.at(7).expect, 0x05);

        const auto hex = DataFormat::parse("41 [&02] 42 [%]", DataFormat::Format::Hex);
        QVERIFY(hex.ok());
        QCOMPARE(hex.elements.at(1).kind, ParsedBytes::Element::Delay);
        QCOMPARE(hex.elements.at(1).value, 2);
        QCOMPARE(hex.elements.at(2).kind, ParsedBytes::Element::Data);
        QCOMPARE(hex.elements.at(3).kind, ParsedBytes::Element::Break);

        const auto dec = DataFormat::parse("065 [&01] [!10]", DataFormat::Format::Dec);
        QVERIFY(dec.ok());
        QCOMPARE(dec.elements.at(1).kind, ParsedBytes::Element::Delay);
        QCOMPARE(dec.elements.at(2).kind, ParsedBytes::Element::Handshake);
        QCOMPARE(dec.elements.at(2).value, 0x10);
    }

    void notationErrorsAreReported()
    {
        QVERIFY(!DataFormat::parse("[&999]", DataFormat::Format::Text).ok());
        QVERIFY(!DataFormat::parse("[^0F]", DataFormat::Format::Text).ok());
        QVERIFY(!DataFormat::parse("[!GG]", DataFormat::Format::Text).ok());
        QVERIFY(!DataFormat::parse("41 4", DataFormat::Format::Hex).ok());
        QVERIFY(!DataFormat::parse("999", DataFormat::Format::Dec).ok());
    }

    void matcherZeroOrOneWildcard()
    {
        // '#' matches zero or one byte; the longest match at the earliest
        // start wins ("match inside a match" is not returned).
        SequenceMatcher matcher;
        QVector<SequenceMatcher::Pattern> patterns;
        SequenceMatcher::Pattern pattern;
        pattern.sequenceId = 1;
        pattern.elements = DataFormat::parse("temp=?##F", DataFormat::Format::Text).elements;
        patterns.append(pattern);
        matcher.setPatterns(patterns);

        QByteArray fired;
        connect(&matcher, &SequenceMatcher::matched, [&](int, const QVector<SequenceMatcher::Token> &tokens) {
            for (const auto &t : tokens)
                if (t.kind == SequenceMatcher::Token::Data)
                    fired.append(char(t.value));
        });
        matcher.feed("temp=102F", 1000);
        // ? takes '1', the two '#' take '0' and '2', 'F' matches.
        QCOMPARE(fired, QByteArray("temp=102F"));
    }

    void matcherDelayRequiresPause()
    {
        // The '&' delay element only matches after the given silent gap
        // (value * 10 ms).
        SequenceMatcher matcher;
        QVector<SequenceMatcher::Pattern> patterns;
        SequenceMatcher::Pattern pattern;
        pattern.sequenceId = 1;
        pattern.elements = DataFormat::parse("A[&05]B", DataFormat::Format::Text).elements;
        patterns.append(pattern);
        matcher.setPatterns(patterns);

        int fires = 0;
        connect(&matcher, &SequenceMatcher::matched, [&](int, const QVector<SequenceMatcher::Token> &) { ++fires; });
        matcher.feed("AB", 1000);        // no pause between A and B
        QCOMPARE(fires, 0);
        matcher.feed("A", 2000);
        matcher.feed("B", 2100);         // 100 ms gap >= 50 ms required
        QCOMPARE(fires, 1);
    }

    void matcherChecksumValidation()
    {
        // A receive sequence with a checksum area matches only when the
        // stored checksum is right (mode OK) or wrong (mode WRONG).
        SequenceMatcher matcher;
        QVector<SequenceMatcher::Pattern> patterns;
        SequenceMatcher::Pattern ok;
        ok.sequenceId = 1;
        ok.hasChecksum = true;
        ok.checksumSpec = "MOD256";
        ok.checksumMode = 0;
        // Docklight receives checksum placeholders as part of the sequence:
        // "ABC" plus one placeholder byte, MOD256 over the first three.
        ok.elements = DataFormat::parse("41 42 43 00", DataFormat::Format::Hex).elements;
        // "ABC" has MOD256 sum 0xC6.
        patterns.append(ok);
        matcher.setPatterns(patterns);

        int fires = 0;
        connect(&matcher, &SequenceMatcher::matched, [&](int, const QVector<SequenceMatcher::Token> &) { ++fires; });
        matcher.feed(QByteArray::fromHex("414243C6"), 1000);   // checksum fits
        QCOMPARE(fires, 1);
        matcher.feed(QByteArray::fromHex("41424300"), 1100);   // wrong checksum
        QCOMPARE(fires, 1);
        patterns[0].checksumMode = 1;   // WRONG: mismatch constitutes the match
        matcher.setPatterns(patterns);
        matcher.feed(QByteArray::fromHex("41424300"), 1200);
        QCOMPARE(fires, 2);
    }

    void dlDocklightApiSurface()
    {
        // The Docklight-named methods exist and behave.
        CommManager manager;
        SequenceModel sequences;
        sequences.add({{"name", "Ping"}, {"isSend", false}, {"format", 0}, {"data", "PING"}});
        ScriptRunner runner(&manager, &sequences, nullptr);
        bool finished = false;
        bool ok = false;
        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &err) {
            finished = true;
            ok = success;
            error = err;
            loop.quit();
        });
        runner.run("1 + 1;");
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(ok, qPrintable(error));
    }

    void dlOnReceiveEventProcedure()
    {
        CommManager manager;
        SequenceModel sequences;
        sequences.add({{"name", "Temp"}, {"isSend", false}, {"format", 0}, {"data", "temp="}});
        ScriptRunner runner(&manager, &sequences, nullptr);
        bool finished = false;
        bool ok = false;
        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &err) {
            finished = true;
            ok = success;
            error = err;
            loop.quit();
        });
        // The event procedure reacts to a match; processing happens while
        // the main script parks, like Docklight's event timing.
        runner.run(
            "var seen = '';"
            "DL.Wait(300);"
            "DL.Assert(seen === 'Temp', 'OnReceive not called: ' + seen);"
            "DL.Assert(DL.OnReceive_GetData() === 'temp=84F', 'data mismatch');"
            "function DL_OnReceive() { seen = DL.OnReceive_GetName(); }");
        QTest::qWait(150);
        QVERIFY(runner.engine());
        QMetaObject::invokeMethod(runner.engine(), "onIncoming", Qt::QueuedConnection,
                                  Q_ARG(int, 0), Q_ARG(QByteArray, QByteArray("temp=84F")));
        QMetaObject::invokeMethod(runner.engine(), "onReceiveEvent", Qt::QueuedConnection,
                                  Q_ARG(int, 1), Q_ARG(int, 0), Q_ARG(QString, "Temp"),
                                  Q_ARG(QByteArray, QByteArray("temp=84F")), Q_ARG(int, 1),
                                  Q_ARG(qint64, 1000));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(ok, qPrintable(error));
    }

    void dlFileOutput()
    {
        QTemporaryDir dir;
        CommManager manager;
        ScriptRunner runner(&manager, new SequenceModel(), nullptr);
        bool finished = false;
        bool ok = false;
        QString error;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &err) {
            finished = true;
            ok = success;
            error = err;
            loop.quit();
        });
        runner.run(QString(
            "var f = FileOutput('%1', false);"
            "DL.Assert(f.isOpen(), 'file open');"
            "f.writeLine('hello'); f.writeHex('41 42'); f.close();").arg(dir.path() + "/out.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(ok, qPrintable(error));
        QFile file(dir.path() + "/out.txt");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QCOMPARE(content, QByteArray("hello\nAB"));
    }

    void tcpClientEcho()
    {
        // A TCP channel behaves like a serial one: what the link sends
        // reaches the peer and answers come back.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();

        NetLink link;
        SerialConfig config;
        config.device = QStringLiteral("TCP:127.0.0.1:%1").arg(port);
        QVERIFY(link.open(config));
        QVERIFY(link.isOpen());

        QByteArray received;
        QEventLoop loop;
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *peer = server.nextPendingConnection();
            connect(peer, &QAbstractSocket::readyRead, peer, [peer, &received, &loop] {
                received += peer->readAll();
                if (received.contains("ping")) {
                    peer->write("pong");
                    return;
                }
                if (received.contains("pong"))
                    loop.quit();
            });
        });
        QCOMPARE(link.write("ping"), qint64(4));
        loop.exec();
        QVERIFY(received.contains("ping"));
        QCOMPARE(link.write("again"), qint64(5));
    }

    void tcpServerAcceptsAndEchoes()
    {
        // SERVER:port listens; a client connects and the channel passes data.
        NetLink listener;
        SerialConfig listenConfig;
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost);   // pick a free port
        const quint16 port = probe.serverPort();
        probe.close();
        listenConfig.device = QStringLiteral("SERVER:%1").arg(port);
        QVERIFY(listener.open(listenConfig));
        QVERIFY(listener.isWaiting());   // status 2, like Docklight

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(client.waitForConnected(2000));
        // The link reports open only once the partner is there.
        QTRY_VERIFY_WITH_TIMEOUT(listener.isOpen(), 2000);

        QByteArray got;
        QEventLoop loop;
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        connect(&client, &QAbstractSocket::readyRead, [&] {
            got += client.readAll();
            if (got.contains("hi"))
                loop.quit();
        });
        QCOMPARE(listener.write("hi"), qint64(2));
        loop.exec();
        QVERIFY(got.contains("hi"));
    }

    void udpPeerRoundTrip()
    {
        QUdpSocket peer;
        QVERIFY(peer.bind(QHostAddress::LocalHost));
        const quint16 port = peer.localPort();

        NetLink link;
        SerialConfig config;
        config.device = QStringLiteral("UDP:127.0.0.1:%1").arg(port);
        QVERIFY(link.open(config));

        QEventLoop loop;
        QByteArray fromLink;
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        QObject::connect(&peer, &QUdpSocket::readyRead, [&] {
            while (peer.hasPendingDatagrams()) {
                QByteArray d;
                d.resize(int(peer.pendingDatagramSize()));
                peer.readDatagram(d.data(), d.size());
                fromLink += d;
            }
            if (fromLink.contains("datagram"))
                loop.quit();
        });
        QCOMPARE(link.write("datagram"), qint64(8));
        loop.exec();
        QVERIFY(fromLink.contains("datagram"));
    }

    void udpSplitPorts()
    {
        // UDP:host:port:localPort sends to the remote endpoint while
        // receiving on the separate local port.
        QUdpSocket remote;
        QVERIFY(remote.bind(QHostAddress::LocalHost));
        const quint16 remotePort = remote.localPort();

        NetLink link;
        SerialConfig config;
        // Pick a free local port for the split receive side.
        QUdpSocket probe;
        QVERIFY(probe.bind(QHostAddress::LocalHost));
        const quint16 localPort = probe.localPort();
        probe.close();
        config.device = QStringLiteral("UDP:127.0.0.1:%1:%2").arg(remotePort).arg(localPort);
        QVERIFY(link.open(config));
        QCOMPARE(link.write("split"), qint64(5));

        QEventLoop loop;
        QByteArray got;
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        QObject::connect(&remote, &QUdpSocket::readyRead, [&] {
            while (remote.hasPendingDatagrams()) {
                QByteArray d;
                d.resize(int(remote.pendingDatagramSize()));
                remote.readDatagram(d.data(), d.size());
                got += d;
            }
            if (got.contains("split"))
                loop.quit();
        });
        loop.exec();
        QVERIFY(got.contains("split"));

        QVERIFY(link.write("still open") > 0);
    }

    void commManagerRoutesTcpChannel()
    {
        // The full CommManager path with a TCP channel A and an echo server.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();
        QByteArray serverGot;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *peer = server.nextPendingConnection();
            connect(peer, &QAbstractSocket::readyRead, peer, [peer, &serverGot] {
                serverGot += peer->readAll();
            });
        });

        CommManager manager;
        SerialConfig a, b;
        a.device = QStringLiteral("TCP:127.0.0.1:%1").arg(port);
        QVERIFY(manager.start(a, b));
        QVERIFY(manager.send("over tcp"));
        QTRY_COMPARE_WITH_TIMEOUT(serverGot, QByteArray("over tcp"), 2000);
        manager.stop();
    }

    void sideChannelSendsAndReceives()
    {
        // A side channel is a real connection: a TCP side channel delivers
        // to a peer and DirectSend routes 3..10 to it.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();
        QByteArray got;
        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *peer = server.nextPendingConnection();
            connect(peer, &QAbstractSocket::readyRead, peer, [peer, &got, &loop] {
                got += peer->readAll();
                if (got.contains("side"))
                    loop.quit();
            });
        });

        CommManager manager;
        SerialConfig a, b;
        a.device = "/dev/null";   // unused here
        QVERIFY(manager.start(a, b));
        QVERIFY(manager.openSideChannel(3, QStringLiteral("TCP:127.0.0.1:%1").arg(port)));
        QVERIFY(manager.sideChannelOpen(3));
        QVERIFY(manager.sideSend(3, "side"));
        loop.exec();
        QVERIFY(got.contains("side"));

        manager.closeSideChannel(3);
        QVERIFY(!manager.sideChannelOpen(3));
        manager.stop();
    }

    void scriptOpensSideChannelAndSends()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();
        QByteArray got;
        QEventLoop loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *peer = server.nextPendingConnection();
            connect(peer, &QAbstractSocket::readyRead, peer, [peer, &got, &loop] {
                got += peer->readAll();
                if (got.contains("scripted"))
                    loop.quit();
            });
        });

        CommManager manager;
        SequenceModel sequences;
        Backend backend;
        Q_UNUSED(backend)
        ScriptRunner runner(&manager, &sequences, nullptr);
        bool finished = false;
        bool ok = false;
        QString error;
        connect(&runner, &ScriptRunner::finished, [&](bool success, const QString &err) {
            finished = true;
            ok = success;
            error = err;
            loop.quit();
        });
        SerialConfig a, b;
        a.device = "/dev/null";
        manager.start(a, b);
        runner.run(QStringLiteral(
            "DL.Assert(DL.OpenSideChannel(4, 'TCP:127.0.0.1:%1'), 'side open');"
            "DL.Assert(DL.DirectSend(4, 'scripted', 'A'), 'side send');").arg(port));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(ok, qPrintable(error));
        QVERIFY(got.contains("scripted"));
    }

    void fullDocklightScriptingScenario()
    {
        // One scripted session against a live TCP device exercising the
        // whole Docklight surface: sequences with parameters, OnSend data
        // manipulation, receive triggers with comment macros, WaitForSequence
        // counters, a side channel, file logging and file output.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();
        QByteArray deviceGot;
        bool sawPing = false;
        bool sawCmd42 = false;
        QEventLoop deviceLoop;
        QTimer::singleShot(15000, &deviceLoop, &QEventLoop::quit);
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *peer = server.nextPendingConnection();
            connect(peer, &QAbstractSocket::readyRead, peer, [peer, &deviceGot, &sawPing, &sawCmd42] {
                deviceGot += peer->readAll();
                if (deviceGot.endsWith("\r\n")) {
                    const QByteArray line = deviceGot.trimmed();
                    deviceGot.clear();
                    sawPing = sawPing || line.contains("PING");
                    sawCmd42 = sawCmd42 || line.contains("CMD 42");
                    if (line == "PING")
                        peer->write("ACK\r\n");
                    else if (line.startsWith("CMD "))
                        peer->write(QByteArray("OK ") + line.mid(4) + "\r\n");
                    else if (line == "EXIT")
                        peer->disconnectFromHost();
                }
            });
        });

        // Everything runs through one Backend, exactly like the GUI app:
        // its own comm, its own sequence table, its own script runner.
        Backend backend;
        SequenceModel *sequences = backend.sequences();
        sequences->add({{"name", "Ping"}, {"isSend", true}, {"format", 0}, {"data", "PING\r\n"}});
        sequences->add({{"name", "Command"}, {"isSend", true}, {"format", 0},
                        {"data", "CMD ??\r\n"}, {"hotkey", 5}});
        sequences->add({{"name", "GotAck"}, {"isSend", false}, {"format", 0},
                        {"data", "ACK"}, {"comment", "Got ACK: %_S"}});
        sequences->add({{"name", "GotOk"}, {"isSend", false}, {"format", 0},
                        {"data", "OK #"}, {"trigger", true}, {"replyToId", 1}});
        sequences->add({{"name", "Reply"}, {"isSend", true}, {"format", 0}, {"data", "BYE\r\n"}});
        ScriptRunner *runner = backend.scripts();
        bool finished = false;
        bool ok = false;
        QString error;
        QEventLoop loop;
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        connect(runner, &ScriptRunner::finished, [&](bool success, const QString &err) {
            finished = true;
            ok = success;
            error = err;
            loop.quit();
        });

        QTemporaryDir dir;
        const QString logBase = dir.path() + "/session";
        backend.channelA()->setDevice(QStringLiteral("TCP:127.0.0.1:%1").arg(port));
        QVERIFY(backend.comm()->start(backend.channelA()->config(), backend.channelB()->config()));
        runner->run(QStringLiteral(
            "DL.Comment('status=' + DL.GetChannelStatus(1));"
            "DL.Assert(DL.GetChannelStatus(1) === 1, 'channel open got ' + DL.GetChannelStatus(1));"
            "DL.Assert(DL.StartLogging('%1', false, 'AH', 0, false), 'logging started');"
            "DL.ResetReceiveCounter();"
            "DL.SendSequence('Ping');"
            "DL.Assert(DL.WaitForSequence('GotAck', 1, 3000) >= 1, 'no ACK');"
            "DL.Assert(DL.GetReceiveCounter('GotAck') === 1, 'counter one');"
            "DL.SendSequence('Command', '42');"
            "DL.Assert(DL.WaitForSequence('GotOk', 1, 3000) >= 1, 'no OK echo');"
            "DL.Assert(DL.CalcChecksum('MOD256', 'PING', 'A') === '2E', 'checksum');"
            "var f = FileOutput('%2', false);"
            "f.writeLine('session done'); f.close();"
            "DL.StopLogging();"
            "DL.DirectSend(3, 'side says hi');"
            "DL.Comment('scenario finished');").arg(logBase, dir.path() + "/out.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 20000);
        QVERIFY2(ok, qPrintable(error));
        backend.comm()->stop();

        // The device saw the real bytes of both sequences (deviceGot holds
        // the tail after the last complete line; track totals separately).
        QVERIFY(sawPing);
        QVERIFY(sawCmd42);

        // The _asc log file holds the session.
        QFile asc(logBase + "_asc.txt");
        QVERIFY(asc.exists());
        QVERIFY(asc.open(QIODevice::ReadOnly));
        const QString ascText = QString::fromUtf8(asc.readAll());
        QVERIFY(ascText.contains("PING"));
        QVERIFY(ascText.contains("CMD 42"));

        QFile out(dir.path() + "/out.txt");
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(out.readAll()).trimmed(), QString("session done"));
    }

    void portSettingsReachTheLine()
    {
        // The pty carries the termios state both ends share, so the master
        // side reveals what applyConfig actually programmed.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        SerialLink link;
        SerialConfig config;
        config.device = slave;
        config.baud = 9600;
        config.dataBits = 7;
        config.parity = 2;   // odd
        config.stopBits = 2;
        config.flow = 2;     // Xon/Xoff
        QVERIFY(link.open(config));

        termios tio {};
        QVERIFY(tcgetattr(master, &tio) == 0);
        QCOMPARE(int(cfgetospeed(&tio)), int(B9600));
        // Linux ptys normalize the character size to 8 bits and drop parity
        // (they have no UART), so only what a pty genuinely reflects is
        // asserted here; the CSIZE/parity programming is exercised by real
        // hardware.
        QVERIFY(tio.c_cflag & CSTOPB);
        QVERIFY(tio.c_iflag & IXON);
        QVERIFY(tio.c_iflag & IXOFF);
        QVERIFY(!(tio.c_cflag & CRTSCTS));
        link.close();
        ::close(master);
    }

    void receiveTriggerAutoReplies()
    {
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        // Receive "PING" -> reply with "PONG".
        const int replyRow = backend.sequences()->add(
            QVariantMap{{"name", "Reply"}, {"isSend", true}, {"format", 0}, {"data", "PONG"}});
        const int replyId = backend.sequences()->get(replyRow)["seqId"].toInt();
        backend.sequences()->add(QVariantMap{{"name", "Watch"}, {"isSend", false}, {"format", 0},
                                             {"data", "PING"}, {"replyToId", replyId}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        // The device sends PING; the trigger must answer PONG on the port.
        QCOMPARE(::write(master, "PING", 4), 4);
        QByteArray replied;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1500 && !replied.contains("PONG")) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                replied.append(buffer, int(n));
        }
        QVERIFY(replied.contains("PONG"));

        backend.comm()->stop();
        ::close(master);
    }

    void parityDecoderSubstitutesErrorChar()
    {
        // PARMRK escaping, unit level: "\377 \0 X" marks a byte that
        // arrived with a parity error and is replaced by the configured
        // ParityErrorChar; "\377 \377" is a real 0xFF data byte.
        // Built with explicit lengths: QByteArray(const char *) would stop
        // at the embedded 0x00 byte.
        const char errorTriplet[] = {'\xFF', '\x00', '\x41'};
        const char escapedFF[] = {'\xFF', '\xFF'};
        const QByteArray raw = QByteArray(errorTriplet, 3)
                                   + QByteArray(escapedFF, 2) + "B";
        QCOMPARE(SerialLink::decodeParity(raw, int('?')),
                 QByteArray("?") + char(0xFF) + "B");
        QCOMPARE(SerialLink::decodeParity(raw, int('#')),
                 QByteArray("#") + char(0xFF) + "B");
        // ParityErrorChar -1 ("ignore" in Docklight) keeps the raw error
        // triplet, so the original character stays visible, while the
        // 0xFF escape is still un-doubled.
        QCOMPARE(SerialLink::decodeParity(raw, -1),
                 QByteArray("\xFF\x00\x41", 3) + char(0xFF) + "B");
        // Plain data passes through untouched.
        QCOMPARE(SerialLink::decodeParity("PING", int('?')), QByteArray("PING"));

        // The FF 00 00 triplet marks a BREAK condition: it fires the break
        // event instead of delivering a data byte.
        bool sawBreak = false;
        const char breakTriplet[] = {'\xFF', '\x00', '\x00'};
        QCOMPARE(SerialLink::decodeParity(QByteArray(breakTriplet, 3), int('?'),
                                          nullptr, &sawBreak),
                 QByteArray());
        QVERIFY(sawBreak);
    }

    void parityErrorCharPassesDataThrough()
    {
        // With PARMRK active the kernel doubles real 0xFF bytes and our
        // decoder un-doubles them, so ordinary data (including 0xFF and
        // 0x00 bytes) must come out exactly as it went in. A pty cannot
        // inject real parity errors, so the '?' substitution itself is
        // covered by parityDecoderSubstitutesErrorChar above.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        backend.channelA()->setParity(1);   // even; enables PARMRK decoding
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        const char rawData[] = {'\xFF', '\x00', '\x41', '\xFF', '\xFF', '\x42'};
        const QByteArray raw(rawData, 6);
        QCOMPARE(::write(master, raw.constData(), raw.size()), qsizetype(raw.size()));
        QTest::qWait(300);

        QString hex;
        for (int i = 0; i < backend.log()->count(); ++i)
            hex += backend.log()->hexAt(i) + ' ';
        QVERIFY2(hex.contains("FF 00 41 FF FF 42"), qPrintable(hex));

        backend.comm()->stop();
        ::close(master);
    }

    void clockStampsAtFixedInterval()
    {
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        backend.setClockStampInterval(1);   // every second
        QCOMPARE(backend.clockStampInterval(), 1);
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        // Two stamps need ~2 s; give it 3 s.
        QTest::qWait(3000);
        int stamps = 0;
        for (int i = 0; i < backend.log()->count(); ++i) {
            const QString text = backend.log()->textAt(i);
            if (QRegularExpression("^\\d{4}-\\d{2}-\\d{2} \\d{2}:").match(text).hasMatch())
                ++stamps;
        }
        QVERIFY2(stamps >= 2, qPrintable(QString("stamps=%1").arg(stamps)));

        // Turning the interval off stops the stamps; the timer must not
        // survive communication stop either.
        backend.setClockStampInterval(0);
        backend.comm()->stop();
        ::close(master);
    }

    void findNavigatesThroughLog()
    {
        Backend backend;
        backend.log()->append(CommLog::Rx, 0, "alpha beta", false);
        backend.log()->append(CommLog::Rx, 0, "gamma delta", false);
        backend.log()->append(CommLog::Tx, 0, "alpha again", false);
        backend.log()->append(CommLog::Rx, 0, "beta one", false);

        QCOMPARE(backend.log()->findNext("alpha", -1), 0);
        QCOMPARE(backend.log()->findNext("alpha", 0), 2);
        QCOMPARE(backend.log()->findNext("alpha", 2), 0);   // wraps around
        QCOMPARE(backend.log()->findNext("BETA", 0), 3);    // case-insensitive, hex too
        QCOMPARE(backend.log()->findNext("nonexistent", -1), -1);
        QCOMPARE(backend.log()->findNext("", -1), -1);
    }

    void printLogWritesPdf()
    {
        Backend backend;
        backend.log()->append(CommLog::Rx, 0, "PING", false);
        backend.log()->append(CommLog::Tx, 0, "PONG", false);
        const QString path = "/tmp/lucidlight-test-print.pdf";
        QFile::remove(path);
        backend.printLogTo(path);
        QFile pdf(path);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const QByteArray head = pdf.read(5);
        pdf.close();
        QCOMPARE(head, QByteArray("%PDF-"));
        QVERIFY(pdf.size() > 500);
        QFile::remove(path);
    }

    void unixSocketClientServerRoundTrip()
    {
        // UNIX:/path client + UNIXSERVER:/path listener behave like the TCP
        // pair: the listener waits, accepts one connection, both exchange
        // data, and the link status reflects connect/disconnect.
        const QString path = QStringLiteral("/tmp/lucidlight-test-%1.sock").arg(QCoreApplication::applicationPid());
        QFile::remove(path);

        Backend backend;
        backend.channelA()->setDevice("UNIXSERVER:" + path);
        backend.channelB()->setDevice("UNIX:" + path);
        backend.comm()->setMode(CommManager::Monitor);
        QVERIFY(backend.comm()->start(backend.channelA()->config(), backend.channelB()->config()));
        QVERIFY(backend.comm()->running());

        // Both links open; the client connects immediately (the listener
        // socket file exists as soon as listen() succeeds).
        QTRY_VERIFY_WITH_TIMEOUT(backend.comm()->link(0)->isOpen(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.comm()->link(1)->isOpen(), 2000);

        QVERIFY2(backend.comm()->sendOn(0, QByteArray("hello")),
                 qPrintable(backend.comm()->link(0)->lastError() + '/' + backend.comm()->link(1)->lastError()));
        QTRY_VERIFY_WITH_TIMEOUT(backend.log()->count() >= 3, 2000);
        QByteArray rx;
        for (int i = 0; i < backend.log()->count(); ++i)
            rx.append((backend.log()->hexAt(i) + ' ').toLatin1());
        QVERIFY2(rx.contains("68 65 6C 6C 6F"), qPrintable(rx));   // "hello"

        backend.comm()->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(path), 1000);
    }

    void channelStatusFollowsDocklightCodes()
    {
        // DL.GetChannelStatus semantics from the manual: 0 closed, 1 open,
        // 2 waiting (server/client), 3 channel error.
        Backend backend;
        int masterA = -1;
        char slaveA[108];
        masterA = makePty(slaveA, sizeof(slaveA));
        QVERIFY(masterA >= 0);
        backend.channelA()->setDevice(slaveA);
        backend.comm()->setMode(CommManager::Single);
        // Stopped communication reports 0.
        QCOMPARE(backend.scriptChannelStatus(1), 0);
        QVERIFY(backend.comm()->start(backend.channelA()->config(),
                                      backend.channelB()->config()));
        QCOMPARE(backend.scriptChannelStatus(1), 1);

        // A non-existing COM port while running puts the channel in error (3).
        QVERIFY(!backend.scriptSetChannelSettings("COM17:9600,NONE,8,1", 1));
        QCOMPARE(backend.scriptChannelStatus(1), 3);

        // A listening TCP server reports "waiting for connection" (2).
        const quint16 port = quint16(50000 + (QCoreApplication::applicationPid() % 5000));
        QVERIFY(backend.scriptSetChannelSettings(
            QStringLiteral("SERVER:%1").arg(port), 1));
        QCOMPARE(backend.scriptChannelStatus(1), 2);

        // Network device strings survive SetChannelSettings unsplit.
        QCOMPARE(backend.channelA()->config().device,
                 QStringLiteral("SERVER:%1").arg(port));

        backend.comm()->stop();
        QCOMPARE(backend.scriptChannelStatus(1), 0);
        if (masterA >= 0)
            ::close(masterA);
    }

    void parityDecoderTailAcrossReads()
    {
        // PARMRK escapes can split across read() calls; the decoder must
        // carry the incomplete tail so a triplet is never misread as data.
        // All byte arrays are built explicitly: QByteArray(const char *)
        // stops at an embedded 0x00.
        auto bytes = [](std::initializer_list<char> list) {
            QByteArray out;
            for (char c : list)
                out.append(c);
            return out;
        };
        QByteArray tail;

        // "A" + a lone trailing 0xFF: held back (triplet start or doubled pair).
        QCOMPARE(SerialLink::decodeParity(bytes({'A', '\xFF'}), int('?'), &tail),
                 QByteArray("A"));
        QCOMPARE(tail, bytes({'\xFF'}));

        // The tail completes an error triplet with the next read (the real
        // read path prepends the carried tail, like readReady does).
        QCOMPARE(SerialLink::decodeParity(tail + bytes({'\x00', 'B'}), int('?'), &tail),
                 QByteArray("?"));
        QVERIFY(tail.isEmpty());

        // A complete doubled pair at the end must NOT be held back.
        QCOMPARE(SerialLink::decodeParity(bytes({'\xFF', '\xFF'}), int('?'), &tail),
                 bytes({'\xFF'}));
        QVERIFY(tail.isEmpty());

        // "FF 00" at the end is a triplet missing its byte: hold both.
        QCOMPARE(SerialLink::decodeParity(bytes({'A', '\xFF', '\x00'}), int('?'), &tail),
                 QByteArray("A"));
        QCOMPARE(tail, bytes({'\xFF', '\x00'}));
        QCOMPARE(SerialLink::decodeParity(tail + bytes({'C'}), int('?'), &tail),
                 QByteArray("?"));
        QVERIFY(tail.isEmpty());

        // But "FF FF 00" is complete data (escaped 0xFF, then 0x00): the FF
        // before the 00 was consumed as the second half of the pair.
        QCOMPARE(SerialLink::decodeParity(bytes({'\xFF', '\xFF', '\x00'}), int('?'), &tail),
                 bytes({'\xFF', '\x00'}));
        QVERIFY(tail.isEmpty());

        // An odd run of three 0xFF holds exactly one.
        QCOMPARE(SerialLink::decodeParity(bytes({'\xFF', '\xFF', '\xFF'}), int('?'), &tail),
                 bytes({'\xFF'}));
        QCOMPARE(tail, bytes({'\xFF'}));
        // The lone FF plus the next byte 'D': a plain 0xFF data byte and 'D'.
        QCOMPARE(SerialLink::decodeParity(tail + bytes({'D'}), int('?'), &tail),
                 bytes({'\xFF', 'D'}));
        QVERIFY(tail.isEmpty());

        // Plain data passes through with no tail.
        QCOMPARE(SerialLink::decodeParity(QByteArray("PING"), int('?'), &tail),
                 QByteArray("PING"));
        QVERIFY(tail.isEmpty());
    }

    void matcherPerChannelNoCrossMatches()
    {
        // Each port's stream is matched separately: "PI" arriving on
        // channel A and "NG" on channel B must never assemble "PING".
        char slaveA[128], slaveB[128];
        int masterA = makePty(slaveA, sizeof(slaveA));
        int masterB = makePty(slaveB, sizeof(slaveB));
        QVERIFY(masterA >= 0 && masterB >= 0);

        Backend backend;
        backend.channelA()->setDevice(slaveA);
        backend.channelB()->setDevice(slaveB);
        backend.comm()->setMode(CommManager::Monitor);
        backend.sequences()->add(QVariantMap{{"name", "Watch"}, {"isSend", false},
                                             {"format", 0}, {"data", "PING"},
                                             {"trigger", true}, {"comment", "matched!"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        QCOMPARE(::write(masterA, "PI", 2), 2);
        QTest::qWait(50);
        QCOMPARE(::write(masterB, "NG", 2), 2);
        QTest::qWait(400);

        int comments = 0;
        for (int i = 0; i < backend.log()->count(); ++i)
            if (backend.log()->textAt(i).contains("matched!"))
                ++comments;
        QCOMPARE(comments, 0);

        backend.comm()->stop();
        ::close(masterA);
        ::close(masterB);
    }

    void commentMacrosReportChannelTwo()
    {
        char slave[128], dummy[128];
        int master = makePty(slave, sizeof(slave));
        int masterDummy = makePty(dummy, sizeof(dummy));
        QVERIFY(master >= 0 && masterDummy >= 0);

        Backend backend;
        backend.channelA()->setDevice(dummy);   // port A must be set to start
        backend.channelB()->setDevice(slave);
        backend.comm()->setMode(CommManager::Monitor);
        backend.sequences()->add(QVariantMap{{"name", "Watch"}, {"isSend", false},
                                             {"format", 0}, {"data", "PING"},
                                             {"trigger", true}, {"comment", "ch %_C dir %_X"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        QCOMPARE(::write(master, "PING", 4), 4);
        QElapsedTimer timer;
        timer.start();
        QString logged;
        while (timer.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            for (int i = 0; i < backend.log()->count(); ++i) {
                const QString text = backend.log()->textAt(i);
                if (text.startsWith("ch "))
                    logged = text;
            }
            if (!logged.isEmpty())
                break;
        }
        QCOMPARE(logged, QString("ch 2 dir RX"));

        backend.comm()->stop();
        ::close(master);
        ::close(masterDummy);
    }

    void monitorModeForwardsData()
    {
        // Docklight's Data Forwarding: in Monitor mode, data received on one
        // port is re-transmitted on the opposite port's TX line (this is how
        // a monitoring cable routes traffic through the app). The expert
        // option can turn it off.
        char slaveA[128], slaveB[128];
        int masterA = makePty(slaveA, sizeof(slaveA));
        int masterB = makePty(slaveB, sizeof(slaveB));
        QVERIFY(masterA >= 0 && masterB >= 0);

        Backend backend;
        backend.channelA()->setDevice(slaveA);
        backend.channelB()->setDevice(slaveB);
        backend.comm()->setMode(CommManager::Monitor);
        QVERIFY(backend.comm()->start(backend.channelA()->config(),
                                      backend.channelB()->config()));
        QCOMPARE(::write(masterA, "FWD1", 4), 4);
        QTest::qWait(300);
        QByteArray got;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1000 && !got.contains("FWD1")) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const ssize_t n = ::read(masterB, buffer, sizeof(buffer));
            if (n > 0)
                got.append(buffer, int(n));
        }
        QVERIFY(got.contains("FWD1"));

        // Forwarding off: data stays in the app.
        backend.comm()->setDataForwarding(false);
        QCOMPARE(::write(masterA, "FWD2", 4), 4);
        QTest::qWait(300);
        QByteArray got2;
        timer.restart();
        while (timer.elapsed() < 400) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const ssize_t n = ::read(masterB, buffer, sizeof(buffer));
            if (n > 0)
                got2.append(buffer, int(n));
        }
        QVERIFY(!got2.contains("FWD2"));

        backend.comm()->stop();
        ::close(masterA);
        ::close(masterB);
    }

    void commentMacrosExpandOnMatch()
    {
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        // The comment carries the %_S macro (matched data in the row's
        // notation) and %_L (byte count).
        backend.sequences()->add(QVariantMap{{"name", "Watch"}, {"isSend", false}, {"format", 0},
                                             {"data", "PING"}, {"trigger", true},
                                             {"comment", "got %_S len %_L"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        QCOMPARE(::write(master, "PING", 4), 4);
        QElapsedTimer timer;
        timer.start();
        QString logged;
        while (timer.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            logged.clear();
            for (int i = 0; i < backend.log()->count(); ++i) {
                const QString text = backend.log()->textAt(i);
                if (text.contains("got "))
                    logged = text;
            }
            if (!logged.isEmpty())
                break;
        }
        QVERIFY2(!logged.isEmpty(), "macro comment never logged");
        QVERIFY(logged.contains("got PING len 4"));

        backend.comm()->stop();
        ::close(master);
    }

    void checksumAreaCalculatedOnSend()
    {
        // A send sequence with a checksum area appends the computed checksum
        // bytes at the '@' position on every send.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        // "AB" + 1 checksum byte, MOD256 over bytes 1..2 (0x41+0x42 = 0x83).
        backend.sequences()->add(QVariantMap{
            {"name", "Framed"}, {"isSend", true}, {"format", 1},
            {"data", "41 42 00"}, {"checksumSpec", "(1,2) MOD256"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());
        backend.sendSequence(backend.sequences()->count() - 1);

        QByteArray got;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1500 && !got.contains('\x83')) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                got.append(buffer, int(n));
        }
        QVERIFY(got.contains('\x83'));

        backend.comm()->stop();
        ::close(master);
    }

    void sequenceRepeatSendsPeriodically()
    {
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        const int row = backend.sequences()->add(
            QVariantMap{{"name", "Tick"}, {"isSend", true}, {"format", 0},
                        {"data", "T"}, {"repeatIntervalMs", 100}});
        backend.toggleComm();   // starts communication and the repeat clock
        QVERIFY(backend.comm()->running());

        backend.sendSequence(row);   // first send + arms the repeat
        QByteArray seen;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                seen.append(buffer, int(n));
        }
        // First send plus repeats at 100 ms: comfortably more than three.
        QVERIFY(seen.count('T') >= 4);

        backend.sendSequence(row);   // disarms the repeat
        backend.comm()->stop();
        ::close(master);
    }



    void externalEditorRoundTrip()
    {
        // Docklight "Customize / External Editor": the script goes to a temp
        // file, the external command may modify it, and the change is
        // reloaded into the built-in editor.
        Backend backend;
        backend.setScriptSource("print(1)\n");
        const QString tmp = QDir::tempPath() + "/lucidlight-script.js";
        QFile::remove(tmp);
        // A fake "editor" that appends a comment line to the temp file.
        backend.setExternalEditor(
            "sh -c \"sleep 0.3; echo '// edited by external editor' >> %FILE%\"");
        QVERIFY(backend.editScriptExternally());
        QTRY_VERIFY_WITH_TIMEOUT(
            backend.scriptSource().contains("edited by external editor"),
            5000);
        QVERIFY(backend.scriptSource().contains("print(1)"));
        QFile::remove(tmp);
        // Empty command disables the feature.
        backend.setExternalEditor("");
        QVERIFY(!backend.editScriptExternally());
    }

    void snapshotCatchesTriggerSequence()
    {
        // Docklight Snapshot: the window stays hidden while armed; when a
        // receive sequence with a trigger fires, the buffered data is shown
        // and communication stops automatically.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        backend.sequences()->add(QVariantMap{{"name", "Greet"}, {"isSend", true},
                                             {"format", 0}, {"data", "Hello"}});
        // Receive sequence with the trigger flag on.
        backend.sequences()->add(
            QVariantMap{{"name", "Echo"}, {"isSend", false}, {"format", 0},
                        {"data", "Hello"}, {"trigger", true},
                        {"comment", "Snapshot!"}});

        QVERIFY(backend.snapshotArmed() == false);
        backend.toggleSnapshot();
        QVERIFY(backend.snapshotArmed());
        QVERIFY(backend.running());
        QTRY_VERIFY_WITH_TIMEOUT(backend.log()->frozen(), 2000);
        const int hiddenCount = backend.log()->count();
        QThread::msleep(250);
        // Still hidden after more traffic would arrive: write and pump.
        QVERIFY(::write(master, "noise", 5) == 5);
        QThread::msleep(150);
        for (int i = 0; i < 10; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(30);
        }
        // The trigger has not fired yet (nothing matching was sent).
        QVERIFY(backend.snapshotArmed());
        QVERIFY(backend.log()->count() == hiddenCount);

        // Fire the trigger: send Hello; the loopback echoes it back.
        backend.sendSequence(0);
        // Echo every TX back plus extra noise, so the trigger ends up
        // roughly in the middle of the buffered window.
        for (int i = 0; i < 40 && backend.snapshotArmed(); ++i) {
            QCoreApplication::processEvents();
            char buf[256];
            ssize_t n;
            while ((n = ::read(master, buf, sizeof(buf))) > 0)
                QCOMPARE(::write(master, buf, n), n);   // echo
            QVERIFY(::write(master, "noise", 5) == 5);
            QThread::msleep(40);
        }
        QVERIFY(!backend.snapshotArmed());
        // The buffered data is now visible and comm stopped.
        QVERIFY(!backend.log()->frozen());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.running(), 3000);
        QStringList texts;
        for (int i = 0; i < backend.log()->count(); ++i)
            texts << backend.log()->rows()[i].text;
        const QString all = texts.join('\n');
        QVERIFY(all.contains("Hello"));
        QVERIFY(all.contains("Snapshot!"));
        QVERIFY(all.contains("triggered"));
        ::close(master);
    }

    void sequenceRepeatRecomputesChecksum()
    {
        // Docklight's periodic send recalculates checksum areas on every
        // transmission; the repeat path must use the same builder as the
        // manual send.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        const int row = backend.sequences()->add(
            QVariantMap{{"name", "CRC"}, {"isSend", true}, {"format", 1},
                        {"data", "01 04 00 00"}, {"checksumSpec", "CRC-MODBUS"},
                        {"checksumMode", 0}, {"repeatIntervalMs", 80}});
        QString toastText;
        QObject::connect(&backend, &Backend::toast, &backend,
                         [&](const QString &t) { toastText = t; });
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        backend.sendSequence(row);
        QByteArray seen;
        char buffer[128];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 700) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                seen.append(buffer, int(n));
        }
        backend.sendSequence(row);   // disarm
        backend.comm()->stop();
        ::close(master);
        // 01 04 + CRC-MODBUS big-endian? CalcChecksum("CRC-MODBUS","0104") =
        // reflected low byte first. Every repeat must carry the same CRC.
        QString rows;
        for (int i = 0; i < backend.log()->count(); ++i)
            rows += QStringLiteral("[%1]").arg(backend.log()->textAt(i));
        QVERIFY2(seen.size() >= 8, qPrintable("hex=" + QString::fromLatin1(seen.toHex(' '))
                 + " rows=" + rows + " toast=" + toastText));
        QCOMPARE(seen.mid(0, 4), seen.mid(seen.size() - 4, 4));
        QCOMPARE(seen.mid(0, 2), QByteArray("\x01\x04", 2));
    }

    void sendSequenceParameterDialogFlow()
    {
        // Docklight's Send Sequence Parameter dialog: a wildcard sequence
        // sent from the UI emits parameterRequest; the answered values fill
        // the wildcard areas (blank padding like the manual's rules).
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        backend.sequences()->add(QVariantMap{
            {"name", "Set value"}, {"isSend", true}, {"format", 0},
            {"data", "SET=??\r\n"}});
        backend.sequences()->add(QVariantMap{
            {"name", "Plain"}, {"isSend", true}, {"format", 0},
            {"data", "HI"}});
        QCOMPARE(backend.sequenceWildcardAreas(0), QStringList{"2"});
        QCOMPARE(backend.sequenceWildcardAreas(1), QStringList{});

        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        int requestedRow = -1;
        QStringList requestedMin;
        connect(&backend, &Backend::parameterRequest,
                [&](int row, const QStringList &minLens) {
            requestedRow = row;
            requestedMin = minLens;
        });
        backend.sendSequence(0);
        QTRY_COMPARE_WITH_TIMEOUT(requestedRow, 0, 2000);
        QCOMPARE(requestedMin, QStringList{"2"});

        // Answer the dialog: one parameter fills the two-character area,
        // padded with a blank (ASCII 32).
        backend.sendSequenceWithParams(0, QStringList{"7"});
        QByteArray seen;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1000 && seen.size() < 8) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                seen.append(buffer, int(n));
        }
        QCOMPARE(seen, QByteArray("SET=7 \r\n", 8));

        backend.comm()->stop();
        ::close(master);
    }

    void receiveChecksumWithWildcardsMatches()
    {
        // Modbus-style: data with wildcard bytes plus a checksum area over
        // (1,7) with the "L" little-endian modifier, exactly like the
        // manual's Modbus RTU example.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        backend.sequences()->add(QVariantMap{
            {"name", "Resp"}, {"isSend", false}, {"format", 1},
            {"data", "01 03 04 ?? ?? ?? ?? ?? ??"}, {"checksumSpec", "(1,7) CRC-MODBUS L"},
            {"comment", "HIT %_S"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

                QByteArray response("\x01\x03\x04\x00\x01\x02\x03\xEA\x92", 9);
        QCOMPARE(::write(master, response.constData(), response.size()),
                 qint64(response.size()));
        QStringList rows;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1500 && rows.isEmpty()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            for (const auto &row : backend.log()->rows())
                if (row.direction == CommLog::Comment
                    && row.text.startsWith("HIT 01 03 04"))
                    rows.append(row.text);
        }
        QVERIFY(!rows.isEmpty());

        backend.comm()->stop();
        ::close(master);
    }

    void receiveSequenceChecksumValidation()
    {
        // Receive sequences validate checksums per the manual: mode 0 = only
        // correct checksums match, 1 = only wrong ones, 2 = both.
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        const int row = backend.sequences()->add(
            QVariantMap{{"name", "RXCRC"}, {"isSend", false}, {"format", 1},
                        {"data", "55 AA ?? ??"}, {"checksumSpec", "CRC-MODBUS"},
                        {"checksumMode", 2}, {"comment", "HIT %_L"}});
        backend.toggleComm();
        QVERIFY(backend.comm()->running());

        // A valid CRC-MODBUS frame (55 AA 01) and a broken one.
        const quint16 c = [] {
            quint16 v = 0xFFFF;
            const uchar data[3] = {0x55, 0xAA, 0x01};
            for (uchar b : data) {
                v ^= b;
                for (int bit = 0; bit < 8; ++bit)
                    v = (v & 1) ? (v >> 1) ^ 0xA001 : v >> 1;
            }
            return v;   // Modbus sends the low byte first
        }();
        QCOMPARE(Checksum::compute("CRC-MODBUS", QByteArray("\x55\xAA\x01", 3)),
                 QByteArray(1, char(c >> 8)) + QByteArray(1, char(c & 0xFF)));
        const char good[5] = {char(0x55), char(0xAA), char(0x01),
                              char(c & 0xFF), char(c >> 8)};
        const char bad[5] = {char(0x55), char(0xAA), char(0x01), char(0), char(0)};

        ::write(master, good, 5);
        QTest::qWait(200);
        // Events (port opened, comm started) plus RX row and HIT comment.
        const int afterGood = backend.log()->count();
        QVERIFY2(afterGood >= 4, qPrintable(QString::number(afterGood)));

        ::write(master, bad, 5);
        QTest::qWait(200);
        // BOTH mode counts the broken frame as a hit as well: +RX +comment.
        QCOMPARE(backend.log()->count(), afterGood + 2);

        backend.comm()->stop();
        if (master >= 0)
            ::close(master);
    }

    void vbscriptSamplesRunUnchanged()
    {
        // Scripts written in Docklight's VBScript (as printed in the manual)
        // must run unmodified: the engine translates them to JavaScript.
        QVERIFY(VbsTrans::looksLikeVbScript(
            "Dim x\nDL.AddComment \"hi\""));
        QVERIFY(!VbsTrans::looksLikeVbScript(
            "var x = 1;\nDL.AddComment(\"hi\");"));

        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);
        QStringList notes;
        bool finishedOk = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::outputLine, [&](const QString &line, bool error) {
            if (!error)
                notes.append(line);
        });
        connect(&runner, &ScriptRunner::commentRequested,
                [&](const QString &line, bool) {
            notes.append(line);
        });
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &) {
            finishedOk = ok;
            loop.quit();
        });

        // Loop structures, Select Case with To ranges, functions, Subs and
        // string functions — all constructs from the manual's VBScript pages.
        const char *vbs =
            "Counter = 0\n"
            "While Counter < 20\n"
            "  Counter = Counter + 1\n"
            "Wend\n"
            "DL.AddComment \"Counter is \" & Counter\n"
            "Total = 0\n"
            "For i = 1 To 4\n"
            "  Total = Total + i\n"
            "Next\n"
            "DL.AddComment \"Total \" & Total\n"
            "N = 0\n"
            "Do\n"
            "  N = N + 2\n"
            "Loop Until N >= 6\n"
            "DL.AddComment \"N \" & N\n"
            "Value = 7\n"
            "Select Case Value\n"
            "  Case 1 To 5\n"
            "    DL.AddComment \"low\"\n"
            "  Case 6 To 10\n"
            "    DL.AddComment \"mid\"\n"
            "  Case Else\n"
            "    DL.AddComment \"high\"\n"
            "End Select\n"
            "Function Twice(x)\n"
            "  Twice = x * 2\n"
            "End Function\n"
            "DL.AddComment \"Twice \" & Twice(21)\n"
            "Sub Show(text)\n"
            "  DL.AddComment \"Show: \" & text\n"
            "End Sub\n"
            "Show \"done\"\n"
            "s = \"Hello World\"\n"
            "DL.AddComment Left(s, 5) & \"/\" & UCase(Mid(s, 7))\n"
            "If 3 = 3 Then DL.AddComment \"eq\"\n"
            "If 3 <> 4 Then DL.AddComment \"neq\" Else DL.AddComment \"bad\"\n"
            "With DL\n"
            "  .AddComment \"with works\"\n"
            "End With\n"
            "Select Case 10\n"
            "  Case 5, 10\n"
            "    DL.AddComment \"five or ten\"\n"
            "  Case Else\n"
            "    DL.AddComment \"other\"\n"
            "End Select\n"
            "n = 5 _\n"
            "    + 1\n"
            "DL.AddComment \"cont \" & n\n";
        runner.run(QString::fromUtf8(vbs));
        QTRY_VERIFY_WITH_TIMEOUT(finishedOk, 5000);
        QVERIFY2(finishedOk, qPrintable(notes.join(" | ")));

        QStringList joined = notes;
        const QString all = joined.join('\n');
        QVERIFY(all.contains("Counter is 20"));
        QVERIFY(all.contains("Total 10"));
        QVERIFY(all.contains("N 6"));
        QVERIFY(all.contains("mid"));
        QVERIFY(all.contains("Twice 42"));
        QVERIFY(all.contains("Show: done"));
        QVERIFY(all.contains("Hello/WORLD"));
        QVERIFY(all.contains("eq"));
        QVERIFY(all.contains("neq"));
        QVERIFY(all.contains("with works"));
        QVERIFY(all.contains("five or ten"));
        QVERIFY(all.contains("cont 6"));
        QVERIFY(!all.contains("high"));
        QVERIFY(!all.contains("bad"));
    }

    void onSendChecksumExampleFromManual()
    {
        // Manual "Sub DL_OnSend() - Send Sequence Data Manipulation": a
        // random character is poked into the sequence and a MOD256 checksum
        // over the preceding bytes is written to the last byte. The manual's
        // sample output is "Checksum on 54 65 73 74 53 0D is 00(Hex), ...".
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        Backend backend;
        backend.channelA()->setDevice(slave);
        // QString(const char*) truncates at embedded NULs — build the
        // 7-byte "TestX<CR><NUL>" payload explicitly.
        const QString testData =
            QString::fromLatin1("TestX\r", 6) + QChar(0);
        backend.sequences()->add(QVariantMap{
            {"name", "Test"}, {"isSend", true}, {"format", 0},
            {"data", testData}});
        QStringList notes;
        QEventLoop loop;
        bool done = false;
        connect(backend.scripts(), &ScriptRunner::commentRequested,
                [&](const QString &line, bool) { notes.append(line); });
        connect(backend.scripts(), &ScriptRunner::finished,
                [&](bool, const QString &) { done = true; loop.quit(); });
        const char *vbs =
            "DL.SendSequence \"Test\"\n"
            "DL.Pause 100\n"
            "DL.Quit\n"
            "Sub DL_OnSend()\n"
            "  seqSize = DL.OnSend_GetSize()\n"
            "  If seqSize > 2 Then\n"
            "    DL.OnSend_Poke seqSize - 2, 65 + Rnd * 25\n"
            "    chksumHex = DL.CalcChecksum(\"MOD256\", DL.OnSend_GetData(\"H\"), \"H\", 1, seqSize - 1)\n"
            "    DL.OnSend_Poke seqSize, CInt (\"&h\" + chksumHex)\n"
            "    DL.AddComment vbCrLf & vbCrLf\n"
            "    DL.AddComment \"Checksum on\", False, False\n"
            "    For i = 1 To seqSize - 1\n"
            "      DL.AddComment \" \" & DL.OnSend_Peek(i, \"H\"), False, False\n"
            "    Next\n"
            "    DL.AddComment \" is \" & DL.OnSend_Peek(seqSize, \"H\") & \"(Hex), \" & DL.OnSend_Peek(seqSize, \"D\") & \"(Decimal)\"\n"
            "  End If\n"
            "End Sub\n";
        backend.toggleComm();   // the manual example starts communication first
        backend.runScript(vbs);
        loop.exec();
        QVERIFY(done);
        backend.comm()->stop();

        // One merged comment line per send, in the manual's format.
        QString merged;
        for (int i = 0; i < notes.size(); ++i)
            if (notes.at(i).contains("Checksum on")) {
                merged = notes.at(i);
                for (int j = i + 1; j < notes.size()
                     && !notes.at(j).contains("\r\n"); ++j)
                    merged += notes.at(j);
            }
        QVERIFY(merged.contains("(Hex), "));
        QRegularExpression re(
            "Checksum on 54 65 73 74 [4-5][0-9A-F] 0D is [0-9A-F]{2}\\(Hex\\),"
            " \\d+\\(Decimal\\)");
        QVERIFY(re.match(merged).hasMatch());

        // The transmitted bytes end with the checksum of the first six.
        QByteArray got;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1500 && got.size() < 7) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0)
                got.append(buffer, int(n));
        }
        QVERIFY(got.size() >= 7);
        const uchar *p = reinterpret_cast<const uchar *>(got.constData());
        QCOMPARE(int(p[0]), 0x54);
        QCOMPARE(int(p[1]), 0x65);
        QCOMPARE(int(p[2]), 0x73);
        QCOMPARE(int(p[3]), 0x74);
        QVERIFY(p[4] >= 65 && p[4] <= 90);           // random letter
        QCOMPARE(int(p[5]), 0x0D);
        int sum = 0;
        for (int i = 0; i < 6; ++i)
            sum += int(p[i]);
        QCOMPARE(int(p[6]), sum & 0xFF);             // MOD256 checksum byte

        backend.comm()->stop();
        ::close(master);
    }

    void debugObjectMatchesManual()
    {
        // Manual "Debug object / Script Debugging": Debug.Mode = 1 enables
        // PrintMsg/Assert/Break; with mode 0 the calls do nothing.
        CommManager manager;
        SequenceModel sequences;
        ScriptRunner runner(&manager, &sequences, nullptr);
        QStringList notes;
        bool finishedOk = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::commentRequested,
                [&](const QString &line, bool) { notes.append(line); });
        connect(&runner, &ScriptRunner::finished,
                [&](bool ok, const QString &) {
            finishedOk = ok;
            loop.quit();
        });
        runner.run("Debug.Mode = 1;\n"
                   "for (var count = 1; count <= 3; ++count)\n"
                   "  Debug.PrintMsg(\"count = \" + count);\n"
                   "Debug.Mode = 0;\n"
                   "Debug.PrintMsg(\"never\");\n"
                   "Debug.Break();\n"
                   "DL.AddComment(\"after break\");");
        loop.exec();
        QVERIFY(finishedOk);
        QCOMPARE(notes.size(), 4);
        QVERIFY(notes.at(0).endsWith("Debug: count = 1"));
        QVERIFY(notes.at(2).endsWith("Debug: count = 3"));
        QCOMPARE(notes.at(3), QStringLiteral("after break"));
    }

    void scriptEchoesThroughRealPort()
    {
        char slave[128];
        int master = makePty(slave, sizeof(slave));
        QVERIFY(master >= 0);

        CommManager manager;
        SequenceModel sequences;
        SerialConfig config;
        config.device = slave;
        manager.start(config, SerialConfig());

        ScriptRunner runner(&manager, &sequences, nullptr);
        QString lastLine;
        bool finishedOk = false;
        QEventLoop loop;
        connect(&runner, &ScriptRunner::outputLine, [&](const QString &line, bool error) {
            if (!error)
                lastLine = line;
        });
        connect(&runner, &ScriptRunner::finished, [&](bool ok, const QString &) {
            finishedOk = ok;
            loop.quit();
        });
        runner.run("DL.StartCommunication();"
                   "DL.SendStr(\"hello device\\r\\n\");"
                   "var line = DL.ReceiveLine(2000);"
                   "print(line);"
                   "DL.Assert(line === \"hello device\", \"echo mismatch\");");
        QTest::qWait(200);
        QVERIFY(runner.engine());

        // The remote end echoes every line back, like a real device.
        QByteArray pending;
        char buffer[64];
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000 && !finishedOk) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n > 0) {
                pending.append(buffer, int(n));
                if (pending.endsWith('\n')) {
                    ::write(master, pending.constData(), pending.size());
                    pending.clear();
                }
            }
        }
        QVERIFY(finishedOk);
        QCOMPARE(lastLine, QString("hello device"));
        manager.stop();
        ::close(master);
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(LucidlightTests)
#include "lucidlight_tests.moc"
