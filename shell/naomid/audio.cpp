#include "audio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <utility>


Q_LOGGING_CATEGORY(lcAudio, "naomid.audio")

namespace {

const char PULSE_SOCKET[] = "unix:/run/user/32011/pulse/native";
const char PULSE_COOKIE[] = "/run/naomid/pulse-cookie";   // копия ключа phablet (naomid.service)
const int STEP = 5;
const char DESK_SINK[] = "naomi.desk";   // выход «Монитор»: пустой (module-null-sink), его монитор снимает оболочка
const char PHONE_SINK[] = "sink.primary_output";   // динамик телефона (PulseAudio-droid; порт — динамик или трубка)
// громкость и «без звука» каждого выхода — здесь: PulseAudio их не помнит (module-device-restore у Ubuntu Touch не
// загружен), после перезагрузки или его перезапуска динамик был на 100 %
const char STATE_FILE[] = "/userdata/naomi/state/audio.json";

QProcessEnvironment pulseEnv()
{
    QProcessEnvironment env;
    env.insert("LC_ALL", "C");
    env.insert("PULSE_SERVER", PULSE_SOCKET);
    env.insert("PULSE_COOKIE", PULSE_COOKIE);
    env.insert("HOME", "/tmp");   // pactl не пишет в домашнюю папку root
    env.insert("PATH", "/usr/bin:/bin");
    return env;
}

// выход PulseAudio-droid по имени порта; Bluetooth — отдельный выход bluez_*
QString kindOf(const QString &sink, const QString &port)
{
    if (sink == QLatin1String(DESK_SINK))
        return "desk";
    if (sink.startsWith("bluez_") || port.contains("a2dp") || port.contains("sco"))
        return "bluetooth";
    if (port.contains("speaker"))
        return "speaker";
    if (port.contains("earpiece"))
        return "earpiece";
    if (port.contains("headset") || port.contains("headphone") || port.contains("line"))
        return "headphones";
    if (port.contains("usb"))
        return "usb";
    return "other";
}

} // namespace

Audio::Audio(QObject *parent)
    : BusObject("/org/naomi/Phone/Audio", "org.naomi.Phone.Audio", parent)
{
    set("Volume", 0);
    set("Muted", false);
    set("Output", "other");
    set("OutputName", QString());
    set("Headphones", QString());
    set("DeskVolume", -1);
    set("SoftVolume", false);
    loadSaved();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);   // ползунок — одна запись на секунду
    connect(&m_saveTimer, &QTimer::timeout, this, &Audio::save);

    m_retry.setSingleShot(true);
    m_retry.setInterval(5000);
    connect(&m_retry, &QTimer::timeout, this, &Audio::subscribe);
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(150);
    connect(&m_refresh, &QTimer::timeout, this, &Audio::refresh);
    m_apply.setSingleShot(true);
    m_apply.setInterval(40);
    connect(&m_apply, &QTimer::timeout, this, &Audio::applyVolume);

    m_routeWatch.setConnection(bus());
    m_routeWatch.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_routeWatch, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &who) {
        m_routeWatch.removeWatchedService(who);
        if (!m_routeWatch.watchedServices().isEmpty())
            return;
        qCInfo(lcAudio, "%s ушёл с шины — звук по своему правилу", qPrintable(who));
        m_route.clear();
        route(false);
    });

    m_events.setProcessEnvironment(pulseEnv());
    m_events.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_events, &QProcess::readyRead, this, [this] {
        // «Event 'change' on sink #1»; потоки (sink-input) не нужны — только выходы, сервер, карты
        static const QRegularExpression re("^Event '\\w+' on (sink|server|card) #");
        while (m_events.canReadLine())
            if (re.match(QString::fromUtf8(m_events.readLine())).hasMatch())
                m_refresh.start();
    });
    connect(&m_events, &QProcess::finished, this, [this] {
        qCInfo(lcAudio, "связи с PulseAudio нет, пробую снова через 5 с");
        m_retry.start();
    });
    subscribe();
    // служба перезапустилась при живом столе — его порядок придёт следом (PhoneClient видит службу заново); своим
    // правилом раньше времени звук ушёл бы с «Монитора» на динамик
    QTimer::singleShot(3000, this, [this] {
        if (std::exchange(m_routeReady, true))
            return;
        route(false);
    });
}

Audio::~Audio()
{
    m_events.disconnect(this);
    m_retry.stop();
    m_refresh.stop();
    m_apply.stop();
    if (m_events.state() != QProcess::NotRunning) {
        m_events.terminate();
        if (!m_events.waitForFinished(500)) {
            m_events.kill();
            m_events.waitForFinished(200);
        }
    }
}

