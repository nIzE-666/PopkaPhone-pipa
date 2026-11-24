#include "display_stats.h"
#include "device_profile.h"
#include "scene_rotation.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSettings>

#include <vector>

namespace {

NaomiDisplaySizes sizes()
{
    NaomiDisplaySizes s{};
    if (auto fn = reinterpret_cast<NaomiDisplaySizesFn>(QGuiApplication::platformFunction(kNaomiDisplaySizes)))
        fn(&s);
    return s;
}

} // namespace

DisplayStats::DisplayStats(QObject *parent)
    : QObject(parent)
    , m_snapshot(reinterpret_cast<NaomiFrameSnapshotFn>(
          QGuiApplication::platformFunction(kNaomiFrameSnapshot)))
    , m_setRefresh(reinterpret_cast<NaomiSetRefreshFn>(
          QGuiApplication::platformFunction(kNaomiSetRefresh)))
{
    const auto &profile = DeviceProfile::current();
    if (profile.tablet) {
        const NaomiDisplaySizes display = sizes();
        const int fallback = display.panelWidth > 0 ? display.sceneRotation : profile.landscapeRotation;
        QSettings settings("naomi", "shell");
        bool ok = false;
        const int saved = settings.value("display/sceneRotation").toInt(&ok);
        m_sceneRotation = ok && SceneRotation::isValid(saved) ? saved : fallback;
        m_autoRotate = settings.value("display/autoRotate", false).toBool();
    }
    if (!m_snapshot)
        return;  // не eglfs_naomi (например, запуск на Mac) — цифр нет
    m_snapshot(&m_s);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_snapshot(&m_s);
        emit changed();
    });
    m_timer.start(250);
}

QSize DisplayStats::renderSize() const
{
    const NaomiDisplaySizes s = sizes();
    return {s.renderWidth, s.renderHeight};
}

QSize DisplayStats::panelSize() const
{
    const NaomiDisplaySizes s = sizes();
    return {s.panelWidth, s.panelHeight};
}

void DisplayStats::setSceneRotation(int rotation)
{
    if (!DeviceProfile::current().tablet || !SceneRotation::isValid(rotation) || rotation == m_sceneRotation)
        return;
    m_sceneRotation = rotation;
    QSettings("naomi", "shell").setValue("display/sceneRotation", rotation);
    emit sceneRotationChanged();
}

void DisplayStats::setAutoRotate(bool enabled)
{
    if (!DeviceProfile::current().tablet || enabled == m_autoRotate)
        return;
    m_autoRotate = enabled;
    QSettings("naomi", "shell").setValue("display/autoRotate", enabled);
    emit autoRotateChanged();
}

void DisplayStats::chooseOrientation(int rotation)
{
    if (!SceneRotation::isValid(rotation))
        return;
    setAutoRotate(false);
    setSceneRotation(rotation);
}

void DisplayStats::toggleOrientation()
{
    chooseOrientation(SceneRotation::toggle(m_sceneRotation));
}

int DisplayStats::savedRenderWidth()
{
    const auto &profile = DeviceProfile::current();
    if (profile.tablet)
        return 0;  // планшет всегда подаёт кадр размером с панель
    return QSettings("naomi", "shell").value("display/render", 1080).toInt();
}

QVariantList DisplayStats::refreshRates() const
{
    QVariantList rates;
    if (auto fn = reinterpret_cast<NaomiRefreshRatesFn>(QGuiApplication::platformFunction(kNaomiRefreshRates))) {
        std::vector<int> hz(size_t(fn(nullptr, 0)));
        fn(hz.data(), int(hz.size()));
        for (int rate : hz)
            rates.append(rate);
    }
    return rates;
}

void DisplayStats::setRenderWidth(int width)
{
    if (width != 720 && width != 1080)
        return;
    QSettings("naomi", "shell").setValue("display/render", width);
    if (width != renderSize().width())
        emit restartRequested();
}

void DisplayStats::setActive(bool on)
{
    if (!m_snapshot)
        return;
    if (on)
        m_timer.start(250);
    else
        m_timer.stop();
}

bool DisplayStats::setRefresh(int hz)
{
    if (!m_setRefresh || !m_setRefresh(hz))
        return false;
    if (auto *window = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow()))
        window->update();
    return true;
}
