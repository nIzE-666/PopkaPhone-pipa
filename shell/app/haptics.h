// Вибромотор (Xiaomi Pad 6: qti-haptics, FF_CONSTANT через /dev/input/eventN; у оболочки — группа input): короткий
// отклик на нажатие клавиши экранной клавиатуры и на долгое нажатие. В QML — naomiHaptics. Моторa нет — молчит.
#pragma once

#include <QHash>
#include <QObject>

class Haptics : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)   // хранится: ~/.config/naomi/haptics.conf
public:
    explicit Haptics(QObject *parent = nullptr);
    ~Haptics() override;
    bool available() const { return m_fd >= 0; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    Q_INVOKABLE void tap(int ms = 12);   // импульс ms миллисекунд (5…200)

signals:
    void enabledChanged();

private:
    int effectFor(int ms);
    int m_fd = -1;
    bool m_enabled = true;
    QHash<int, int> m_effects;   // длительность → id эффекта в ядре (загружен один раз)
};