void Audio::subscribe()
{
    m_known.clear();   // новое подключение — PulseAudio мог перезапуститься: громкости выходов вернуть заново
    m_events.start("pactl", {"subscribe"});
    // module-switch-on-connect (touch.pa) сам переводит звук на каждый новый выход — наушники перебивали выбор стола
    // («Динамики»), пока служба не вернёт. Куда звук — решает только служба (route)
    pactl({"unload-module", "module-switch-on-connect"}, [](const QByteArray &) {
        qCInfo(lcAudio, "module-switch-on-connect выгружен: куда звук, решает служба");
    });
    refresh();
}

void Audio::loadSaved()
{
    QFile f(QString::fromLatin1(STATE_FILE));
    if (f.open(QIODevice::ReadOnly))
        m_saved = QJsonDocument::fromJson(f.readAll()).object();
}

void Audio::save()
{
    QDir().mkpath(QFileInfo(QString::fromLatin1(STATE_FILE)).path());
    QSaveFile f(QString::fromLatin1(STATE_FILE));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_saved).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void Audio::pactl(const QStringList &args, std::function<void(const QByteArray &)> done)
{
    auto *p = new QProcess(this);
    p->setProcessEnvironment(pulseEnv());
    connect(p, &QProcess::finished, this, [p, done = std::move(done)](int code) {
        p->deleteLater();
        if (code != 0) {
            qCDebug(lcAudio, "pactl %s: код %d %s", qPrintable(p->arguments().join(' ')), code,
                    p->readAllStandardError().trimmed().constData());
            return;
        }
        if (done)
            done(p->readAllStandardOutput());
    });
    p->start("pactl", args);
}

void Audio::refresh()
{
    pactl({"-f", "json", "info"}, [this](const QByteArray &info) {
        const QString def = QJsonDocument::fromJson(info).object().value("default_sink_name").toString();
        pactl({"-f", "json", "list", "sinks"}, [this, def](const QByteArray &list) {
            QSet<QString> names;
            QHash<QString, QString> bluez;   // выходы наушников Bluetooth → имя устройства
            QString fresh;                   // только что подключившиеся
            int desk = -1;
            for (const QJsonValue &v : QJsonDocument::fromJson(list).array()) {
                const QJsonObject s = v.toObject();
                const QString name = s.value("name").toString();
                names << name;
                if (name.startsWith("bluez_")) {
                    // имя не латиницей pactl -f json отдаёт как «(null)»
                    const QString desc = s.value("description").toString();
                    bluez.insert(name, desc.isEmpty() || desc == QLatin1String("(null)") ? QStringLiteral("Bluetooth") : desc);
                    if (!m_sinks.contains(name))
                        fresh = name;
                }
                int percent = 0;
                const QJsonObject channels = s.value("volume").toObject();
                for (const QString &ch : channels.keys())
                    percent = qMax(percent, channels.value(ch).toObject().value("value_percent").toString()
                                                .chopped(1).toInt());
                if (name.startsWith(QLatin1String(DESK_SINK) + '.')) {   // «naomi.desk.2» — от гонки создания: прочь
                    qCInfo(lcAudio, "лишний выход %s — убираю", qPrintable(name));
                    pactl({"unload-module", QString::number(s.value("owner_module").toInt())});
                    continue;
                }
                SinkState &st = m_state[name];
                st.volume = percent;
                st.muted = s.value("mute").toBool();
                if (percent > 0)
                    st.audible = percent;
                st.soft = !s.value("flags").toArray().contains(QStringLiteral("HW_VOLUME_CTRL"));
                // Память громкости: выход впервые за это подключение (включили, PulseAudio перезапустился, наушники
                // подключились) — вернуть запомненное; иначе — запомнить, если поменялось
                if (!name.startsWith("sink.fake")) {
                    const QJsonObject saved = m_saved.value(name).toObject();
                    if (!m_known.contains(name)) {
                        m_known.insert(name);
                        if (!saved.isEmpty() && (saved.value("volume").toInt() != percent
                                                 || saved.value("muted").toBool() != st.muted)) {
                            qCInfo(lcAudio, "%s: громкость %d%%%s — как была", qPrintable(name),
                                   saved.value("volume").toInt(), saved.value("muted").toBool() ? ", без звука" : "");
                            pactl({"set-sink-volume", name, QString::number(saved.value("volume").toInt()) + "%"});
                            pactl({"set-sink-mute", name, saved.value("muted").toBool() ? "1" : "0"});
                            continue;   // числа — со следующего перечитывания, уже свои
                        }
                    }
                    if (saved.value("volume").toInt(-1) != percent || saved.value("muted").toBool() != st.muted) {
                        m_saved.insert(name, QJsonObject{{"volume", percent}, {"muted", st.muted}});
                        m_saveTimer.start();
                    }
                }
                if (name == QLatin1String(DESK_SINK))
                    desk = qMin(percent, 100);
                if (name != def)
                    continue;
                if (m_target < 0 && !m_apply.isActive())   // свою склейку не перебиваем старым значением
                    set("Volume", qMin(percent, 100));
                set("Muted", s.value("mute").toBool());
                set("SoftVolume", st.soft);
                const QString kind = kindOf(name, s.value("active_port").toString());
                set("Output", kind);
                set("OutputName", kind == "desk"        ? QStringLiteral("Монитор")
                                  : bluez.contains(name) ? bluez.value(name)
                                                         : s.value("description").toString());
            }
            set("DeskVolume", desk);
            // наушники: подключившиеся последними; пропали — другие, если есть
            if (!fresh.isEmpty())
                qCInfo(lcAudio, "наушники Bluetooth на связи: %s", qPrintable(fresh));
            if (!fresh.isEmpty() || !bluez.contains(m_btSink))
                m_btSink = !fresh.isEmpty() ? fresh : bluez.isEmpty() ? QString() : bluez.keys().constFirst();
            set("Headphones", bluez.value(m_btSink));
            m_default = def;
            m_sinks = names;
            // звук — куда велит порядок: наушники подключились или пропали, «Монитор» пропал (PulseAudio
            // перезапустился — создать заново, с прежней громкостью), выход сменил кто-то другой
            if (!m_deskGoing && m_routeReady)
                route(false);
        });
    });
}

