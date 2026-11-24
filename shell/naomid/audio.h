// naomid — звук: громкость, «без звука» и куда идёт звук (динамик, наушники, Bluetooth) — у выхода
// PulseAudio по умолчанию. PulseAudio — в сеансе phablet; служба (root) говорит с ним через pactl
// (сокет /run/user/32011/pulse/native, копия ключа phablet в /run/naomid). Изменения — по
// `pactl subscribe` (один процесс, без событий спит), перечитывание — `pactl -f json`. Куда звук, решает служба
// одна (module-switch-on-connect PulseAudio выгружен): по порядку выходов от стола (SetRoute) — первый, что есть;
// без стола — наушники Bluetooth, если на связи, иначе динамик. Порядок держится сам: наушники подключились или
// пропали — звук сразу туда, куда велит порядок. Выход «Монитор» — звук рабочего стола на мониторе приставки:
// пустой выход PulseAudio naomi.desk, его звук оболочка шлёт приставке (docs/PI.md, «Звук на мониторе»).
#pragma once

#include "bus_object.h"

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

class Audio : public BusObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.Audio")
    Q_PROPERTY(int Volume READ volume)            // 0…100
    Q_PROPERTY(bool Muted READ muted)
    Q_PROPERTY(QString Output READ output)        // speaker, earpiece, headphones, bluetooth, usb, desk, other
    Q_PROPERTY(QString OutputName READ outputName)  // для Bluetooth — имя устройства, иначе описание выхода
    Q_PROPERTY(QString Headphones READ headphones)  // наушники Bluetooth на связи (их имя), где бы ни был звук; "" — нет
    Q_PROPERTY(int DeskVolume READ deskVolume)    // громкость выхода «Монитор», где бы ни был звук; −1 — его нет
    // громкость выхода по умолчанию — в самих отсчётах (PulseAudio), а не в драйвере: монитор выхода отдаёт звук уже
    // после неё (эквалайзеру — поправка)
    Q_PROPERTY(bool SoftVolume READ softVolume)

public:
    explicit Audio(QObject *parent = nullptr);
    ~Audio() override;

    int volume() const { return prop("Volume").toInt(); }
    bool muted() const { return prop("Muted").toBool(); }
    QString output() const { return prop("Output").toString(); }
    QString outputName() const { return prop("OutputName").toString(); }
    QString headphones() const { return prop("Headphones").toString(); }
    int deskVolume() const { return prop("DeskVolume").toInt(); }
    bool softVolume() const { return prop("SoftVolume").toBool(); }

public slots:
    // percent — дробное: числа из QML приходят как double. Частые вызовы (ползунок) склеиваются
    Q_SCRIPTABLE void SetVolume(double percent);
    Q_SCRIPTABLE void VolumeUp();                 // кнопки громкости: шаг 5, «вверх» снимает «без звука»
    Q_SCRIPTABLE void VolumeDown();
    Q_SCRIPTABLE void SetMuted(bool on);
    // Куда звук: order — выходы по порядку, звук — на первый, что есть: desk — «Монитор» (есть всегда: нет — служба его
    // создаёт), bluetooth — наушники (если на связи), phone — динамик телефона; пустой — своё правило (наушники, иначе
    // динамик). Стол: ["bluetooth", "desk"] — в наушники, а пропадут — на монитор. deskVolume — громкость «Монитора»,
    // если его приходится создавать заново (перезапуск PulseAudio, включение; −1 — 50 %): её помнит оболочка, живой
    // выход держит свою. audible — переключил человек: выход, куда ушёл звук, не должен молчать (громкость 0 или «без
    // звука» — его прошлая громкость или 50 %). Позвавший ушёл с шины (оболочка упала) — своё правило
    Q_SCRIPTABLE void SetRoute(const QStringList &order, double deskVolume, bool audible);

private:
    void subscribe();
    void refresh();
    void applyVolume();
    void pactl(const QStringList &args, std::function<void(const QByteArray &)> done = {});
    QString wantedSink() const;
    void route(bool audible);
    void goDesk(bool audible);
    void loadSaved();
    void save();
    void makeAudible(const QString &sink, std::function<void()> then);

    QProcess m_events;          // pactl subscribe
    QTimer m_retry;             // PulseAudio ещё не запущен (загрузка) — пробовать снова
    QTimer m_refresh;           // несколько событий подряд — одно перечитывание
    QTimer m_apply;             // склейка SetVolume
    int m_target = -1;          // громкость, которую надо поставить
    QSet<QString> m_sinks;      // выходы с прошлого перечитывания — заметить новый Bluetooth
    QString m_default;          // выход по умолчанию с прошлого перечитывания
    QString m_btSink;           // выход наушников Bluetooth (последний подключившийся); "" — нет
    QStringList m_route;        // порядок выходов от стола (SetRoute); пустой — своё правило
    double m_deskVolume = -1;   // громкость «Монитора», если создавать заново (от стола)
    QDBusServiceWatcher m_routeWatch;  // кто задал порядок (оболочка): ушёл с шины — своё правило
    bool m_routeReady = false;         // первые 3 с после запуска службы выход не трогать: стол пришлёт свой порядок
    bool m_deskGoing = false;          // переключение на «Монитор» идёт (создание выхода — не дважды)
    bool m_deskAudible = false;        // пока шло, попросили «не молчать»
    struct SinkState { int volume = 0; bool muted = false; int audible = 0; bool soft = false; };   // audible — последняя ненулевая
    QHash<QString, SinkState> m_state;   // выходы с прошлого перечитывания
    QJsonObject m_saved;                 // запомненные громкость и «без звука» выходов (STATE_FILE)
    QSet<QString> m_known;               // выходы, уже виденные за это подключение к PulseAudio
    QTimer m_saveTimer;
};
