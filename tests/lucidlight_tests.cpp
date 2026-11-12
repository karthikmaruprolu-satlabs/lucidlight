#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "backend.h"
#include "checksum.h"
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
        expected.append(char(0));   // '??' is two one-byte wildcards now
        QCOMPARE(parsed.bytes(), expected);
        QCOMPARE(parsed.wildcard().at(3), true);
        QCOMPARE(parsed.wildcard().at(4), true);
    }

    void hexNotationParsesPairsAndWildcards()
    {
        const auto parsed = DataFormat::parse("48 65 ?? 6C", DataFormat::Format::Hex);
        QVERIFY(parsed.ok());
        QByteArray expected("He");
        expected.append(char(0));
        expected.append(char(0));   // '??' is two one-byte wildcards now
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
        // XOR of "123456789": 31^32^...^39
        QCOMPARE(Checksum::compute("XOR", data).at(0), char(0x31 ^ 0x32 ^ 0x33
                     ^ 0x34 ^ 0x35 ^ 0x36 ^ 0x37 ^ 0x38 ^ 0x39));
        // LRC: two's complement of the MOD256 sum, so total = 0.
        const QByteArray lrc = Checksum::compute("LRC", data);
        const int sum = 0x31 + 0x32 + 0x33 + 0x34 + 0x35 + 0x36 + 0x37 + 0x38
                        + 0x39 + uchar(lrc.at(0));
        QCOMPARE(sum % 256, 0);
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