void Audio::SetVolume(double percent)
{
    m_target = qBound(0, qRound(percent), 100);
    set("Volume", m_target);   // сразу — интерфейс не ждёт PulseAudio
    if (!m_apply.isActive())
        m_apply.start();
}

void Audio::applyVolume()
{
    if (m_target < 0)
        return;
    const int target = m_target;
    m_target = -1;
    pactl({"set-sink-volume", "@DEFAULT_SINK@", QString::number(target) + "%"});
}

void Audio::VolumeUp()
{
    if (muted())
        SetMuted(false);
    SetVolume((m_target >= 0 ? m_target : volume()) + STEP);
}

void Audio::VolumeDown()
{
    SetVolume((m_target >= 0 ? m_target : volume()) - STEP);
}

void Audio::SetMuted(bool on)
{
    set("Muted", on);
    pactl({"set-sink-mute", "@DEFAULT_SINK@", on ? "1" : "0"});
}

// Порядок выходов — от стола (оболочки); позвавший ушёл с шины — своё правило (m_routeWatch)
void Audio::SetRoute(const QStringList &order, double deskVolume, bool audible)
{
    const QString who = calledFromDBus() ? message().service() : QString();
    m_routeReady = true;
    m_route = order;
    m_deskVolume = deskVolume;
    if (!who.isEmpty()) {
        if (order.isEmpty())
            m_routeWatch.removeWatchedService(who);
        else if (!m_routeWatch.watchedServices().contains(who))
            m_routeWatch.addWatchedService(who);
    }
    qCInfo(lcAudio, "порядок выходов: %s", order.isEmpty() ? "своё правило" : qPrintable(order.join(" → ")));
    route(audible);
}

// первый доступный выход по порядку; без порядка — наушники, если на связи, иначе динамик
QString Audio::wantedSink() const
{
    const QStringList order = m_route.isEmpty() ? QStringList{"bluetooth", "phone"} : m_route;
    for (const QString &kind : order) {
        if (kind == QLatin1String("desk"))
            return DESK_SINK;
        if (kind == QLatin1String("bluetooth") && !m_btSink.isEmpty())
            return m_btSink;
        if (kind == QLatin1String("phone"))
            return PHONE_SINK;
    }
    return PHONE_SINK;
}

// Звук — на нужный выход (по последнему перечитыванию). Потоки, что идут «по умолчанию» (браузер, Android), PulseAudio
// 16 переводит сам вслед за выходом по умолчанию
void Audio::route(bool audible)
{
    const QString want = wantedSink();
    if (want == QLatin1String(DESK_SINK) && m_default != want) {
        goDesk(audible);
        return;
    }
    if (m_default == want) {
        if (audible)
            makeAudible(want, [] {});
        return;
    }
    if (!m_sinks.contains(want) && !m_sinks.isEmpty())
        return;   // выхода нет (динамик ещё не поднялся) — до следующего перечитывания
    auto go = [this, want] {
        pactl({"set-default-sink", want}, [want](const QByteArray &) {
            qCInfo(lcAudio, "звук — на %s", qPrintable(want));
        });
    };
    if (audible)
        makeAudible(want, go);
    else
        go();
}

