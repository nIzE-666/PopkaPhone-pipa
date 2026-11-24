// «Браузер» — блокировка рекламы и слежки (docs/WEB.md, «Реклама и слежка»): перехватчик запросов Qt WebEngine по
// доменным правилам EasyList и EasyPrivacy (их собирает tools/naomi web-filters в ~/.local/share/naomi/web/filters.txt).
// Запрос к домену из списка (или его поддомену) не уходит — страница грузится без него: быстрее и без слежки. Саму
// страницу, на которую идут (адрес в строке), не блокирует никогда. Заголовки API — third_party/README.md.
#pragma once

#include <QtWebEngineCore/qwebengineurlrequestinterceptor.h>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class QThread;

class WebFilter : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT
public:
    // список читается в своём потоке (≈ 90 тыс. доменов — 70–116 мс): запуск браузера его не ждёт; пока не прочитан,
    // не блокируется ничего — в эти доли секунды движок ещё только поднимается
    explicit WebFilter(const QString &path, QObject *parent = nullptr);
    ~WebFilter() override;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override;
    // профиль страниц (WebEngineProfile из QML): с этой минуты его запросы идут через фильтр
    Q_INVOKABLE void attach(QObject *profile);

private:
    struct Rule {
        quint32 types = 0;          // какие запросы (биты типов; 0 — все)
        bool thirdParty = false;    // только сторонние (не того же сайта, что страница)
        QStringList on;             // исключение — только на этих сайтах (пусто — везде)
    };
    struct Lists {
        QHash<QString, Rule> block, allow;
        QSet<QString> sites;
    };
    static bool load(const QString &path, Lists &out);
    // домен или его родитель
    static const Rule *find(const QHash<QString, Rule> &rules, const QString &host);
    static bool hasDomain(const QSet<QString> &domains, const QString &host);

    QThread *m_loader = nullptr;
    QHash<QString, Rule> m_block, m_allow;
    QSet<QString> m_sites;          // на этих сайтах не блокировать ничего
    quint64 m_blocked = 0, m_logged = 0;
    qint64 m_logAt = 0;
};
