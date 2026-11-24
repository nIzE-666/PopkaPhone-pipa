// НаомиФон — Android-приложения (Waydroid, docs/ANDROID.md). В QML — naomiAndroid.
//   Android — второй Android в контейнере LXC на том же ядре (Waydroid 1.6.3, образ LineageOS 20 с Google Play), его
//   окна — клиент Wayland оболочки, как наши программы. Сессию Android (`waydroid session start`) оболочка запускает
//   сама, когда впервые просят приложение, и держит своим дочерним процессом: оболочка ушла — сессия и контейнер
//   уходят с ней (сокет Wayland оболочки контейнер видит привязанным файлом: новый сокет после перезапуска оболочки
//   ему не виден). Сессия, оставшаяся от прошлой оболочки, при запуске выключается.
//   Список приложений — файлы ~/.local/share/applications/waydroid.<пакет>.desktop: их пишет сам Waydroid, когда
//   Android готов, и при каждой установке и удалении приложения; оболочка следит за папкой.
//   Окно Android узнаётся по группе процессов клиента: lxc.payload.waydroid (isAndroid). Звук Android — AndroidSound,
//   пока Android включён.
#pragma once

#include "android_scroll.h"
#include "android_sound.h"

#include <functional>

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class QDBusServiceWatcher;
class QProcess;
class QWaylandCompositor;
class QWaylandOutput;
struct wl_client;
struct wl_global;

class AndroidApps : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool installed READ installed CONSTANT)                 // Waydroid настроен (образы есть)
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)         // [{pkg, name, icon}] — по имени
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)           // off, starting, on (готов), stopping
    // задачи Android — где окна приложений на самом деле (naomid Android.Tasks): [{id, pkg, x, y, w, h, mode,
    // visible}], пиксели экрана Android; AndroidMove берёт из них рамку своего окна
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
    Q_PROPERTY(bool frozen READ frozen NOTIFY frozenChanged)           // заморожен столом (никто не смотрит)
    Q_PROPERTY(bool asleep READ asleep NOTIFY asleepChanged)           // усыплён столом (окон Android не видно)
public:
    explicit AndroidApps(const QString &waylandSocket, QObject *parent = nullptr);

    bool installed() const { return m_installed; }
    QVariantList apps() const { return m_apps; }
    QString state() const;
    QVariantList tasks() const { return m_tasks; }
    bool frozen() const { return m_frozen; }
    bool asleep() const { return m_asleep; }
    // Видно ли на столе хоть одно окно Android (Desktop.qml, androidSeen). Не видно grace мс подряд — Android спит,
    // как телефон с погашенным экраном (naomid Android.Sleep): свёрнутая или закрытая окном стола игра иначе работала
    // на полную — ядро, видеочип, 3 Вт (docs/ANDROID-AUDIT.md, находка 3). Видно — просыпается сразу (~0,2 с);
    // окна Waydroid при этом создаёт заново — стол ставит их на прежние места (Desktop.qml, androidGone)
    Q_INVOKABLE void setSeen(bool seen, int grace);
    // есть ли у Android окна на столе (Desktop.qml): без окон ролику играть негде — видеокодек ему не нужен (main.cpp,
    // «Кодек на всех»)
    Q_INVOKABLE void setWindows(bool any);
    bool windows() const { return m_windows; }
    // перечитать задачи Android (naomid RefreshTasks): окно появилось, его слои сменили размер или место сами
    Q_INVOKABLE void refreshTasks();

    // клиент Wayland с этим pid — Android (процесс контейнера Waydroid)
    Q_INVOKABLE bool isAndroid(qint64 pid);
    // открыть приложение (пакет): Android выключен — включить, приложение — когда он готов
    Q_INVOKABLE void launch(const QString &pkg);
    Q_INVOKABLE void stop();   // выключить Android
    // Android встал: окно пакета pkg так и не получило картинки (AndroidWindow, 12 с) — служба вывода Waydroid ждёт
    // ответа Wayland, которого нет, а за ней стоит весь Android (docs/ANDROID.md, «Подводные камни»). Перезапустить
    // Android и открыть заново pkg и приложения, которые просили за последнюю минуту; не чаще раза в 5 минут
    Q_INVOKABLE void restartStuck(const QString &pkg);
    // выход оболочки: сессию — SIGTERM (она гасит контейнер), не ждём
    void shutdown();
    // Композитор и вывод стола. Из глобальных объектов Wayland Android видит один вывод — стол (wl_output телефона
    // скрыт): Waydroid берёт частоту своей развёртки — наибольшую из выводов, раз при включении, — и с выводом телефона
    // шёл по 90 Гц, а стол — по монитору зрителя (docs/ANDROID.md, «Кадры в такт стола»). Отметки показа
    // (wp_presentation) — только Android: другим программам их не шлёт никто
    void setCompositor(QWaylandCompositor *compositor);
    void setDeskOutput(QWaylandOutput *output);
    // вывод, который виден только программам Windows (узнаются по номеру процесса), а других они не видят: Wine берёт
    // список режимов экрана у вывода (docs/WINDOWS.md, «Выводы»; DesktopScreen::winOutput)
    void setWinOutput(QWaylandOutput *output, std::function<bool(qint64)> isWin);
    // Колесо над окном Android — жестом прокрутки (android_scroll.h): слою layer (элемент Qt Wayland) в его точке at,
    // сдвиг d — в единицах слоя (вниз и вправо — плюс); end — прокрутка кончилась
    Q_INVOKABLE void wheel(QObject *layer, QPointF at, QPointF d, bool end);
    // вернуть фокус ввода Android окну item (элемент окна Waydroid): клавиатура Wayland уходит с него и приходит снова —
    // Waydroid по её приходу делает задачу активной. После жеста колеса над другим окном Android (android_scroll.h)
    Q_INVOKABLE void refocus(QObject *item);
    // перенести задачу Android пакета pkg из from (пиксели экрана Android) левым верхом в to: стол дотащил окно сам и
    // отпустил (AndroidMove). Команду Android даёт только root в контейнере — переносит naomid (Android.MoveTask)
    Q_INVOKABLE void moveTask(const QString &pkg, QRect from, QPoint to);
    // то же с размером: задачу — из from в прямоугольник to (развернуть во весь экран Android и вернуть — AndroidMove)
    Q_INVOKABLE void resizeTask(const QString &pkg, QRect from, QRect to);
    // стол никто не смотрит (WebLink::freezeDesk, вместе с браузером) — Android замораживается целиком: процессы
    // контейнера стоят (lxc-freeze, Waydroid ContainerManager.Freeze), память цела; зритель вернулся — оттаивает
    void freeze(bool on);

