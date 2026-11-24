// НаомиФон — ввод текста: модуль ввода Qt «naomi» (QT_IM_MODULE), статический плагин из библиотеки
// naomi-input — один и тот же в оболочке и в каждой программе (docs/KEYBOARD.md). Qt сообщает ему, когда поле
// ввода просит клавиатуру, а клавиатура (Naomi.Ui: Keyboard, в QML — naomiKeyboard) через него печатает в то,
// что сейчас в фокусе: текст — QInputMethodEvent, «стереть» и «ввод» — клавишами. В программе клавиатура
// рисуется в её же окне (KeyboardPanel) — с ней одно целое; в оболочке — поверх, для её полей.
//
// Внешняя клавиатура (трансляция экрана на Mac, docs/STREAM.md): в программе модуль слушает D-Bus — служба
// org.naomi.Input.p<pid>, объект /input, интерфейс org.naomi.Input (externalKey). Оболочка шлёт туда клавиши
// зрителя для программы в фокусе: текст печатается тем же путём, что с экранной клавиатуры, — любой раскладкой.
//
// Тачпад (как в iOS: долгое нажатие на пробел): trackStart/trackMove/trackEnd — палец водит точку от курсора
// поля (на 12 % быстрее пальца), курсор встаёт на знак под ней (positionAt поля TextEdit). trackPoint — эта точка в координатах поля:
// программа может нарисовать по ней «плавающий» курсор (блокнот). У поля без positionAt (TextInput) курсор
// ходит стрелками — шаг на каждые 9 точек пальца.
#pragma once

#include <QPointer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <qpa/qplatforminputcontext.h>
#include <qpa/qplatforminputcontextplugin_p.h>

#include <functional>
#include <utility>

class KeyboardContext : public QPlatformInputContext
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isInputPanelVisible NOTIFY visibleChanged)
    Q_PROPERTY(bool tracking READ tracking NOTIFY trackingChanged)
    Q_PROPERTY(QPointF trackPoint READ trackPoint NOTIFY trackPointChanged)
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged)
    Q_PROPERTY(bool inputEnabled READ inputEnabled NOTIFY focusObjectChanged)
public:
    using KeyFallback = std::function<void(bool down, int qtKey, int modifiers, const QString &text)>;

    static KeyboardContext *instance();   // после создания QGuiApplication; null — модуль не загружен

    KeyboardContext();
    ~KeyboardContext() override;

    bool isValid() const override { return true; }
    void showInputPanel() override;
    void hideInputPanel() override;
    void setManualOnly(bool enabled) { m_manualOnly = enabled; }
    bool isManualOnly() const { return m_manualOnly; }
    void setDismissOnFocusLoss(bool enabled) { m_dismissOnFocusLoss = enabled; }
    void setKeyFallback(KeyFallback fallback) { m_keyFallback = std::move(fallback); }
    bool isInputPanelVisible() const override { return m_visible; }
    bool isManual() const { return m_manual; }
    QRectF keyboardRect() const override { return m_rect; }
    void setFocusObject(QObject *object) override;
    void update(Qt::InputMethodQueries queries) override;

    QString layout() const { return m_layout; }
    Q_INVOKABLE void setLayout(const QString &layout);
    Q_INVOKABLE void toggleLayout();
    bool inputEnabled() const;
    Q_INVOKABLE void showManual();
    Q_INVOKABLE void toggle();

    Q_INVOKABLE void commit(const QString &text);
    Q_INVOKABLE void replaceBefore(int count, const QString &text);   // заменить count знаков перед курсором
    Q_INVOKABLE void key(int qtKey);                     // Qt::Key_Backspace, Qt::Key_Return…
    // клавиша внешней клавиатуры: текст без клавиши (qtKey 0) — печатается, иначе — нажатие с модификаторами
    void externalKey(bool down, int qtKey, int modifiers, const QString &text);
    // Браузер (docs/WEB.md): сайтам нужны нажатия, а не ввод текста — у YouTube «f», пробел, «k» — клавиши.
    // Тогда латиница и цифры приходят нажатием с текстом, прочие буквы — нажатием вне поля ввода; в поле — вводом.
    static void setRealKeys(bool on);
    Q_INVOKABLE QString textBeforeCursor() const;         // для заглавной в начале предложения
    Q_INVOKABLE int hints() const;                        // Qt::InputMethodHints поля: пароль, латиница…
    Q_INVOKABLE void setRect(qreal x, qreal y, qreal w, qreal h);   // где клавиатура, в точках окна

    bool tracking() const { return m_tracking; }
    QPointF trackPoint() const { return m_trackPoint; }
    Q_INVOKABLE bool trackStart();                 // false — в фокусе нет поля с курсором
    Q_INVOKABLE void trackMove(qreal dx, qreal dy);   // сдвиг пальца, точки
    Q_INVOKABLE void trackEnd();

signals:
    void visibleChanged();
    void trackingChanged();
    void trackPointChanged();
    void layoutChanged();
    void focusObjectChanged();

private:
    QVariant query(Qt::InputMethodQuery q) const;
    bool usesDirectInput(QObject *focus) const;
    void setVisible(bool visible);

    bool m_visible = false;
    bool m_manual = false;
    bool m_manualOnly = false;
    bool m_dismissOnFocusLoss = false;
    QString m_layout = QStringLiteral("EN");
    KeyFallback m_keyFallback;
    QRectF m_rect;
    bool m_tracking = false;
    QPointer<QObject> m_trackItem;
    QPointF m_trackPoint;
    qreal m_arrowCarry = 0;   // поле без positionAt: накопленный сдвиг до следующей стрелки
    static inline bool s_realKeys = false;
};

class KeyboardContextPlugin : public QPlatformInputContextPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformInputContextFactoryInterface_iid FILE "keyboard_context.json")
public:
    QPlatformInputContext *create(const QString &key, const QStringList &params) override;
};
