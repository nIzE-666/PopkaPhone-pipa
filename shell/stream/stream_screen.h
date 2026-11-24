// НаомиФон — экран, который служба трансляции может показывать вместо экрана телефона: рисуется в свою текстуру
// своим контекстом GL (рабочий стол — shell/app/desktop_screen.h, docs/DESKTOP.md). Служба снимает кадр в
// afterRendering его окна и шлёт туда ввод зрителя (мышь, клавиши).
#pragma once

#include <QSize>
#include <QPointF>

class QQuickWindow;

class StreamScreen
{
public:
    virtual ~StreamScreen() = default;
    virtual QQuickWindow *window() const = 0;
    virtual QSize pixelSize() const = 0;     // кадр
    virtual QSize logicalSize() const = 0;   // в точках: сюда — мышь
    virtual QPointF mapToWindow(QPointF point) const { return point; }
    // новая форма и масштаб (кадр — заново): кадр — logical × scale, чётный; служба буферы уже отпустила
    virtual void setGeometry(QSize logical, double scale) = 0;
    virtual double scale() const = 0;        // пикселей кадра в точке
    virtual unsigned texture() const = 0;    // кадр — текстура GL (первая строка — низ кадра, как у окна)
    virtual void setActive(bool on) = 0;     // смотрят — рисует; нет — стоит
    // кадров не чаще; named — частоту назвал зритель (Want: частота его монитора), нет — по умолчанию сеанса
    virtual void setMaxFps(int fps, bool named) = 0;
    // такт зрителя (Tick): кадр — сейчас, если есть что рисовать; value — его обновление монитора (мкс его часов)
    virtual void tick(quint64 value) = 0;
    virtual void requestFrame() = 0;
    virtual bool makeCurrent() = 0;
    virtual void doneCurrent() = 0;
};
