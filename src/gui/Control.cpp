#include "Control.h"

#include "ArrangeDialog.h"
#include "ExportDialog.h"
#include "History.h"
#include "MainWindow.h"
#include "MasterDialog.h"
#include "PianoRoll.h"
#include "ScoreDialog.h"
#include "PluginDiscovery.h"
#include "PluginWindows.h"
#include "ScriptJob.h"
#include "StemDialog.h"
#include "TranscribeDialog.h"
#include "TuneDialog.h"
#include "Widgets.h"
#include "control/Tools.h"
#include "core/Chords.h"
#include "core/ClipEdit.h"
#include "core/Scales.h"

#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTextDocumentFragment>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

// a call that can't be done: its reason goes back to the assistant
struct Fail {
    QString why;
};

double toDb(float gain)
{
    return gain <= 1e-6f ? -120.0 : std::round(200.0 * std::log10(gain)) / 10.0;
}

float fromDb(double db)
{
    return db <= -60.0 ? 0.0f : std::min(1.27f, static_cast<float>(std::pow(10.0, db / 20.0)));
}

QString path(const QString& file)
{
    return QDir::toNativeSeparators(file);
}

} // namespace

Control::Control(MainWindow& window)
    : QObject(&window)
    , fWindow(window)
{
    fPoll.setInterval(300);
    connect(&fPoll, &QTimer::timeout, this, &Control::pollJobs);
    // the tempo and key analysis has no window: its job follows the script
    connect(fWindow.fAnalysis, &ScriptJob::finished, this, [this](bool ok, const QString& message) {
        for (Job& job : fJobs)
            if (job.kind == QLatin1String("analyze") && !job.done)
            {
                job.done = true;
                job.percent = 100;
                job.message = ok ? QStringLiteral("tempo %1 BPM, key %2")
                                       .arg(fWindow.fSession.tempo())
                                       .arg(fWindow.fSession.key().empty() ? QStringLiteral("unknown") : QString::fromStdString(fWindow.fSession.key()))
                                 : message;
            }
    });
}

Control::~Control() = default;

bool Control::listen()
{
    const QString name = control::serverName();
    // a socket left behind by a chost that crashed answers nobody: it can go
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(300))
        return false;
    QLocalServer::removeServer(name);
    fServer = new QLocalServer(this);
    fServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!fServer->listen(name))
    {
        qWarning("remote control: %s", qPrintable(fServer->errorString()));
        return false;
    }
    connect(fServer, &QLocalServer::newConnection, this, &Control::onConnection);
    return true;
}

