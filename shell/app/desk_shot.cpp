#include "desk_shot.h"

#include "desktop_screen.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QMutex>
#include <QQuickImageProvider>

#include <cmath>

Q_LOGGING_CATEGORY(lcDeskShot, "naomi.screenshot")

namespace {

constexpr int kWaitMs = 1000;        // SCREENSHOT_WAIT_MS: стол так долго не даёт кадр — снимок не получился
constexpr double kThumbMaxW = 320;   // THUMB_MAX_W × THUMB_MAX_H: миниатюра вписана сюда (точки стола)
constexpr double kThumbMaxH = 200;
// Сжатие PNG — zlib 1 (Qt: качество 80). Замер 18 ноября 2026, кадр стола приставки 2400 × 1350: по умолчанию
// (zlib 6) — 1,95 с на большом ядре и 2,7 с на малом, 2,1 МБ; zlib 1 — 0,35 и 0,72 с, 2,6 МБ; без сжатия — 0,19 с,
// но 9,5 МБ. У НаомиОС снимок пишется за десятые доли секунды — и миниатюра выезжает сразу; тут так же
constexpr int kPngQuality = 80;

QString folder() { return QDir::homePath() + QStringLiteral("/Screenshots"); }

} // namespace

// Миниатюра последнего снимка — Image в QML берёт её сразу (без чтения файла)
class DeskShot::Provider : public QQuickImageProvider
{
public:
    Provider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    void set(const QString &id, QImage image)
    {
        QMutexLocker lock(&m_mutex);
        m_id = id;
        m_image = std::move(image);
    }
    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        QMutexLocker lock(&m_mutex);
        if (id != m_id)
            return {};
        if (size)
            *size = m_image.size();
        return m_image;
    }

private:
    QMutex m_mutex;
    QString m_id;
    QImage m_image;
};

DeskShot::DeskShot(DesktopScreen *desk, QObject *parent)
    : QObject(parent)
    , m_desk(desk)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kWaitMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_waiting)
            fail(QStringLiteral("стол не дал кадр"));
    });
}

DeskShot::~DeskShot()
{
    if (m_writer.joinable())
        m_writer.join();
}

QQuickImageProvider *DeskShot::imageProvider()
{
    if (!m_provider)
        m_provider = new Provider;
    return m_provider;
}

void DeskShot::setBusy(bool on)
{
    if (on == m_busy)
        return;
    m_busy = on;
    emit busyChanged();
}

bool DeskShot::take(const QRectF &area)
{
    if (m_busy)
        return false;
    // Имя: «Скриншот 2026-11-18 в 07.53.56.png»; два в одну секунду — второй с номером «(2)»
    const QString dir = folder();
    m_folderCreated = !QDir(dir).exists();
    if (!QDir().mkpath(dir)) {
        fail(QStringLiteral("нет папки ~/Screenshots"));
        return true;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const QString name = QStringLiteral("Скриншот %1 в %2").arg(now.toString(QStringLiteral("yyyy-MM-dd")),
                                                                 now.toString(QStringLiteral("HH.mm.ss")));
    m_path = dir + '/' + name + QStringLiteral(".png");
    for (int n = 2; QFile::exists(m_path) && n < 100; ++n)
        m_path = dir + '/' + name + QStringLiteral(" (%1).png").arg(n);

    // Рамка — в пиксели кадра тем же округлением, что у сцены (round(x·s), s — как сцена ложится в кадр,
    // pixelScale): снимается ровно то, что было в рамке
    const double s = m_desk->pixelScale();
    const QSize frame = m_desk->pixelSize();
    QRect px(QPoint(), frame);
    if (!area.isEmpty()) {
        const int x0 = int(std::lround(area.left() * s)), y0 = int(std::lround(area.top() * s));
        const int x1 = int(std::lround(area.right() * s)), y1 = int(std::lround(area.bottom() * s));
        px = QRect(x0, y0, x1 - x0, y1 - y0).intersected(px);
    }
    if (px.isEmpty()) {
        fail(QStringLiteral("пустая область"));
        return true;
    }
    // Миниатюра: снимок, вписанный в 320 × 200 точек (мелкий — как есть); в пикселях — как у сцены: ляжет 1:1
    const double lw = px.width() / s, lh = px.height() / s;
    const double k = std::min(1.0, std::min(kThumbMaxW / lw, kThumbMaxH / lh));
    m_thumbPixels = QSize(std::max(1, int(std::lround(lw * k * s))), std::max(1, int(std::lround(lh * k * s))));
    m_thumbSize = QSizeF(m_thumbPixels) / s;

    setBusy(true);
    m_waiting = true;
    m_timeout.start();
    if (!m_desk->captureNext(px, [this](QImage image) { captured(std::move(image)); })) {
        fail(QStringLiteral("стол сейчас не показывают"));
        return true;
    }
    qCInfo(lcDeskShot, "скриншот: %s (%dx%d в %d,%d)", qPrintable(m_path), px.width(), px.height(), px.x(), px.y());
    return true;
}

// Кадр стола взят (поток интерфейса, сразу после кадра): PNG и миниатюра — своим потоком, стол не ждёт
void DeskShot::captured(QImage image)
{
    if (!m_waiting)
        return;   // уже не ждали (срок вышел)
    m_waiting = false;
    m_timeout.stop();
    if (image.isNull()) {
        fail(QStringLiteral("не удалось прочитать кадр"));
        return;
    }
    if (m_writer.joinable())
        m_writer.join();
    m_writer = std::thread([this, image = std::move(image), path = m_path, thumbPixels = m_thumbPixels]() mutable {
        QElapsedTimer t;
        t.start();
        // строки снизу вверх (как у GL) — перевернуть; стол непрозрачен: PNG без альфы
        const QImage rgb = image.flipped(Qt::Vertical).convertToFormat(QImage::Format_RGB32);
        image = QImage();
        QImage thumb = rgb.scaled(thumbPixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);   // усреднение
        const qint64 thumbMs = t.elapsed();
        const int slash = path.lastIndexOf('/');
        const QString part = path.left(slash + 1) + '.' + path.mid(slash + 1) + QStringLiteral(".part");
        QImageWriter writer(part, "png");
        writer.setQuality(kPngQuality);
        bool ok = writer.write(rgb);
        ok = ok && QFile::rename(part, path);
        if (!ok)
            QFile::remove(part);
        qCInfo(lcDeskShot, "скриншот %s: %dx%d, миниатюра %lld мс, всё %lld мс", ok ? "сохранён" : "НЕ сохранён",
               rgb.width(), rgb.height(), thumbMs, t.elapsed());
        QMetaObject::invokeMethod(this, [this, ok, thumb = std::move(thumb), full = ok ? rgb : QImage()]() mutable {
            if (ok && !full.isNull())
                if (QClipboard *cb = QGuiApplication::clipboard())
                    cb->setImage(full);
            finish(ok, std::move(thumb));
        }, Qt::QueuedConnection);
    });
}

void DeskShot::finish(bool ok, QImage thumb)
{
    if (m_writer.joinable())
        m_writer.join();
    if (!ok) {
        fail(QStringLiteral("не удалось записать файл"));
        return;
    }
    const QString id = QString::number(++m_serial);
    if (m_provider)
        m_provider->set(id, std::move(thumb));
    setBusy(false);
    emit saved(m_path, QStringLiteral("image://deskshot/") + id, m_thumbSize, m_folderCreated);
}

void DeskShot::fail(const QString &why)
{
    qCWarning(lcDeskShot, "скриншот не получился: %s", qPrintable(why));
    m_waiting = false;
    m_timeout.stop();
    setBusy(false);
    emit failed(why);
}
