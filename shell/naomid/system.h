// naomid — телефон целиком: перезагрузить и выключить (кнопки в шторке телефона и в меню N рабочего стола — со
// вторым нажатием, docs/NAOMID.md, «Система»). Служба — root: systemd сам выполнит, оболочке (phablet) прав не нужно.
#pragma once

#include "bus_object.h"

#include <QProcess>
#include <QTimer>

class System : public BusObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.naomi.Phone.System")

public:
    explicit System(QObject *parent = nullptr);

public slots:
    Q_SCRIPTABLE void Reboot();
    Q_SCRIPTABLE void PowerOff();
    Q_SCRIPTABLE void RebootOneUi(const QDBusMessage &request);

private:
    void replyToOneUi(const QString &error);

    QProcess m_oneUi;
    QTimer m_oneUiTimeout;
    QDBusMessage m_oneUiRequest;
};