// Выход «Монитор» создаётся раз (module-null-sink: 48 кГц стерео — как играет Chromium, без пересчёта) и живёт до
// перезапуска PulseAudio: «без звука» и громкость у него свои. Есть — сделать основным; нет (включили, PulseAudio
// перезапускался) — создать во float; остался прежний s16 — заново во float с той же громкостью
void Audio::goDesk(bool audible)
{
    if (m_deskGoing) {   // уже идёт (стол просит дважды подряд): второй выход не создавать — «naomi.desk.2»
        m_deskAudible |= audible;
        return;
    }
    m_deskGoing = true;
    m_deskAudible = audible;
    auto toDesk = [this] {
        if (wantedSink() != QLatin1String(DESK_SINK)) {   // пока переключались, порядок сменился
            m_deskGoing = false;
            route(std::exchange(m_deskAudible, false));
            return;
        }
        auto go = [this] {
            pactl({"set-default-sink", DESK_SINK}, [this](const QByteArray &) {
                m_deskGoing = false;
                qCInfo(lcAudio, "звук — на «Монитор»");
            });
        };
        if (std::exchange(m_deskAudible, false))   // переключил человек
            makeAudible(DESK_SINK, go);
        else
            go();
    };
    pactl({"-f", "json", "list", "sinks"}, [this, toDesk](const QByteArray &list) {
        // громкость нового «Монитора» — запомненная naomid, иначе та, что помнит оболочка, иначе 50 %
        const QJsonObject saved = m_saved.value(DESK_SINK).toObject();
        int start = !saved.isEmpty() ? saved.value("volume").toInt()
                                     : m_deskVolume >= 0 ? qBound(0, qRound(m_deskVolume), 100) : 50;
        auto create = [this, toDesk](int start) {
            // float: мешает и ставит громкость без потери разрядов (в s16 громкость 40 % оставляла от звука ~11 бит).
            // «Колонки» (device.form_factor=speaker, приоритет PulseAudio 9500 против 9000 у динамика телефона):
            // наушники пропали — PulseAudio сам, до службы, берёт «Монитор» (было — 170 мс звука из динамика телефона,
            // пока служба не вернёт на монитор; замер 20 ноября 2026)
            pactl({"load-module", "module-null-sink", QStringLiteral("sink_name=%1").arg(DESK_SINK), "rate=48000",
                   "channels=2", "format=float32le",
                   "sink_properties=\"device.description=Монитор device.class=sound device.form_factor=speaker\""},
                  [this, toDesk, start](const QByteArray &) {
                      qCInfo(lcAudio, "выход «Монитор» создан (float), громкость %d%%", start);
                      pactl({"set-sink-volume", DESK_SINK, QString::number(start) + "%"},
                            [toDesk](const QByteArray &) { toDesk(); });
                  });
        };
        for (const QJsonValue &v : QJsonDocument::fromJson(list).array()) {
            const QJsonObject o = v.toObject();
            if (o.value("name").toString() != QLatin1String(DESK_SINK))
                continue;
            if (!o.value("sample_specification").toString().startsWith("float32le")
                || o.value("properties").toObject().value("device.form_factor").toString() != QLatin1String("speaker")) {
                // прежний (s16 или не «колонки») — заново с той же громкостью
                start = m_state.value(DESK_SINK).volume;
                pactl({"unload-module", QString::number(o.value("owner_module").toInt())},
                      [create, start](const QByteArray &) { create(start); });
                return;
            }
            toDesk();
            return;
        }
        // выхода нет (включили, PulseAudio перезапускался): громкость — раньше, чем выход станет основным: иначе на
        // миг читается 100 %
        create(start);
    });
}

// Переключил человек — выход не должен молчать: громкость 0 — его прошлая ненулевая (или 50 %), «без звука» — снять
void Audio::makeAudible(const QString &sink, std::function<void()> then)
{
    const SinkState st = m_state.value(sink);
    if (st.volume == 0) {
        const int to = st.audible > 0 ? st.audible : 50;
        qCInfo(lcAudio, "%s молчал (0%%) — громкость %d%%", qPrintable(sink), to);
        pactl({"set-sink-volume", sink, QString::number(to) + "%"});
    }
    if (st.muted) {
        qCInfo(lcAudio, "%s был без звука — звук включён", qPrintable(sink));
        pactl({"set-sink-mute", sink, "0"});
    }
    then();
}
