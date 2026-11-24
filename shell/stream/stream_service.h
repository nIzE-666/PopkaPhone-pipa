// НаомиФон — трансляция экрана зрителям (docs/STREAM.md): naomiStream в QML. Зрителей сколько угодно, картинка у всех
// одна; на какие экраны выведен стол и какой из них главный — план экранов (docs/SCREENS.md).
// Выключатель (enabled, плитка «трансляция» в шторке, помнится): выключено — зрителей нет вовсе, постучавшийся
// ждёт на тихом пути (Switch 0), включили — подключается сам (docs/STREAM.md, «Выключатель»). Приставка на Pi
// смотрит стол — выключение сначала уводит панель и окна стола (её часы проявляются в те же обновления монитора), и
// только потом зритель отключается; её сеанс начинается без них — показать, когда она скажет (docs/PI.md, «Без дока»).
// Пока зрителя нет — ничего не делает (ни копий кадра, ни кодера). Пришёл — поток отрисовки после каждого кадра
// оболочки копирует его и пакует в NV12 прямо в буфер кодера (видеочип, без процессора), свой поток отдаёт кадр
// кодеру Venus, когда видеочип закончил (метка-fence), и шлёт сжатое зрителю; ввод зрителя — StreamInput.
// Связь — TCP только на 127.0.0.1 (зритель приходит по SSH): stream_protocol.h.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <memory>

#include "audio_capture.h"
#include "sound_history.h"

class QQuickWindow;
class HardKeyboard;
class StreamInput;
class StreamScreen;

class StreamService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)   // выключатель трансляции
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(QString viewer READ viewer NOTIFY changed)       // чей экран главный («pi», «mac»)
    Q_PROPERTY(QStringList viewers READ viewers NOTIFY changed)  // все, у кого идёт сеанс
    Q_PROPERTY(double fps READ fps NOTIFY changed)          // кадров отправлено за секунду
    Q_PROPERTY(double mbps READ mbps NOTIFY changed)
    Q_PROPERTY(double encodeMs READ encodeMs NOTIFY changed) // от кадра оболочки до сжатого, медиана за секунду
    Q_PROPERTY(bool desktop READ desktop NOTIFY changed)     // зритель смотрит рабочий стол, а не экран телефона
    // по чему сейчас идёт картинка: "cable" (usb0), "wifi" (wlan0) или "" — по ядру, не со слов зрителя
    Q_PROPERTY(QString route READ route NOTIFY changed)
    // зритель играет звук стола (Want audio — приставка на Pi): выход «Монитор» есть (docs/PI.md, «Звук на мониторе»)
    Q_PROPERTY(bool audio READ audio NOTIFY changed)
    // звук стола идёт на выход «Монитор» (naomid Output desk) — снимать его и слать зрителю; ставит стол (QML)
    Q_PROPERTY(bool sendAudio READ sendAudio WRITE setSendAudio NOTIFY changed)
    // Экран без дока у приставки: стол без панели и окон, одни обои (ставит служба, рисует стол — QML); уходят и
    // возвращаются анимацией, если awayAnimated, иначе — сразу (начало сеанса приставки)
    Q_PROPERTY(bool deskAway READ deskAway NOTIFY deskAwayChanged)
    Q_PROPERTY(bool awayAnimated READ awayAnimated NOTIFY awayAnimatedChanged)
    // настройки мыши стола (окно «Настройки» → «Мышь», хранит стол — QML): зрителю сообщением Mouse — курсор и колесо
    // считает приставка (docs/DESKTOP.md, «Настройки»); ключи — как в сообщении, значения — числа (булевы — 0 и 1)
    Q_PROPERTY(QVariantMap mouse READ mouse WRITE setMouse NOTIFY mouseChanged)
    // настройки трекпада стола («Настройки» → «Трекпад», хранит стол — QML): зрителю сообщением Pad — курсор, щелчки,
    // прокрутку и жесты из пальцев делает приставка (docs/PI.md, «Трекпад как на Mac»); ключи — как в сообщении.
    // padTouch — пальцы на трекпаде зрителя, пока раздел открыт (show в настройках; сообщение PadTouch): mode, buttons,
    // fingers — [{ x, y (доли трекпада), off (не считается) }]; padAction — смахивание просит действие стола
    Q_PROPERTY(QVariantMap trackpad READ trackpad WRITE setTrackpad NOTIFY trackpadChanged)
    Q_PROPERTY(QVariantMap padTouch READ padTouch NOTIFY padTouchChanged)
    // настройки экрана стола («Экран», «Без дока»): зрителю сообщением Screen — размер стола, частоту монитора и его
    // экран без дока ведёт приставка; monitor — её монитор (сообщение Monitor: name, port, w, h, hz, rates), пусто —
    // приставка ещё не подключалась с тех пор, как запущена оболочка
    Q_PROPERTY(QVariantMap screen READ screen WRITE setScreen NOTIFY screenChanged)
    Q_PROPERTY(QVariantMap monitor READ monitor NOTIFY monitorChanged)
    // на столе стоит прицел скриншота области (ставит стол — QML): Esc зрителя — screenshotKey(0), не программе
    Q_PROPERTY(bool shotSelecting READ shotSelecting WRITE setShotSelecting NOTIFY shotSelectingChanged)
    // раскладка клавиатуры стола (docs/DESKTOP.md, «Раскладка»): зрителя (сообщение Layout), а не сообщает он её (окно
    // Mac: раскладка — у самого Mac) или его нет — клавиатуры самого телефона (hard_keyboard.h); "EN", "RU" или "" —
    // нет ни той, ни другой
    Q_PROPERTY(QString layout READ layout NOTIFY layoutChanged)
    // Экраны (docs/SCREENS.md): все, что телефон знает, — на связи и нет. Запись — { id, name, kind ("monitor" /
    // "window" / "tablet" / "tv"), w, h, viewer (кто несёт: "pi", "mac"), route ("cable" / "wifi" / ""), online, on (стол
    // на него выведен), main (главный: форма стола — его), state: "desk" — показывает стол, "splash" — заставка без дока,
    // "offline" — не на связи, "standby" — не на связи, но включится сам, когда его поставят на «стол» или «главный»
    // (wake: телевизор, docs/TV.md), light — яркость экрана можно ставить (планшет), lightAuto, lightLevel — как стоит }.
    // Раздел «Экраны» в «Настройках» телефона и на столе
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    // обои стола — файл и номер версии (ставит стол — QML): экранам, что рисуют заставку без дока сами (окно Mac, планшет)
    Q_PROPERTY(QString wallpaper READ wallpaper WRITE setWallpaper NOTIFY wallpaperChanged)