signals:
    void appsChanged();
    void stateChanged();
    void window(QObject *toplevel, QObject *xdgSurface);   // окно Android (Main.qml → стол)
    void launching(const QString &pkg);                    // приложение просили — его окно ещё в пути
    void tasksChanged();
    void frozenChanged();
    void asleepChanged();
    void windowsChanged();
    void scrollEnded(QObject *layer);   // жест колеса кончился: палец был на этом слое (android_scroll.h)

private slots:
    void onTaskProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    enum class State { Off, Starting, On, Stopping };
    void applyTasks(const QString &json);
    QVariantList m_tasks;
    void setState(State s);
    void startSession();
    void runWaydroid(const QStringList &args);
    void readApps();
    void sessionAppeared();
    void androidReady();
    void applyFreeze();
    void sessionGone();
    void restart(const char *why, QStringList apps);
    QTimer m_startTimeout;              // включается дольше 180 с — встал (служба вывода не отметилась)
    qint64 m_restartedAt = 0;           // когда перезапускали сами (мс), — не чаще раза в 5 минут
    QHash<QString, qint64> m_launchedAt;   // пакет → когда просили открыть (мс)

    QString m_socket;
    bool m_installed = false;
    State m_state = State::Off;
    QProcess *m_session = nullptr;
    QDBusServiceWatcher *m_watch = nullptr;
    QStringList m_pending;       // приложения, ждущие сессии
    bool m_wantFrozen = false;   // стол не смотрят — Android должен стоять
    bool m_frozen = false;
    void sleep(bool on);
    void sendSleep();
    void updateIdleStop();
    QTimer m_sleepTimer;         // окон Android не видно — отсчёт до сна
    QTimer m_idleStopTimer;      // все окна Android закрыты 10 мин — остановить сессию, чтобы не сажать батарею
    bool m_windows = false;      // у Android есть окна на столе
    bool m_asleep = false;       // так решил стол
    bool m_sleepSent = false;    // так сказано Android (заморожен — скажем, когда оттает)
    QHash<qint64, bool> m_clients;
    QVariantList m_apps;
    QString m_appsDir;
    QFileSystemWatcher m_dirWatch;
    QTimer m_reread;
    QWaylandCompositor *m_compositor = nullptr;
    std::function<bool(qint64)> m_winClient;
    void *m_winOutput = nullptr;
    void *m_deskOutput = nullptr;   // глобальный объект wl_output стола (его данные — для фильтра)
    static bool filterGlobal(const wl_client *client, const wl_global *global, void *self);
    AndroidSound m_sound;
    AndroidScroll m_scroll;
};
