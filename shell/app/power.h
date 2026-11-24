// НаомиФон — питание экрана: кнопка питания, гашение без касаний, AOD («экран всегда включён»), яркость,
// настройка сенсора касаний. Экран: включён, AOD (часы на чёрном, яркость ниже) или выключен.
// С AOD гашение — кнопкой или без касаний (screenTimeout) — уводит в AOD, а не выключает; из AOD будят
// касание экрана, касание датчика отпечатка и кнопка питания (docs/SHELL.md, «AOD»). AOD — та же включённая
// панель при низкой яркости; переход — один ход aodAmount на каждом кадре: QML двигает картинку, здесь — яркость.
// Вторая ступень (aodDeep, по умолчанию включена): 20 минут AOD без движения — та же картинка остаётся, а панель
// уходит в малое питание (87 мА против 127, docs/POWER.md); сдвинули телефон (датчик движения naomid, его просит
// QML по watchMotion и зовёт moved) — панель обратно в обычный режим, отсчёт заново; пробуждение — тоже. deep — панель
// в малом питании: QML меняет картинку до кадра, с которым панель туда уходит (обои проекта гаснут — AodScreen.qml).
// NAOMI_AOD_DEEP=секунд — другой срок, для проверок.
// Кнопки громкости тоже ловит здесь (до программ): volumeKey(+1/−1), при погашенном экране и в AOD — ничего.
// Кнопка питания на включённом экране (powerKey, функция Power button студии NaomiPhone): "lock" — погасить, как
// без касаний (в AOD, если он включён; по умолчанию), "aod" — в AOD всегда, "off" — выключить, без AOD, "none" —
// ничего; погашенный экран и AOD она будит всегда.
// Автояркость: освещённость (ambientLux — от датчика через naomid, её ставит QML) → яркость по кривой;
// ползунок в этом режиме не выключает автоматику, а сдвигает кривую (docs/SHELL.md, «Автояркость»).
#pragma once

#include "naomi_display.h"

#include <QEasingCurve>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>

#include <memory>

class BacklightWriter;
class QQuickWindow;
class QSocketNotifier;
class QWindow;

class Power : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool displayOn READ displayOn NOTIFY displayOnChanged)   // включён (не AOD и не выключен)
    Q_PROPERTY(bool dozing READ dozing NOTIFY dozingChanged)            // AOD на экране или переход к нему
    Q_PROPERTY(qreal aodAmount READ aodAmount NOTIFY aodAmountChanged)  // 0 — экран, 1 — AOD; между — переход
    Q_PROPERTY(bool aodSupported READ aodSupported CONSTANT)
    Q_PROPERTY(bool aod READ aod WRITE setAod NOTIFY aodChanged)        // настройка: гасить в AOD
    Q_PROPERTY(bool aodDeep READ aodDeep WRITE setAodDeep NOTIFY aodDeepChanged)   // настройка: вторая ступень
    Q_PROPERTY(bool watchMotion READ watchMotion NOTIFY watchMotionChanged)        // нужен датчик движения
    Q_PROPERTY(bool deep READ deep NOTIFY deepChanged)                              // AOD, панель в малом питании
    Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(int screenTimeout READ screenTimeout WRITE setScreenTimeout NOTIFY screenTimeoutChanged)
    Q_PROPERTY(bool autoBrightness READ autoBrightness WRITE setAutoBrightness NOTIFY autoBrightnessChanged)
    Q_PROPERTY(int ambientLux READ ambientLux WRITE setAmbientLux NOTIFY ambientLuxChanged)
    Q_PROPERTY(QString powerKey READ powerKey WRITE setPowerKey NOTIFY powerKeyChanged)   // lock | aod | off | none