public:
    explicit StreamService(QObject *parent = nullptr);
    ~StreamService() override;
    // источники кадра: экран телефона (окно оболочки) и рабочий стол — экран только для трансляции; зритель
    // выбирает (сообщение Mode), по умолчанию — экран телефона
    enum Source { Phone = 0, Desktop = 1 };
    void attach(QQuickWindow *window, StreamScreen *localDesktop = nullptr, int sceneRotation = 0);
    void setNativeRotation(int rotation);
    void attachDesktop(StreamScreen *desktop);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    // Рядом могут понадобиться декодеры видео: у Android есть окна, и он не спит. Видеокодек один —
    // кодер трансляции оставляет им место (скорость работы ниже наибольшей; docs/STREAM.md, «Кодек на всех»)
    void setDecodersNear(bool near);
    bool connected() const { return m_connected; }
    QString viewer() const { return m_viewer; }
    QStringList viewers() const { return m_viewers; }
    double fps() const { return m_fps; }
    double mbps() const { return m_mbps; }
    double encodeMs() const { return m_encodeMs; }
    bool desktop() const { return m_source == Desktop; }
    QString route() const { return m_route; }
    bool audio() const { return m_audio; }
    bool sendAudio() const { return m_sendAudio; }
    void setSendAudio(bool on);
    bool deskAway() const { return m_deskAway; }
    bool awayAnimated() const { return m_awayAnimated; }
    QVariantMap mouse() const { return m_mouse; }
    void setMouse(const QVariantMap &mouse);
    QVariantMap trackpad() const { return m_trackpad; }
    void setTrackpad(const QVariantMap &trackpad);
    QVariantMap padTouch() const { return m_padTouch; }
    QVariantMap screen() const { return m_screen; }
    void setScreen(const QVariantMap &screen);
    QVariantMap monitor() const { return m_monitor; }
    bool shotSelecting() const { return m_shotSelecting; }
    void setShotSelecting(bool on);
    QString layout() const;
    // сменить раскладку ("EN" / "RU"; щелчок по подписи на панели): держит зритель — просьба ему, layout сменится, когда
    // он ответит; иначе — клавиатуре телефона, сразу
    Q_INVOKABLE void setLayout(const QString &layout);
    // ввод с экранной клавиатуры стола (планшет) — окну клиента Wayland (браузер, Waydroid) и элементам без IM
    void injectKey(bool down, int qtKey, int modifiers, const QString &text);
    // «Тест» в «Настройках» → «Без дока»: стол уходит (как «закрыть сессию»), а приставка сразу делает с монитором то, что
    // без дела (сон или приглушение) — не ждать 10 минут. Только приставке (зритель с экраном без дока)
    Q_INVOKABLE void idleTest();
    // стол уходит, а монитор приставки без дела — сразу: dimLevel 0…100 — приглушить до этой доли, 255 — как в настройках
    void leaveIdle(int dimLevel);
    Q_INVOKABLE void awaySettled();   // стол: панель и окна ушли (анимация дошла до конца) — последний кадр
    QVariantList screens() const;
    // положение экрана в «Экранах»: 0 — стол на него не выведен (заставка), 1 — выведен, 2 — выведен, и он главный.
    // Убрали стол с последнего (на всех «нет») — стол выключается; главный ушёл — им становится следующий выведенный
    Q_INVOKABLE void setScreenPosition(const QString &id, int position);
    Q_INVOKABLE void forgetScreen(const QString &id);   // экран не на связи — убрать из списка
    // яркость экрана, у которого её можно ставить (запись экрана: light; планшет): automatic — экран ведёт её сам по
    // своему датчику света, иначе — level, 1…100 %
    Q_INVOKABLE void setScreenLight(const QString &id, bool automatic, int level);
    // как жест на самом экране (клавиша, щелчок, двойное касание): стол — сюда (docs/SCREENS.md, «Пробуждение»)
    Q_INVOKABLE void wakeScreen(const QString &id) { onWake(id); }
    QString wallpaper() const { return m_wallpaper; }
    void setWallpaper(const QString &file);
    // звук стола (монитор выхода «Монитор») за последние 85 мс — эквалайзеру окна звука; live — его снимают сейчас
    // (зритель играет звук стола), сигнал — changed
    std::shared_ptr<const SoundHistory> deskSound() const { return m_deskSound; }
    bool deskSoundLive() const { return m_capture.running(); }