void Control::onConnection()
{
    while (QLocalSocket* socket = fServer->nextPendingConnection())
    {
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void Control::onReadyRead(QLocalSocket* socket)
{
    while (socket->canReadLine())
    {
        const QByteArray line = socket->readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const QJsonValue id = request.value(QStringLiteral("id"));
        QPointer<QLocalSocket> to(socket);
        call(request.value(QStringLiteral("tool")).toString(), request.value(QStringLiteral("args")).toObject(),
             [to, id](bool ok, const QJsonValue& result, const QString& error) {
                 if (!to)
                     return;
                 QJsonObject response{{QStringLiteral("id"), id}, {QStringLiteral("ok"), ok}};
                 if (ok)
                     response.insert(QStringLiteral("result"), result);
                 else
                     response.insert(QStringLiteral("error"), error);
                 to->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                 to->flush();
             });
    }
}

bool Control::knows(const QString& tool) const
{
    if (tool == QLatin1String("hello"))
        return true;
    for (const QJsonValue& t : control::tools())
        if (t.toObject().value(QStringLiteral("name")).toString() == tool)
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// the tools

void Control::call(const QString& tool, const QJsonObject& args, Reply reply)
{
    MainWindow& w = fWindow;
    chost::Session& session = w.fSession;
    chost::HostEngine& host = w.fHost;
    const std::vector<chost::Track>& tracks = session.tracks();

    // errors the window would show in a message box come back here instead
    QStringList errors;
    struct Sink {
        MainWindow& w;
        Sink(MainWindow& window, QStringList* errors) : w(window) { w.fErrorSink = errors; }
        ~Sink() { w.fErrorSink = nullptr; }
    } sink(w, &errors);
    auto check = [&] {
        if (!errors.isEmpty())
            throw Fail{errors.join(QLatin1Char('\n'))};
    };

    auto has = [&](const char* key) { return args.contains(QLatin1String(key)); };
    auto trackArg = [&]() -> int {
        if (!args.value(QStringLiteral("track")).isDouble())
            throw Fail{QStringLiteral("track (an index from get_project) is needed")};
        const int track = args.value(QStringLiteral("track")).toInt(-1);
        if (track < 0 || track >= static_cast<int>(tracks.size()))
            throw Fail{QStringLiteral("there is no track %1: the project has %2 tracks (0 to %3)")
                           .arg(track).arg(tracks.size()).arg(static_cast<int>(tracks.size()) - 1)};
        return track;
    };
    auto insertArg = [&](int track) -> int {
        const int insert = args.value(QStringLiteral("insert")).toInt(-1);
        const int count = static_cast<int>(tracks[track].inserts.size());
        if (insert < 0 || insert >= count)
            throw Fail{QStringLiteral("track %1 has no insert %2 (it has %3)").arg(track).arg(insert).arg(count)};
        return insert;
    };
    // the plugin a call is about: an insert of the track, or its instrument
    auto pluginArg = [&](int track) -> uint {
        if (args.value(QStringLiteral("instrument")).toBool())
        {
            if (tracks[track].instrument < 0)
                throw Fail{QStringLiteral("track %1 has no instrument").arg(track)};
            return static_cast<uint>(tracks[track].instrument);
        }
        if (!has("insert"))
            throw Fail{QStringLiteral("say which plugin: insert (an index) or instrument: true")};
        return tracks[track].inserts[static_cast<size_t>(insertArg(track))];
    };
    // a spec as given, a plugin file, or the installed plugin of that name
    auto resolvePlugin = [&](const QString& text, bool instrument) -> QString {
        static const QRegularExpression spec(QStringLiteral("^(lv2|vst3|vst2|vst2-32|vst|ladspa|sf2|sfz):"), QRegularExpression::CaseInsensitiveOption);
        if (spec.match(text).hasMatch())
            return text;
        const QFileInfo file(text);
        if (file.exists())
        {
            const QString suffix = file.suffix().toLower();
            if (suffix == QLatin1String("vst3") || suffix == QLatin1String("sf2") || suffix == QLatin1String("sfz"))
                return suffix + QLatin1Char(':') + QDir::fromNativeSeparators(file.absoluteFilePath());
            if (suffix == QLatin1String("dll") || suffix == QLatin1String("so"))
                return QStringLiteral("vst2:") + QDir::fromNativeSeparators(file.absoluteFilePath());
        }
        std::vector<const chost::PluginEntry*> exact, partial;
        for (const chost::PluginEntry& p : w.fPlugins)
        {
            const QString name = QString::fromStdString(p.name);
            if (name.compare(text, Qt::CaseInsensitive) == 0)
                exact.push_back(&p);
            else if (name.contains(text, Qt::CaseInsensitive))
                partial.push_back(&p);
        }
        for (auto* list : {&exact, &partial})
        {
            // an instrument where one is wanted, an effect for an insert; VST3 before VST2 of the same plugin
            std::stable_sort(list->begin(), list->end(), [instrument](const chost::PluginEntry* a, const chost::PluginEntry* b) {
                if ((a->isSynth == instrument) != (b->isSynth == instrument))
                    return a->isSynth == instrument;
                return (a->format == "VST3") > (b->format == "VST3");
            });
            if (list == &partial && list->size() > 1 && (*list)[0]->name != (*list)[1]->name)
            {
                QStringList names;
                for (size_t i = 0; i < list->size() && i < 8; ++i)
                    names << QString::fromStdString((*list)[i]->name);
                throw Fail{QStringLiteral("\"%1\" matches several plugins: %2. Give the full name or the spec (find_plugins).")
                               .arg(text, names.join(QStringLiteral(", ")))};
            }
            if (!list->empty())
                return QString::fromStdString(list->front()->spec);
        }
        throw Fail{w.fPlugins.empty() ? QStringLiteral("chost has not finished finding the installed plugins yet; try again in a moment")
                                      : QStringLiteral("no installed plugin is called \"%1\"; find_plugins lists them").arg(text)};
    };
    auto pluginJson = [&](uint id) {
        const chost::RackSlot slot = host.slot(id);
        return QJsonObject{{QStringLiteral("name"), QString::fromStdString(slot.name)},
                           {QStringLiteral("maker"), QString::fromStdString(slot.maker)},
                           {QStringLiteral("active"), slot.active},
                           {QStringLiteral("has_window"), slot.hasCustomUi}};
    };
    auto parametersJson = [&](uint id, const QString& query) {
        QJsonArray list;
        for (const chost::ParamInfo& p : host.parameters(id))
        {
            const QString name = QString::fromStdString(p.name);
            if (!query.isEmpty() && !name.contains(query, Qt::CaseInsensitive))
                continue;
            QJsonObject o{{QStringLiteral("index"), static_cast<int>(p.index)}, {QStringLiteral("name"), name},
                          {QStringLiteral("value"), host.parameterValue(id, p.index)}, {QStringLiteral("min"), p.min},
                          {QStringLiteral("max"), p.max}, {QStringLiteral("default"), p.def}};
            if (!p.unit.empty())
                o.insert(QStringLiteral("unit"), QString::fromStdString(p.unit));
            if (p.isOutput)
                o.insert(QStringLiteral("output"), true);
            if (p.isBoolean)
                o.insert(QStringLiteral("boolean"), true);
            else if (p.isInteger)
                o.insert(QStringLiteral("integer"), true);
            list.append(o);
        }
        return list;
    };
    auto kindName = [](chost::TrackKind kind) {
        return kind == chost::TrackKind::Audio ? QStringLiteral("audio") : kind == chost::TrackKind::Bus ? QStringLiteral("bus") : QStringLiteral("instrument");
    };
    auto modified = [&] { return w.fHistory->isModified() && !tracks.empty(); };
    auto okTrack = [&](int track) {
        return QJsonObject{{QStringLiteral("track"), track}, {QStringLiteral("name"), QString::fromStdString(tracks[track].name)}};
    };
    auto markersJson = [&] {
        QJsonArray list;
        const std::vector<chost::Marker>& markers = session.markers();
        for (size_t i = 0; i < markers.size(); ++i)
            list.append(QJsonObject{{QStringLiteral("index"), static_cast<int>(i)},
                                    {QStringLiteral("name"), QString::fromStdString(markers[i].name)},
                                    {QStringLiteral("seconds"), std::round(markers[i].seconds * 1000.0) / 1000.0},
                                    {QStringLiteral("bar"), std::round(markers[i].seconds * session.tempo() / 240.0 * 100.0) / 100.0 + 1.0}});
        return list;
    };
    // the AI dialogs come up fresh for the track asked for, never with the choice of an earlier one
    auto freshDialog = [&](QWidget* dialog, const QString& what) {
        if (!dialog)
            return;
        if (dialog->property("busy").toBool())
            throw Fail{QStringLiteral("%1 is already running; wait_for_jobs").arg(what)};
        delete dialog;
    };

    try
    {
        QJsonValue result;
        if (tool == QLatin1String("hello"))
        {
            result = QJsonObject{{QStringLiteral("app"), QStringLiteral("chost")}, {QStringLiteral("version"), QStringLiteral(CHOST_VERSION)},
                                 {QStringLiteral("protocol"), control::protocolVersion()}};
        }
        else if (tool == QLatin1String("get_project"))
        {
            QJsonArray list;
            for (size_t i = 0; i < tracks.size(); ++i)
            {
                const chost::Track& t = tracks[i];
                QJsonObject o{{QStringLiteral("index"), static_cast<int>(i)},
                              {QStringLiteral("name"), QString::fromStdString(t.name)},
                              {QStringLiteral("kind"), kindName(t.kind)},
                              {QStringLiteral("volume_db"), toDb(t.volume)},
                              {QStringLiteral("muted"), t.muted},
                              {QStringLiteral("solo"), t.solo}};
                if (!t.role.empty())
                    o.insert(QStringLiteral("role"), QString::fromStdString(t.role));
                if (!t.file.empty())
                {
                    o.insert(QStringLiteral("file"), path(QString::fromStdString(t.file)));
                    const auto clip = w.fClips.constFind(QString::fromStdString(t.file));
                    if (clip != w.fClips.constEnd() && clip->seconds > 0.0)
                        o.insert(QStringLiteral("seconds"), std::round(clip->seconds * 100.0) / 100.0);
                }
                if (t.instrument >= 0)
                {
                    QJsonObject instrument = pluginJson(static_cast<uint>(t.instrument));
                    instrument.insert(QStringLiteral("spec"), QString::fromStdString(t.instrumentSpec));
                    o.insert(QStringLiteral("instrument"), instrument);
                }
                QJsonArray inserts;
                for (size_t k = 0; k < t.inserts.size(); ++k)
                {
                    QJsonObject insert = pluginJson(t.inserts[k]);
                    insert.insert(QStringLiteral("index"), static_cast<int>(k));
                    inserts.append(insert);
                }
                o.insert(QStringLiteral("inserts"), inserts);
                if (session.trackOf(t.output) >= 0)
                    o.insert(QStringLiteral("output"), session.trackOf(t.output));
                QJsonArray sends;
                for (const chost::Send& send : t.sends)
                    if (session.trackOf(send.bus) >= 0)
                        sends.append(QJsonObject{{QStringLiteral("bus"), session.trackOf(send.bus)}, {QStringLiteral("level_db"), toDb(send.level)}});
                if (!sends.isEmpty())
                    o.insert(QStringLiteral("sends"), sends);
                list.append(o);
            }
            const double rate = host.sampleRate();
            QJsonObject project{{QStringLiteral("file"), w.fSessionPath.isEmpty() ? QJsonValue() : QJsonValue(path(w.fSessionPath))},
                                {QStringLiteral("unsaved_changes"), modified()},
                                {QStringLiteral("tempo"), session.tempo()},
                                {QStringLiteral("key"), session.key().empty() ? QJsonValue() : QJsonValue(QString::fromStdString(session.key()))},
                                {QStringLiteral("master_volume_db"), toDb(session.masterVolume())},
                                {QStringLiteral("playing"), host.isPlaying()},
                                {QStringLiteral("position_seconds"), rate > 0.0 ? std::round(host.transportFrame() / rate * 100.0) / 100.0 : 0.0},
                                {QStringLiteral("selected_track"), w.fSelected},
                                {QStringLiteral("audio_driver"), QString::fromStdString(host.driverName())},
                                {QStringLiteral("tracks"), list},
                                {QStringLiteral("markers"), markersJson()},
                                {QStringLiteral("metronome"), session.clickOn()}};
            if (!session.isRouted())
                project.insert(QStringLiteral("warning"), QStringLiteral("this audio driver plays no multitrack mix: choose another driver in chost"));
            result = project;
        }
        else if (tool == QLatin1String("new_project") || tool == QLatin1String("open_project"))
        {
            const bool open = tool == QLatin1String("open_project");
            const QString file = args.value(QStringLiteral("path")).toString();
            if (open && !QFileInfo::exists(file))
                throw Fail{QStringLiteral("no such file: %1").arg(file)};
            if (modified() && !args.value(QStringLiteral("discard_unsaved")).toBool())
                throw Fail{QStringLiteral("the project has unsaved changes: save_project first, or ask the user whether to discard them (discard_unsaved: true)")};
            if (open)
            {
                if (!w.openSession(file))
                    check();
            }
            else
                w.resetSession();
            check();
            result = QJsonObject{{QStringLiteral("tracks"), static_cast<int>(tracks.size())}};
        }
        else if (tool == QLatin1String("save_project"))
        {
            QString file = args.value(QStringLiteral("path")).toString();
            if (file.isEmpty() && w.fSessionPath.isEmpty())
                throw Fail{QStringLiteral("the project was never saved: give a path for it (ask the user where)")};
            if (!file.isEmpty())
            {
                if (QFileInfo(file).suffix().isEmpty())
                    file += QStringLiteral(".chost");
                w.fSessionPath = QFileInfo(file).absoluteFilePath();
                w.updateTitle();
            }
            w.saveSession();
            check();
            result = QJsonObject{{QStringLiteral("file"), path(w.fSessionPath)}};
        }
        else if (tool == QLatin1String("add_track"))
        {
            const QString file = args.value(QStringLiteral("file")).toString();
            const QString name = args.value(QStringLiteral("name")).toString();
            const int before = static_cast<int>(tracks.size());
            int track = -1;
            if (!file.isEmpty())
            {
                if (!QFileInfo::exists(file))
                    throw Fail{QStringLiteral("no such file: %1").arg(file)};
                track = w.addAudioTrack(file, name);
            }
            else
            {
                if (args.value(QStringLiteral("kind")).toString() == QLatin1String("instrument"))
                    w.addInstrumentTrack();
                else
                    w.addEmptyTrack();
                track = static_cast<int>(tracks.size()) > before ? static_cast<int>(tracks.size()) - 1 : -1;
                if (track >= 0 && !name.isEmpty())
                {
                    w.selectTrack(track);
                    w.renameSelectedTrack(name);
                }
            }
            check();
            if (track < 0)
                throw Fail{QStringLiteral("the track could not be added")};
            // the first song of a project: chost finds its tempo and key by itself, as a job to wait for
            if (w.fAnalysis->isRunning()
                && std::none_of(fJobs.begin(), fJobs.end(), [](const Job& job) { return job.kind == QLatin1String("analyze") && !job.done; }))
                startJob(QStringLiteral("analyze"), QString::fromStdString(tracks[track].name), nullptr);
            QJsonObject o = okTrack(track);
            o.insert(QStringLiteral("kind"), kindName(tracks[track].kind));
            if (tracks[track].instrument >= 0)
                o.insert(QStringLiteral("instrument"), QString::fromStdString(host.slot(static_cast<uint>(tracks[track].instrument)).name));
            result = o;
        }
        else if (tool == QLatin1String("remove_track"))
        {
            const int track = trackArg();
            const QString name = QString::fromStdString(tracks[track].name);
            w.selectTrack(track);
            w.removeSelectedTrack();
            result = QJsonObject{{QStringLiteral("removed"), name}, {QStringLiteral("tracks"), static_cast<int>(tracks.size())}};
        }
        else if (tool == QLatin1String("set_track"))
        {
            const int track = trackArg();
            if (has("file"))
            {
                const QString file = args.value(QStringLiteral("file")).toString();
                if (!QFileInfo::exists(file))
                    throw Fail{QStringLiteral("no such file: %1").arg(file)};
                if (!w.fits(track, file))
                    throw Fail{tracks[track].kind == chost::TrackKind::Audio
                                   ? QStringLiteral("track %1 is an audio track: it plays audio files, not MIDI (add_track makes a MIDI file an instrument track)").arg(track)
                                   : QStringLiteral("track %1 is an instrument track: it plays MIDI files").arg(track)};
                w.closeEditors();
                if (!session.setTrackFile(track, file.toStdString()))
                    throw Fail{QString::fromStdString(session.lastError())};
                w.refreshTracks();
                w.fHistory->record(QStringLiteral("替换音频"));
            }
            if (has("name"))
            {
                w.selectTrack(track);
                w.renameSelectedTrack(args.value(QStringLiteral("name")).toString());
            }
            if (has("muted"))
                w.setMuted(track, args.value(QStringLiteral("muted")).toBool());
            if (has("solo"))
                w.setSolo(track, args.value(QStringLiteral("solo")).toBool());
            if (has("volume_db"))
            {
                w.setVolume(track, fromDb(args.value(QStringLiteral("volume_db")).toDouble()));
                w.refreshMixState();
            }
            check();
            QJsonObject o = okTrack(track);
            o.insert(QStringLiteral("volume_db"), toDb(tracks[track].volume));
            o.insert(QStringLiteral("muted"), tracks[track].muted);
            o.insert(QStringLiteral("solo"), tracks[track].solo);
            result = o;
        }
        else if (tool == QLatin1String("set_master"))
        {
            session.setMasterVolume(fromDb(args.value(QStringLiteral("volume_db")).toDouble()));
            w.fOutputStrip->setVolume(session.masterVolume());
            w.fHistory->record(QStringLiteral("主音量"), QStringLiteral("master-volume"));
            result = QJsonObject{{QStringLiteral("master_volume_db"), toDb(session.masterVolume())}};
        }
        else if (tool == QLatin1String("set_song"))
        {
            if (has("tempo"))
            {
                const double bpm = args.value(QStringLiteral("tempo")).toDouble();
                if (bpm < 20.0 || bpm > 400.0)
                    throw Fail{QStringLiteral("a tempo between 20 and 400 BPM, please")};
                w.setTempo(bpm);
                w.fTempoBox->blockSignals(true);
                w.fTempoBox->setValue(bpm);
                w.fTempoBox->blockSignals(false);
            }
            if (has("key"))
            {
                session.setKey(args.value(QStringLiteral("key")).toString().trimmed().toStdString());
                w.refreshSongInfo();
                w.fHistory->record(QStringLiteral("调性"));
            }
            result = QJsonObject{{QStringLiteral("tempo"), session.tempo()},
                                 {QStringLiteral("key"), session.key().empty() ? QJsonValue() : QJsonValue(QString::fromStdString(session.key()))}};
        }
        else if (tool == QLatin1String("find_plugins"))
        {
            const QStringList words = args.value(QStringLiteral("query")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const QString type = args.value(QStringLiteral("type")).toString(QStringLiteral("any"));
            const int limit = std::clamp(args.value(QStringLiteral("limit")).toInt(40), 1, 500);
            QJsonArray list;
            int found = 0;
            for (const chost::PluginEntry& p : w.fPlugins)
            {
                if ((type == QLatin1String("effect") && p.isSynth) || (type == QLatin1String("instrument") && !p.isSynth))
                    continue;
                const QString text = QString::fromStdString(p.name + ' ' + p.maker + ' ' + p.format + ' ' + p.category);
                if (!std::all_of(words.begin(), words.end(), [&](const QString& word) { return text.contains(word, Qt::CaseInsensitive); }))
                    continue;
                if (++found > limit)
                    continue;
                QJsonObject o{{QStringLiteral("name"), QString::fromStdString(p.name)}, {QStringLiteral("spec"), QString::fromStdString(p.spec)},
                              {QStringLiteral("format"), QString::fromStdString(p.format)}, {QStringLiteral("instrument"), p.isSynth}};
                if (!p.maker.empty())
                    o.insert(QStringLiteral("maker"), QString::fromStdString(p.maker));
                if (!p.category.empty())
                    o.insert(QStringLiteral("category"), QString::fromStdString(p.category));
                list.append(o);
            }
            QJsonObject o{{QStringLiteral("plugins"), list}, {QStringLiteral("found"), found}};
            if (found > limit)
                o.insert(QStringLiteral("note"), QStringLiteral("only the first %1 shown: narrow the query or raise limit").arg(limit));
            if (w.fDiscovery && w.fDiscovery->isRunning())
                o.insert(QStringLiteral("note"), QStringLiteral("chost is still identifying plugins; more may show up shortly"));
            result = o;
        }
        else if (tool == QLatin1String("add_insert"))
        {
            const int track = trackArg();
            const QString spec = resolvePlugin(args.value(QStringLiteral("plugin")).toString(), false);
            w.selectTrack(track);
            if (!w.addPluginSpec(spec))
                check();
            check();
            const int insert = static_cast<int>(tracks[track].inserts.size()) - 1;
            const uint id = tracks[track].inserts.back();
            QJsonObject o = pluginJson(id);
            o.insert(QStringLiteral("track"), track);
            o.insert(QStringLiteral("insert"), insert);
            o.insert(QStringLiteral("spec"), spec);
            o.insert(QStringLiteral("parameters"), parametersJson(id, {}));
            result = o;
        }
        else if (tool == QLatin1String("remove_insert"))
        {
            const int track = trackArg();
            const int insert = insertArg(track);
            const QString name = QString::fromStdString(host.slot(tracks[track].inserts[static_cast<size_t>(insert)]).name);
            w.selectTrack(track);
            w.removeInsert(insert);
            result = QJsonObject{{QStringLiteral("removed"), name}};
        }
        else if (tool == QLatin1String("set_insert"))
        {
            const int track = trackArg();
            int insert = insertArg(track);
            w.selectTrack(track);
            if (has("active"))
                w.setInsertActive(insert, args.value(QStringLiteral("active")).toBool());
            if (const int move = args.value(QStringLiteral("move")).toInt(0); move != 0)
            {
                const int count = static_cast<int>(tracks[track].inserts.size());
                const int to = std::clamp(insert + move, 0, count - 1);
                w.moveInsert(insert, to - insert);
                insert = to;
            }
            QJsonArray chain;
            for (uint id : tracks[track].inserts)
                chain.append(pluginJson(id));
            result = QJsonObject{{QStringLiteral("insert"), insert}, {QStringLiteral("inserts"), chain}};
        }
        else if (tool == QLatin1String("set_instrument"))
        {
            const int track = trackArg();
            if (tracks[track].kind != chost::TrackKind::Instrument)
                throw Fail{QStringLiteral("track %1 is an audio track; instruments go on instrument tracks").arg(track)};
            const QString spec = resolvePlugin(args.value(QStringLiteral("plugin")).toString(), true);
            w.closeEditors();
            if (!session.setInstrument(track, spec.toStdString()))
                throw Fail{QStringLiteral("loading %1 failed: %2").arg(spec, QString::fromStdString(session.lastError()))};
            w.refreshTracks();
            w.selectTrack(track);
            w.fHistory->record(QStringLiteral("换乐器"));
            QJsonObject o = pluginJson(static_cast<uint>(tracks[track].instrument));
            o.insert(QStringLiteral("spec"), spec);
            result = o;
        }
        else if (tool == QLatin1String("apply_instruments"))
        {
            w.applyInstruments();
            check();
            QJsonArray list;
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].instrument >= 0)
                    list.append(QJsonObject{{QStringLiteral("track"), static_cast<int>(i)}, {QStringLiteral("name"), QString::fromStdString(tracks[i].name)},
                                            {QStringLiteral("instrument"), QString::fromStdString(host.slot(static_cast<uint>(tracks[i].instrument)).name)}});
            result = QJsonObject{{QStringLiteral("instrument_tracks"), list}};
        }
        else if (tool == QLatin1String("suggest_chords"))
        {
            const QString keyText = args.value(QStringLiteral("key")).toString(QString::fromStdString(session.key()));
            chost::Key key = chost::parseKey(keyText.toStdString());
            const bool guessed = !key.valid;
            if (!key.valid)
                key = chost::Key{0, false, true};
            chost::Chord after;
            if (has("after"))
            {
                after = chost::parseChord(args.value(QStringLiteral("after")).toString().toStdString());
                if (!after.valid())
                    throw Fail{QStringLiteral("not a chord: %1").arg(args.value(QStringLiteral("after")).toString())};
            }
            const std::vector<chost::Pad> pads = chost::chordPads(key, args.value(QStringLiteral("sevenths")).toBool());
            const std::vector<float> scores = chost::nextChordScores(key, pads, after.valid() ? &after : nullptr);
            std::vector<size_t> order(pads.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return scores[a] > scores[b]; });
            QJsonArray list;
            for (size_t i : order)
                list.append(QJsonObject{{QStringLiteral("chord"), QString::fromStdString(pads[i].chord.name(key))},
                                        {QStringLiteral("numeral"), QString::fromStdString(pads[i].numeral)},
                                        {QStringLiteral("score"), std::round(scores[i] * 100.0) / 100.0},
                                        {QStringLiteral("in_key"), i < 7}});
            QJsonObject o{{QStringLiteral("key"), QString::fromStdString(chost::keyName(key))}, {QStringLiteral("chords"), list}};
            if (guessed)
                o.insert(QStringLiteral("note"), QStringLiteral("the song's key is not known (ai_analyze finds it): C major assumed"));
            result = o;
        }
        else if (tool == QLatin1String("add_chord_track"))
        {
            std::string error;
            const std::vector<chost::ProgressionChord> chords = chost::parseProgression(args.value(QStringLiteral("chords")).toString().toStdString(), error);
            if (chords.empty())
                throw Fail{QString::fromStdString(error)};
            const QString patternText = args.value(QStringLiteral("pattern")).toString(QStringLiteral("block"));
            const chost::ChordPattern pattern = patternText == QLatin1String("arpeggio") ? chost::ChordPattern::Arpeggio
                                                : patternText == QLatin1String("rhythm") ? chost::ChordPattern::Rhythm
                                                                                         : chost::ChordPattern::Block;
            const int bar = std::max(1, args.value(QStringLiteral("start_bar")).toInt(1));
            const double start = (bar - 1) * 4.0 * 60.0 / session.tempo();
            const chost::MidiClip clip = chost::chordClip(chords, session.tempo(), pattern, start);
            const QString folder = w.chordFolder();
            QDir().mkpath(folder);
            QString file;
            for (int n = 1; file.isEmpty() || QFileInfo::exists(file); ++n)
                file = QDir(folder).filePath(QStringLiteral("和弦_%1.mid").arg(n));
            if (!chost::writeMidiFile(file.toStdString(), clip, error))
                throw Fail{QString::fromStdString(error)};
            const int track = w.addMidiTrack(file, args.value(QStringLiteral("name")).toString(QStringLiteral("和弦")), QStringLiteral("piano"));
            check();
            if (track < 0)
                throw Fail{QStringLiteral("the chord track could not be added")};
            QJsonObject o = okTrack(track);
            o.insert(QStringLiteral("chords"), QString::fromStdString(chost::progressionText(chords, chost::parseKey(session.key()))));
            o.insert(QStringLiteral("notes"), static_cast<int>(clip.notes.size()));
            o.insert(QStringLiteral("seconds"), std::round(clip.seconds * 100.0) / 100.0);
            o.insert(QStringLiteral("file"), path(file));
            result = o;
        }
        else if (tool == QLatin1String("fit_notes_to_scale"))
        {
            const int track = trackArg();
            if (tracks[track].kind != chost::TrackKind::Instrument || tracks[track].file.empty())
                throw Fail{QStringLiteral("track %1 is not a MIDI track").arg(track)};
            const QString file = QString::fromStdString(tracks[track].file);
            chost::MidiClip clip;
            std::string error;
            if (!chost::readMidiFile(file.toStdString(), clip, error))
                throw Fail{QString::fromStdString(error)};
            if (clip.drums)
                throw Fail{QStringLiteral("track %1 is drums, which have no scale").arg(track)};
            chost::Scale scale;
            QString from;
            if (has("scale"))
            {
                scale = chost::parseScale(args.value(QStringLiteral("scale")).toString().toStdString());
                if (!scale.valid())
                    throw Fail{QStringLiteral("not a scale: %1").arg(args.value(QStringLiteral("scale")).toString())};
                from = QStringLiteral("given");
            }
            else if (chost::parseKey(session.key()).valid)
            {
                scale = chost::scaleOfKey(chost::parseKey(session.key()));
                from = QStringLiteral("the song's key");
            }
            else
            {
                float confidence = 0.0f;
                scale = chost::scaleOfKey(chost::guessKey(clip.notes, &confidence));
                if (!scale.valid() || confidence < 0.6f)
                    throw Fail{QStringLiteral("the song's key is not known and the notes don't make it clear: give scale")};
                from = QStringLiteral("guessed from the notes");
            }
            const int moved = chost::fitToScale(clip.notes, scale);
            QString written = file;
            if (moved > 0)
            {
                written = PianoRollDialog::editPath(file);
                if (!chost::writeMidiFile(written.toStdString(), clip, error))
                    throw Fail{QString::fromStdString(error)};
                if (w.fPianoRoll && w.fPianoRoll->midiPath() == file)
                    delete w.fPianoRoll; // it shows the notes as they were
                w.midiFileEdited(file, written, QStringLiteral("修正到调内"));
                check();
            }
            QJsonObject o = okTrack(track);
            o.insert(QStringLiteral("scale"), QString::fromStdString(chost::scaleName(scale)));
            o.insert(QStringLiteral("scale_from"), from);
            o.insert(QStringLiteral("moved"), moved);
            o.insert(QStringLiteral("notes"), static_cast<int>(clip.notes.size()));
            o.insert(QStringLiteral("file"), path(written));
            result = o;
        }
        else if (tool == QLatin1String("export_score"))
        {
            const QString file = args.value(QStringLiteral("path")).toString();
            if (file.isEmpty() || !QFileInfo(file).isAbsolute())
                throw Fail{QStringLiteral("path: an absolute .pdf or .musicxml path")};
            const std::vector<int> available = w.scoreTracks();
            std::vector<int> chosen;
            if (has("tracks"))
            {
                for (const QJsonValue& v : args.value(QStringLiteral("tracks")).toArray())
                {
                    const int t = v.toInt(-1);
                    if (std::find(available.begin(), available.end(), t) == available.end())
                        throw Fail{QStringLiteral("track %1 is not a MIDI track with notes (drum tracks can't be written as a score yet)").arg(t)};
                    chosen.push_back(t);
                }
            }
            else
            {
                chosen = available;
            }
            if (chosen.empty())
                throw Fail{QStringLiteral("there are no MIDI tracks to write: ai_transcribe or add_chord_track make some")};
            std::vector<ScoreDialog::Track> list;
            for (int t : chosen)
                list.push_back({QString::fromStdString(tracks[t].name), t, QString::fromStdString(tracks[t].file)});
            chost::ScoreOptions options;
            options.title = args.value(QStringLiteral("title")).toString(w.fSessionPath.isEmpty() ? QString() : w.projectName()).toStdString();
            options.tempo = session.tempo();
            options.grid = args.value(QStringLiteral("shortest")).toString() == QLatin1String("eighth") ? 2 : 1;
            options.key = chost::parseKey(args.value(QStringLiteral("key")).toString(QString::fromStdString(session.key())).toStdString());
            const QString staffText = args.value(QStringLiteral("staff")).toString();
            const chost::StaffMode staff = staffText == QLatin1String("treble") ? chost::StaffMode::Treble
                                           : staffText == QLatin1String("bass")  ? chost::StaffMode::Bass
                                           : staffText == QLatin1String("grand") ? chost::StaffMode::Grand
                                                                                 : chost::StaffMode::Auto;
            std::string error;
            const chost::Score score = ScoreDialog::build(list, options, staff, error);
            if (score.parts.empty())
                throw Fail{QString::fromStdString(error)};
            QString writeError;
            int pages = 0;
            if (!ScoreDialog::write(score, file, writeError, &pages))
                throw Fail{writeError};
            QJsonArray parts;
            for (const chost::ScorePart& part : score.parts)
                parts.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(part.name)},
                                         {QStringLiteral("staff"), part.staves.size() > 1 ? QStringLiteral("grand")
                                                                   : part.staves[0].clef == chost::Clef::Treble ? QStringLiteral("treble")
                                                                                                                : QStringLiteral("bass")}});
            QJsonObject o{{QStringLiteral("file"), path(file)}, {QStringLiteral("key"), QString::fromStdString(chost::keyName(score.key))},
                          {QStringLiteral("bars"), score.measures}, {QStringLiteral("parts"), parts}};
            if (pages > 0)
                o.insert(QStringLiteral("pages"), pages);
            result = o;
        }
        else if (tool == QLatin1String("edit_clip"))
        {
            const int track = trackArg();
            if (tracks[track].file.empty())
                throw Fail{QStringLiteral("track %1 has no clip").arg(track)};
            bool ok = false;
            chost::ClipEdit edit;
            edit.kind = chost::parseClipEditKind(args.value(QStringLiteral("action")).toString().toStdString(), ok);
            if (!ok)
                throw Fail{QStringLiteral("action: move, silence, keep, fade_in, fade_out, gain or normalize")};
            const bool midi = tracks[track].kind == chost::TrackKind::Instrument;
            if (midi && (edit.kind == chost::ClipEditKind::FadeIn || edit.kind == chost::ClipEditKind::FadeOut ||
                         edit.kind == chost::ClipEditKind::Normalize))
                throw Fail{QStringLiteral("fades and normalize are for audio tracks; on MIDI use gain (velocities)")};
            edit.start = std::max(0.0, args.value(QStringLiteral("start")).toDouble(0.0));
            edit.end = has("end") ? args.value(QStringLiteral("end")).toDouble() : -1.0;
            if (edit.end >= 0.0 && edit.end <= edit.start)
                throw Fail{QStringLiteral("end must be after start")};
            QString what;
            switch (edit.kind)
            {
            case chost::ClipEditKind::Move:
                edit.seconds = has("beats") ? args.value(QStringLiteral("beats")).toDouble() * 60.0 / session.tempo()
                                            : args.value(QStringLiteral("seconds")).toDouble();
                if (edit.seconds == 0.0)
                    throw Fail{QStringLiteral("move: give seconds or beats (not 0)")};
                what = QStringLiteral("移动片段");
                break;
            case chost::ClipEditKind::Silence: what = midi ? QStringLiteral("删除选区里的音符") : QStringLiteral("静音选区"); break;
            case chost::ClipEditKind::Keep: what = QStringLiteral("只保留选区"); break;
            case chost::ClipEditKind::FadeIn: what = QStringLiteral("选区淡入"); break;
            case chost::ClipEditKind::FadeOut: what = QStringLiteral("选区淡出"); break;
            case chost::ClipEditKind::Gain:
                if (!has("db"))
                    throw Fail{QStringLiteral("gain: give db")};
                edit.db = args.value(QStringLiteral("db")).toDouble();
                what = midi ? QStringLiteral("片段力度") : QStringLiteral("片段音量");
                break;
            case chost::ClipEditKind::Normalize:
                edit.db = args.value(QStringLiteral("db")).toDouble(-1.0);
                what = QStringLiteral("标准化");
                break;
            }
            const bool done = w.editClip(track, edit, what);
            check();
            if (!done)
                throw Fail{QStringLiteral("the clip was not edited")};
            QJsonObject o = okTrack(track);
            o.insert(QStringLiteral("file"), path(QString::fromStdString(tracks[track].file)));
            if (w.statusBar()->currentMessage().contains(QStringLiteral("削波")))
                o.insert(QStringLiteral("warning"), QStringLiteral("the clip now peaks above 0 dB and will clip: lower its gain"));
            result = o;
        }
        else if (tool == QLatin1String("add_marker"))
        {
            double seconds = 0.0;
            if (has("bar"))
                seconds = (args.value(QStringLiteral("bar")).toDouble(1.0) - 1.0) * 240.0 / session.tempo();
            else if (has("seconds"))
                seconds = args.value(QStringLiteral("seconds")).toDouble();
            else
                throw Fail{QStringLiteral("give seconds or bar")};
            const int index = w.addMarker(seconds, args.value(QStringLiteral("name")).toString().trimmed());
            result = QJsonObject{{QStringLiteral("index"), index}, {QStringLiteral("markers"), markersJson()}};
        }
        else if (tool == QLatin1String("edit_marker"))
        {
            std::vector<chost::Marker> markers = session.markers();
            const int index = args.value(QStringLiteral("index")).toInt(-1);
            if (index < 0 || index >= static_cast<int>(markers.size()))
                throw Fail{QStringLiteral("there is no marker %1: the project has %2").arg(index).arg(markers.size())};
            chost::Marker& m = markers[static_cast<size_t>(index)];
            QString what;
            if (args.value(QStringLiteral("remove")).toBool())
            {
                markers.erase(markers.begin() + index);
                what = QStringLiteral("删除标记");
            }
            else
            {
                if (has("name"))
                {
                    const QString name = args.value(QStringLiteral("name")).toString().trimmed();
                    if (name.isEmpty())
                        throw Fail{QStringLiteral("name is empty")};
                    m.name = name.toStdString();
                    what = QStringLiteral("标记改名");
                }
                if (has("seconds"))
                {
                    m.seconds = std::max(0.0, args.value(QStringLiteral("seconds")).toDouble());
                    what = QStringLiteral("移动标记");
                }
                if (what.isEmpty())
                    throw Fail{QStringLiteral("give name, seconds or remove: true")};
            }
            session.setMarkers(markers);
            w.refreshMarkers();
            w.fHistory->record(what);
            result = QJsonObject{{QStringLiteral("markers"), markersJson()}};
        }
        else if (tool == QLatin1String("add_bus"))
        {
            const QString kindText = args.value(QStringLiteral("kind")).toString();
            const MainWindow::BusKind kind = kindText == QLatin1String("reverb") ? MainWindow::BusKind::Reverb
                                             : kindText == QLatin1String("delay") ? MainWindow::BusKind::Delay
                                                                                  : MainWindow::BusKind::Group;
            if (kindText != QLatin1String("group") && kindText != QLatin1String("reverb") && kindText != QLatin1String("delay"))
                throw Fail{QStringLiteral("kind: group, reverb or delay")};
            std::vector<int> members;
            for (const QJsonValue& v : args.value(QStringLiteral("tracks")).toArray())
            {
                const int t = v.toInt(-1);
                if (t < 0 || t >= static_cast<int>(tracks.size()))
                    throw Fail{QStringLiteral("there is no track %1").arg(v.toInt(-1))};
                members.push_back(t);
            }
            const int bus = w.addBus(kind, members, args.value(QStringLiteral("name")).toString().trimmed());
            check();
            if (bus < 0)
                throw Fail{QStringLiteral("the bus was not added")};
            QJsonObject o = okTrack(bus);
            QJsonArray inserts;
            for (uint id : tracks[bus].inserts)
                inserts.append(pluginJson(id));
            o.insert(QStringLiteral("inserts"), inserts);
            if (kind != MainWindow::BusKind::Group && tracks[bus].inserts.empty())
                o.insert(QStringLiteral("warning"), QStringLiteral("no %1 plugin was found: add one with add_insert").arg(kindText));
            o.insert(QStringLiteral("note"), QStringLiteral("track indices changed: read get_project again"));
            result = o;
        }
        else if (tool == QLatin1String("set_output"))
        {
            const int track = trackArg();
            const int bus = args.value(QStringLiteral("bus")).toInt(-1);
            if (bus >= 0 && (bus >= static_cast<int>(tracks.size()) || tracks[bus].kind != chost::TrackKind::Bus))
                throw Fail{QStringLiteral("track %1 is not a bus: add_bus makes one").arg(bus)};
            const chost::Uid uid = tracks[track].uid;
            const bool done = w.setTrackOutput(track, bus);
            check();
            if (!done)
                throw Fail{QStringLiteral("the output was not changed")};
            QJsonObject o = okTrack(session.trackOf(uid));
            o.insert(QStringLiteral("note"), QStringLiteral("a track put in a group moves under it: read get_project again"));
            result = o;
        }
        else if (tool == QLatin1String("set_send"))
        {
            const int track = trackArg();
            const int bus = args.value(QStringLiteral("bus")).toInt(-1);
            if (bus < 0 || bus >= static_cast<int>(tracks.size()) || tracks[bus].kind != chost::TrackKind::Bus)
                throw Fail{QStringLiteral("track %1 is not a bus: add_bus makes one").arg(bus)};
            const bool remove = args.value(QStringLiteral("remove")).toBool();
            const bool exists = std::any_of(tracks[track].sends.begin(), tracks[track].sends.end(),
                                            [&](const chost::Send& x) { return x.bus == tracks[bus].uid; });
            if (!remove && !exists && tracks[track].sends.size() >= static_cast<size_t>(ChannelStrip::kSendSlots))
                throw Fail{QStringLiteral("track %1 already has %2 sends").arg(track).arg(ChannelStrip::kSendSlots)};
            if (remove && !exists)
                throw Fail{QStringLiteral("track %1 sends nothing to %2").arg(track).arg(bus)};
            const float level = remove ? -1.0f : fromDb(static_cast<float>(args.value(QStringLiteral("level_db")).toDouble(-10.0)));
            const bool done = w.setTrackSend(track, bus, level);
            check();
            if (!done)
                throw Fail{QStringLiteral("the send was not changed")};
            QJsonObject o = okTrack(track);
            if (!remove)
                o.insert(QStringLiteral("level_db"), std::round(toDb(level) * 10.0) / 10.0);
            result = o;
        }
        else if (tool == QLatin1String("set_metronome"))
        {
            if (has("volume_db"))
                w.setMetronomeVolume(std::min(0.0, args.value(QStringLiteral("volume_db")).toDouble()));
            const bool on = args.value(QStringLiteral("on")).toBool();
            w.setMetronome(on);
            if (on && !session.clickOn())
                throw Fail{QStringLiteral("the metronome didn't start: %1").arg(QString::fromStdString(session.lastError()))};
            result = QJsonObject{{QStringLiteral("metronome"), session.clickOn()}};
        }
        else if (tool == QLatin1String("get_parameters"))
        {
            const int track = trackArg();
            const uint id = pluginArg(track);
            QJsonObject o = pluginJson(id);
            o.insert(QStringLiteral("parameters"), parametersJson(id, args.value(QStringLiteral("query")).toString()));
            result = o;
        }
        else if (tool == QLatin1String("set_parameters"))
        {
            const int track = trackArg();
            const uint id = pluginArg(track);
            const QJsonObject values = args.value(QStringLiteral("values")).toObject();
            if (values.isEmpty())
                throw Fail{QStringLiteral("values: parameter name or index to value")};
            const std::vector<chost::ParamInfo> params = host.parameters(id);
            QJsonObject set;
            QStringList unknown;
            for (auto it = values.begin(); it != values.end(); ++it)
            {
                const int index = host.findParameter(id, it.key().toStdString());
                auto p = std::find_if(params.begin(), params.end(), [index](const chost::ParamInfo& info) { return static_cast<int>(info.index) == index; });
                if (index < 0 || p == params.end() || p->isOutput)
                {
                    unknown << it.key();
                    continue;
                }
                const float value = std::clamp(static_cast<float>(it.value().toDouble()), p->min, p->max);
                host.setParameter(id, p->index, value);
                if (auto editor = w.fEditors.find(id); editor != w.fEditors.end() && editor->second)
                    editor->second->parameterChanged(p->index, value);
                set.insert(QString::fromStdString(p->name), host.parameterValue(id, p->index));
            }
            if (!unknown.isEmpty() && set.isEmpty())
                throw Fail{QStringLiteral("no such parameter: %1 (get_parameters lists them)").arg(unknown.join(QStringLiteral(", ")))};
            QJsonObject o{{QStringLiteral("set"), set}};
            if (!unknown.isEmpty())
                o.insert(QStringLiteral("not_found"), QJsonArray::fromStringList(unknown));
            result = o;
        }
        else if (tool == QLatin1String("show_plugin"))
        {
            const int track = trackArg();
            const uint id = pluginArg(track);
            const bool show = args.value(QStringLiteral("show")).toBool(true);
            if (!show)
            {
                if (host.slot(id).hasCustomUi)
                    host.showCustomUi(id, false);
                if (auto editor = w.fEditors.find(id); editor != w.fEditors.end() && editor->second)
                    editor->second->close();
            }
            else
            {
                w.selectTrack(track);
                if (args.value(QStringLiteral("instrument")).toBool())
                    w.editInstrument();
                else
                    w.editInsert(insertArg(track));
            }
            result = pluginJson(id);
        }
        else if (tool == QLatin1String("transport"))
        {
            const QString action = args.value(QStringLiteral("action")).toString();
            if (action == QLatin1String("play"))
                w.play();
            else if (action == QLatin1String("pause"))
                host.pause();
            else if (action == QLatin1String("stop"))
                w.stop();
            else if (action == QLatin1String("seek"))
                w.seek(args.value(QStringLiteral("seconds")).toDouble());
            else
                throw Fail{QStringLiteral("action: play, pause, stop or seek")};
            const double rate = host.sampleRate();
            result = QJsonObject{{QStringLiteral("playing"), action == QLatin1String("play") || (action == QLatin1String("seek") && host.isPlaying())},
                                 {QStringLiteral("position_seconds"), action == QLatin1String("seek") ? args.value(QStringLiteral("seconds")).toDouble()
                                                                      : rate > 0.0 ? host.transportFrame() / rate : 0.0}};
        }
        else if (tool == QLatin1String("get_levels"))
        {
            auto peak = [&](uint id) {
                const auto [l, r] = host.outputPeaks(id);
                return toDb(std::max(l, r));
            };
            QJsonArray list;
            for (size_t i = 0; i < tracks.size(); ++i)
                list.append(QJsonObject{{QStringLiteral("track"), static_cast<int>(i)}, {QStringLiteral("name"), QString::fromStdString(tracks[i].name)},
                                        {QStringLiteral("peak_db"), peak(tracks[i].fader)}});
            result = QJsonObject{{QStringLiteral("playing"), host.isPlaying()}, {QStringLiteral("master_peak_db"), session.isReady() ? peak(session.master()) : -120.0},
                                 {QStringLiteral("tracks"), list}};
        }
        else if (tool == QLatin1String("undo") || tool == QLatin1String("redo"))
        {
            QUndoStack* stack = w.fHistory->stack();
            const bool undo = tool == QLatin1String("undo");
            QJsonArray done;
            for (int i = 0; i < std::max(1, args.value(QStringLiteral("steps")).toInt(1)); ++i)
            {
                if (undo ? !stack->canUndo() : !stack->canRedo())
                    break;
                done.append(undo ? stack->undoText() : stack->redoText());
                undo ? stack->undo() : stack->redo();
            }
            if (done.isEmpty())
                throw Fail{undo ? QStringLiteral("nothing to undo") : QStringLiteral("nothing to redo")};
            result = QJsonObject{{undo ? QStringLiteral("undone") : QStringLiteral("redone"), done}};
        }
        else if (tool == QLatin1String("get_history"))
        {
            QUndoStack* stack = w.fHistory->stack();
            QJsonArray steps;
            for (int i = 0; i < stack->count(); ++i)
                steps.append(stack->text(i));
            result = QJsonObject{{QStringLiteral("steps"), steps}, {QStringLiteral("done"), stack->index()},
                                 {QStringLiteral("note"), QStringLiteral("the first `done` steps are in effect; undo goes back from there")}};
        }
        else if (tool == QLatin1String("ai_analyze"))
        {
            if (w.fAnalysis->isRunning())
            {
                // chost listens to the first song of a project by itself
                if (std::none_of(fJobs.begin(), fJobs.end(), [](const Job& job) { return job.kind == QLatin1String("analyze") && !job.done; }))
                    startJob(QStringLiteral("analyze"), {}, nullptr);
                result = QJsonObject{{QStringLiteral("started"), QStringLiteral("analyze")},
                                     {QStringLiteral("note"), QStringLiteral("an analysis was already running; wait_for_jobs for its result")}};
                reply(true, result, {});
                return;
            }
            QString what;
            if (has("track"))
            {
                const int track = trackArg();
                if (tracks[track].kind != chost::TrackKind::Audio || tracks[track].file.empty())
                    throw Fail{QStringLiteral("track %1 has no audio to listen to").arg(track)};
                w.selectTrack(track);
                what = QString::fromStdString(tracks[track].name);
            }
            w.analyzeSong(false);
            check();
            startJob(QStringLiteral("analyze"), what, nullptr);
            result = QJsonObject{{QStringLiteral("started"), QStringLiteral("analyze")}};
        }
        else if (tool == QLatin1String("ai_separate_stems") || tool == QLatin1String("ai_tune"))
        {
            const bool stems = tool == QLatin1String("ai_separate_stems");
            const int track = trackArg();
            if (tracks[track].kind != chost::TrackKind::Audio || tracks[track].file.empty())
                throw Fail{QStringLiteral("track %1 has no audio").arg(track)};
            w.selectTrack(track);
            if (stems)
            {
                freshDialog(w.fStemDialog, QStringLiteral("a stem separation"));
                w.separateStems(true);
            }
            else
            {
                freshDialog(w.fTuneDialog, QStringLiteral("pitch correction"));
                w.tune(true);
            }
            check();
            startJob(stems ? QStringLiteral("stems") : QStringLiteral("tune"), QString::fromStdString(tracks[track].name),
                     stems ? static_cast<QWidget*>(w.fStemDialog) : static_cast<QWidget*>(w.fTuneDialog));
            result = QJsonObject{{QStringLiteral("started"), stems ? QStringLiteral("stems") : QStringLiteral("tune")}};
        }
        else if (tool == QLatin1String("ai_transcribe"))
        {
            const bool all = !has("track");
            QString what = QStringLiteral("every audio track");
            if (!all)
            {
                const int track = trackArg();
                if (tracks[track].kind != chost::TrackKind::Audio || tracks[track].file.empty())
                    throw Fail{QStringLiteral("track %1 has no audio").arg(track)};
                w.selectTrack(track);
                what = QString::fromStdString(tracks[track].name);
            }
            freshDialog(w.fTranscribeDialog, QStringLiteral("audio to MIDI"));
            w.transcribe(all, true);
            check();
            startJob(QStringLiteral("transcribe"), what, w.fTranscribeDialog);
            result = QJsonObject{{QStringLiteral("started"), QStringLiteral("transcribe")}};
        }
        else if (tool == QLatin1String("ai_arrange"))
        {
            QString what = args.value(QStringLiteral("chords")).toString();
            if (has("track"))
            {
                const int track = trackArg();
                w.selectTrack(track);
                if (what.isEmpty())
                    what = QString::fromStdString(tracks[track].name);
            }
            freshDialog(w.fArrangeDialog, QStringLiteral("an arrangement"));
            w.arrange(true, args.value(QStringLiteral("chords")).toString());
            check();
            startJob(QStringLiteral("arrange"), what, w.fArrangeDialog);
            result = QJsonObject{{QStringLiteral("started"), QStringLiteral("arrange")}};
        }
        else if (tool == QLatin1String("ai_master") || tool == QLatin1String("export_mix"))
        {
            const bool master = tool == QLatin1String("ai_master");
            if (master)
            {
                freshDialog(w.fMasterDialog, QStringLiteral("mastering"));
                w.masterMix(true);
            }
            else
            {
                freshDialog(w.fExportDialog, QStringLiteral("an export"));
                w.exportMix(true);
            }
            check();
            QWidget* dialog = master ? static_cast<QWidget*>(w.fMasterDialog) : static_cast<QWidget*>(w.fExportDialog);
            if (!dialog)
                throw Fail{QStringLiteral("it could not start")};
            const int id = startJob(master ? QStringLiteral("master") : QStringLiteral("export"), w.projectName(), dialog);
            auto made = [this, id](bool ok, const QString& file) {
                if (ok)
                    setJobFile(id, file);
            };
            if (master)
                connect(w.fMasterDialog, &MasterDialog::mastered, this, made);
            else
                connect(w.fExportDialog, &ExportDialog::exported, this, made);
            result = QJsonObject{{QStringLiteral("started"), master ? QStringLiteral("master") : QStringLiteral("export")}};
        }
        else if (tool == QLatin1String("get_jobs"))
        {
            pollJobs();
            result = QJsonObject{{QStringLiteral("jobs"), jobsJson()}};
        }
        else if (tool == QLatin1String("wait_for_jobs"))
        {
            pollJobs();
            if (!anyJobRunning())
            {
                result = QJsonObject{{QStringLiteral("jobs"), jobsJson()}};
            }
            else
            {
                const double seconds = std::clamp(args.value(QStringLiteral("timeout_seconds")).toDouble(50.0), 1.0, 110.0);
                fWaits.push_back({QDateTime::currentDateTime().addMSecs(static_cast<qint64>(seconds * 1000.0)), std::move(reply)});
                fPoll.start();
                return; // pollJobs answers
            }
        }
        else
        {
            throw Fail{QStringLiteral("chost has no tool \"%1\"").arg(tool)};
        }
        check();
        reply(true, result, {});
    }
    catch (const Fail& fail)
    {
        reply(false, {}, fail.why);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// AI jobs, followed through their dialogs

void Control::setJobFile(int id, const QString& file)
{
    for (Job& job : fJobs)
        if (job.id == id)
            job.file = QDir::toNativeSeparators(file);
}

int Control::startJob(const QString& kind, const QString& what, QWidget* dialog)
{
    // an earlier job of the same kind that is over makes way
    fJobs.erase(std::remove_if(fJobs.begin(), fJobs.end(), [&](const Job& job) { return job.kind == kind && job.done; }), fJobs.end());
    Job job;
    job.id = fNextJob++;
    job.kind = kind;
    job.what = what;
    job.dialog = dialog;
    job.started = QDateTime::currentDateTime();
    job.seenBusy = dialog == nullptr; // the analysis says itself when it's done
    fJobs.push_back(job);
    fPoll.start();
    return job.id;
}

bool Control::anyJobRunning() const
{
    return std::any_of(fJobs.begin(), fJobs.end(), [](const Job& job) { return !job.done; });
}

void Control::pollJobs()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (Job& job : fJobs)
    {
        if (job.done || !job.dialog)
        {
            if (!job.done && job.kind != QLatin1String("analyze"))
            {
                job.done = true;
                if (job.message.isEmpty())
                    job.message = QStringLiteral("its window was closed");
            }
            if (job.kind == QLatin1String("analyze") && !job.done && !fWindow.fAnalysis->isRunning() && job.started.msecsTo(now) > 2000)
                job.done = true;
            continue;
        }
        const bool busy = job.dialog->property("busy").toBool();
        if (auto* status = job.dialog->findChild<QLabel*>(QStringLiteral("status")))
            if (!status->text().isEmpty())
                job.message = QTextDocumentFragment::fromHtml(status->text()).toPlainText();
        if (auto* progress = job.dialog->findChild<QProgressBar*>(QStringLiteral("progress")))
            job.percent = std::max(0, progress->value());
        if (auto* log = job.dialog->findChild<QPlainTextEdit*>(QStringLiteral("log")))
            job.log = log->toPlainText().section(QLatin1Char('\n'), -4);
        job.seenBusy = job.seenBusy || busy;
        // a job that never got going (the AI components missing, say) is over once its dialog had time to start it
        if ((job.seenBusy && !busy) || (!job.seenBusy && job.started.msecsTo(now) > 6000))
            job.done = true;
    }

    const bool running = anyJobRunning();
    for (auto it = fWaits.begin(); it != fWaits.end();)
    {
        if (!running || now >= it->deadline)
        {
            Reply reply = std::move(it->reply);
            it = fWaits.erase(it);
            reply(true, QJsonObject{{QStringLiteral("jobs"), jobsJson()}, {QStringLiteral("still_running"), running}}, {});
        }
        else
            ++it;
    }
    if (!running && fWaits.empty())
        fPoll.stop();
}

QJsonArray Control::jobsJson() const
{
    QJsonArray list;
    for (const Job& job : fJobs)
    {
        QJsonObject o{{QStringLiteral("job"), job.kind}, {QStringLiteral("running"), !job.done}, {QStringLiteral("percent"), job.percent},
                      {QStringLiteral("message"), job.message},
                      {QStringLiteral("started"), job.started.toString(Qt::ISODate)}};
        if (!job.what.isEmpty())
            o.insert(QStringLiteral("of"), job.what);
        if (!job.log.isEmpty())
            o.insert(QStringLiteral("log"), job.log);
        if (!job.file.isEmpty())
            o.insert(QStringLiteral("file"), job.file);
        list.append(o);
    }
    return list;
}