public:
    Power();
    ~Power() override;
    void attach(QWindow *window);   // кнопки и касания окна оболочки — после загрузки QML
    void activity() { userActivity(); }   // ввод мимо окна (клавиши трансляции): гасить экран — заново

    enum class Screen { Off, On, Aod };

    bool displayOn() const { return m_screen == Screen::On; }   // включён для работы (и по пути в AOD)
    bool dozing() const { return m_screen == Screen::Aod || m_toAod; }
    qreal aodAmount() const { return m_aodAmount; }
    bool aodSupported() const { return m_aodSupported; }
    bool aod() const { return m_aod; }
    void setAod(bool on);
    bool aodDeep() const { return m_aodDeep; }
    void setAodDeep(bool on);
    // смотрят рабочий стол (трансляция): кнопки громкости работают и в AOD — звук стола (экран не будят)
    void setDesk(bool on) { m_desk = on; }
    bool watchMotion() const { return m_screen == Screen::Aod && m_aodDeep; }
    bool deep() const { return m_deep; }
    qreal brightness() const { return m_brightness; }     // 0…1, по восприятию; сейчас на экране
    void setBrightness(qreal value);                      // рукой (ползунок)
    int screenTimeout() const { return m_timeoutSec; }    // секунд без касаний до гашения (или AOD); 0 — никогда
    void setScreenTimeout(int seconds);
    bool autoBrightness() const { return m_auto; }
    void setAutoBrightness(bool on);
    int ambientLux() const { return m_lux; }              // люкс; −1 — датчик молчит
    void setAmbientLux(int lux);
    QString powerKey() const { return m_powerKey; }
    void setPowerKey(const QString &action);

    void setScreen(Screen screen);   // сразу, без перехода
    void wake();
    Q_INVOKABLE void sleep();         // погасить: в AOD (переходом), если он включён
    void sleepTo(bool aod);          // погасить: в AOD (переходом) или выключить
    Q_INVOKABLE void moved();        // телефон сдвинули (naomid): в AOD — отсчёт до малого питания заново

signals:
    void displayOnChanged();
    void dozingChanged();
    void aodAmountChanged();
    void aodChanged();
    void aodDeepChanged();
    void deepChanged();
    void watchMotionChanged();
    void brightnessChanged();
    void screenTimeoutChanged();
    void autoBrightnessChanged();
    void ambientLuxChanged();
    void powerKeyChanged();
    void volumeKey(int direction);   // +1 — громче, −1 — тише; при удержании повторяется

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool awake() const { return m_screen == Screen::On && !m_toAod; }
    void animateAod(qreal target, int fullMs);
    void aodFrame();
    void setAodAmount(qreal amount);
    void userActivity();
    void writeBacklight();
    void applyTouchSettings();
    void touchCommand(const char *command);
    void enterDeep();
    void leaveDeep();                       // AOD остаётся, панель — в обычный режим
    void readFingerIrq();
    void show(qreal value);                 // поставить яркость на экран (без запоминания)
    qreal autoValue() const;                // яркость по кривой для принятой освещённости
    void rampTo(qreal target, qreal perSecond);
    void rampStep();

    NaomiSetDisplayModeFn m_setMode = nullptr;
    NaomiFrameSnapshotFn m_frameSnapshot = nullptr;   // частота панели — шаг хода перехода
    Screen m_screen = Screen::On;
    const bool m_aodSupported;
    const bool m_noIdleSleep;
    bool m_aod = false;
    bool m_wakeTouch = false;               // касание, разбудившее из AOD: до отпускания — не интерфейсу
    bool m_toAod = false;                   // идём в AOD или в нём
    bool m_aodDeep = true;
    QString m_powerKey = "lock";
    bool m_desk = false;
    bool m_deep = false;                    // AOD, панель в малом питании
    int m_deepSec = 20 * 60;                // сколько секунд AOD без движения до малого питания
    QTimer m_deepTimer;
    qreal m_aodAmount = 0;
    QPointer<QQuickWindow> m_window;        // ход перехода — по его кадрам
    QEasingCurve m_aodCurve{QEasingCurve::BezierSpline};
    bool m_aodRunning = false;
    qreal m_aodFrom = 0, m_aodTo = 0;
    int m_aodMs = 1;
    int m_aodWarmup = 0;                    // кадров хода до начала отсчёта
    QElapsedTimer m_aodClock;               // со второго кадра хода
    qreal m_awake = 0.6;                    // яркость до AOD — к ней ход назад
    qreal m_aodT = 0;                       // мс хода: по кадру панели за кадр
    int m_fingerFd = -1;                    // прерывание датчика отпечатка (kFingerIrq)
    QSocketNotifier *m_finger = nullptr;    // включён только в AOD
    qreal m_brightness = 0.6;
    qreal m_manual = 0.6;                   // выбор рукой без автояркости
    bool m_auto = true;
    qreal m_adjust = 0;                     // сдвиг кривой рукой: −1…1
    bool m_adjustKnown = false;             // сдвиг ещё не выбран — взять из ручной яркости
    int m_lux = -1;                         // последнее от датчика
    int m_ambient = -1;                     // принятое (после выдержки); −1 — ещё нет
    bool m_pendingUp = false;
    QTimer m_debounce;                      // выдержка перед сменой яркости
    QTimer m_ramp;                          // плавная смена
    QElapsedTimer m_rampClock;
    qreal m_rampTarget = 0, m_rampRate = 0;
    int m_timeoutSec = 60;
    int m_maxBacklight = 0;
    QTimer m_idle;
    QSettings m_settings;
    std::unique_ptr<BacklightWriter> m_backlight;   // запись яркости — в своём потоке
};