signals:
    void changed();
    void activity();   // ввод зрителя мимо окна (клавиши): для гашения экрана
    void volumeKey(int step);   // клавиша громкости зрителя на столе: +1, −1, 0 — «без звука»
    // скриншот стола (docs/DESKTOP.md, «Скриншоты»): 3 — Shift+Alt+3, 4 — Shift+Alt+4, 0 — Esc при прицеле
    void screenshotKey(int key);
    void brightnessShown(int percent);   // клавиши яркости: монитор зрителя теперь на стольких % — плашка стола
    void shotSelectingChanged();
    void deskAwayChanged();
    void awayAnimatedChanged();
    void mouseChanged();
    void trackpadChanged();
    void padTouchChanged();
    void padAction(int action);   // смахивание трекпада зрителя: 1 — меню N (сообщение PadAction)
    void screenChanged();
    void monitorChanged();
    void layoutChanged();
    void screensChanged();
    void wallpaperChanged();

private:
    struct Shared;
    struct Gl;
    class Worker;
    void capture(int source);   // поток, где рисуется источник (телефон — поток отрисовки), после его кадра
    void releaseGl(Gl &g);      // там же, в его контексте
    // поток службы → поток интерфейса
    void onViewer(bool connected, const QString &name);
    void onViewers(const QString &main, const QStringList &names, bool wifi);   // чей экран главный; все, кто смотрит
    void onScreens(const QVariantList &online, const QString &main);            // экраны на связи
    void onWake(const QString &screen);
    // план экранов: стол включён, на какие экраны выведен, главный — помнится (shell.conf) и уходит потоку службы.
    // Стол уходит со всех экранов, что его показывали, — сначала анимация ухода (leave), потом план
    void applyPlan(bool enabled, QStringList on, QString main);
    void pushPlan();
    QStringList liveScreens(bool enabled, const QStringList &on) const;   // какие экраны показали бы стол
    void savePlan();
    void onSource(int source);
    void onRoute(int route);
    void onDesktopSize(QSize logical, double scale);
    void onDesktopFps(int fps, bool named);   // named — частоту назвал зритель (Want), а не по умолчанию
    void onTick(uint64_t value);
    void onAudioViewer(bool on);
    // зрители с экраном без дока есть (Want lock); fresh — стол до того никто не смотрел: он начинает без панели и окон
    void onLockViewer(bool on, bool fresh);
    void onMonitor(const QVariantMap &monitor);
    void onLayout(const QString &layout);   // "" — зритель ушёл
    void onPadTouch(const QVariantMap &touch);
    void checkCursor();   // форма курсора стола сменилась — зрителю (Cursor)
    void checkIdleExit(); // стол без дела дольше, чем в настройках «Без дока», — уходит
    void onReveal();              // зритель показал первый кадр (одни обои): стол — вернуть
    void onAwayShown();           // зритель показал последний кадр ухода: выключаться
    void setAway(bool away, bool animated);
    void finishLeave();
    void updateCapture();   // звук стола: снимать, пока зритель его играет и звук идёт на «Монитор»
    void onStats(double fps, double mbps, double encodeMs);
    void requestFrame();
    void holdGpu(bool on);   // видеочип не ниже 500 МГц (naomid HoldFast), пока зритель тактует стол

    QPointer<QQuickWindow> m_window;
    StreamScreen *m_desktop = nullptr;
    StreamScreen *m_localDesktop = nullptr;
    int m_source = Phone;
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<Gl> m_gl[2];
    std::unique_ptr<Worker> m_worker;
    StreamInput *m_input = nullptr;
    bool m_enabled = true;      // выключатель; из shell.conf (stream/enabled)
    // экраны: на какие выведен стол и главный (shell.conf, screens/on и screens/main); все известные — id → запись
    // (screens/known), порядок — как появлялись; на связи — со слов потока службы
    QStringList m_on;
    QString m_main, m_mainNow;  // выбранный главный; кто главный сейчас (выбранного может не быть на связи)
    QString m_last;             // с какого экрана стол сняли последним (на всех «нет» — стол выключился): его вернёт
                                // выключатель (shell.conf, screens/last)
    QStringList m_order;
    QVariantMap m_known;
    QVariantMap m_lights;       // экран → { auto, level } (shell.conf, screens/light)
    QVariantList m_online;
    bool m_adopt = false;       // выбора ещё не было (первый запуск): первый экран на связи — выведен и главный
    // что знает поток службы (pushPlan): по нему — уходит ли стол со всех экранов
    bool m_pushedEnabled = true;
    QStringList m_pushedOn;
    bool m_wifiViewer = false;  // основной путь хоть одного зрителя — Wi-Fi
    QStringList m_viewers;      // кто смотрит
    QTimer m_revealTimer;       // стол ждёт Reveal зрителя слишком долго — показать самим
    QString m_wallpaper;
    bool m_connected = false;
    QString m_viewer;
    QString m_route;
    double m_fps = 0, m_mbps = 0, m_encodeMs = 0;
    uint64_t m_tickValue = 0;   // такт зрителя, по которому сейчас рисуется стол (capture — в Echo)
    bool m_gpuFast = false;
    bool m_audio = false, m_sendAudio = false;
    AudioCapture m_capture;     // монитор выхода «Монитор» (naomi.desk) → зрителю (Audio) и в m_deskSound
    std::shared_ptr<SoundHistory> m_deskSound = std::make_shared<SoundHistory>(4096);
    QTimer m_gpuIdle;           // тактов нет 3 с — частоту видеочипа отпустить
    QTimer m_wifiIdle;          // тактов нет 10 с — радио Wi-Fi можно спать, если картинка идёт по кабелю
    bool m_moving = false;      // такты идут (или были меньше 10 с назад)
    bool m_wifiAwake = false;   // радио держим без сна (naomid, Wifi.HoldAwake)
    void holdWifi();
    // экран без дока (docs/PI.md, «Без дока»)
    bool m_lockViewer = false, m_deskAway = false, m_awayAnimated = false;
    bool m_leaving = false, m_finalAsked = false;   // выключили: стол уходит; последний кадр уже просили
    uint8_t m_awayMark = 0;     // метка следующего кадра стола (Away: фаза + 1) — его снимет capture
    QTimer m_leaveTimer;        // уход не дошёл (анимация, ответ зрителя) — выключиться всё равно
    QTimer m_idleExitTimer;     // выход из стола без дела (checkIdleExit)
    int m_idleExitEnv = 0;      // NAOMI_DESK_IDLE_EXIT — свой срок, с (проверки)
    int64_t m_idleFrom = 0;     // стол показан приставке с этого момента (0 — не показан)
    QVariantMap m_mouse, m_screen, m_monitor, m_trackpad, m_padTouch;
    QTimer m_cursorPoll;          // форма курсора — и без движения мыши (страница сменила свою)
    uint8_t m_cursor = 0;         // последняя, что ушла зрителю (stream::CursorShape)
    bool m_shotSelecting = false;
    QString m_layout;             // раскладка зрителя; "" — не сообщает или его нет
    HardKeyboard *m_keyboard = nullptr;   // клавиатура самого телефона
};
