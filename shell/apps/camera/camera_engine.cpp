#include "camera_engine.h"
#include "hybris_camera.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int64_t steadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int clampByte(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

bool writeFileAtomic(const QString &path, const uint8_t *data, size_t size, QString *errOut = nullptr)
{
    QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        if (errOut)
            *errOut = QStringLiteral("Не удалось создать папку %1").arg(fi.absolutePath());
        return false;
    }

    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut)
            *errOut = QStringLiteral("Не удалось открыть %1: %2").arg(tmpPath, tmp.errorString());
        return false;
    }
    const qint64 written = tmp.write(reinterpret_cast<const char *>(data), static_cast<qint64>(size));
    tmp.flush();
    tmp.close();
    if (written != static_cast<qint64>(size)) {
        QFile::remove(tmpPath);
        if (errOut)
            *errOut = QStringLiteral("Ошибка записи JPEG на диск");
        return false;
    }

    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        QFile::remove(tmpPath);
        if (errOut)
            *errOut = QStringLiteral("Не удалось переименовать файл в %1").arg(path);
        return false;
    }
    return true;
}

QString uniqueMediaPath(const QString &dirPath, const QString &prefix, const QString &ext)
{
    QDir().mkpath(dirPath);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString candidate = QStringLiteral("%1/%2_%3.%4").arg(dirPath, prefix, stamp, ext);
    int suffix = 1;
    while (QFileInfo::exists(candidate)) {
        candidate = QStringLiteral("%1/%2_%3_%4.%5").arg(dirPath, prefix, stamp).arg(suffix++).arg(ext);
    }
    return candidate;
}

QString picturesCameraDir()
{
    QString pics = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pics.isEmpty())
        pics = QDir::homePath() + QStringLiteral("/Pictures");
    return pics + QStringLiteral("/Camera");
}

QString videosDir()
{
    QString vids = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (vids.isEmpty())
        vids = QDir::homePath() + QStringLiteral("/Videos");
    return vids;
}

std::pair<int, int> pictureSizeForPreset(int cameraId, int preset)
{
    if (cameraId == 0) {
        // Back camera (13 MP on pipa)
        if (preset == 0)
            return {4208, 3120};
        if (preset == 1)
            return {1920, 1080};
        return {1280, 720};
    }
    // Front camera (8 MP on pipa)
    if (preset == 0)
        return {3264, 2448};
    if (preset == 1)
        return {1920, 1080};
    return {1280, 720};
}

FlashMode parseFlashString(const QString &s)
{
    const QString lower = s.trimmed().toLower();
    if (lower == QLatin1String("auto"))
        return FLASH_MODE_AUTO;
    if (lower == QLatin1String("on"))
        return FLASH_MODE_ON;
    if (lower == QLatin1String("torch"))
        return FLASH_MODE_TORCH;
    return FLASH_MODE_OFF;
}

volatile sig_atomic_t g_headlessStop = 0;

void headlessSignalHandler(int)
{
    g_headlessStop = 1;
}

} // namespace

// Non-blocking NV21 -> H.264/MP4 writer using /usr/bin/ffmpeg via POSIX pipe.
// Uses a bounded frame queue so the Android camera binder thread never stalls.
class VideoRecorder {
public:
    ~VideoRecorder() { stop(); }

    bool start(const QString &outPath, int width, int height, int rotationDeg, bool mirrored, QString *errOut = nullptr)
    {
        stop();

        QFileInfo fi(outPath);
        if (!QDir().mkpath(fi.absolutePath())) {
            if (errOut)
                *errOut = QStringLiteral("Не удалось создать папку %1").arg(fi.absolutePath());
            return false;
        }

        int pipefd[2] = {-1, -1};
        if (pipe2(pipefd, O_CLOEXEC) != 0) {
            if (errOut)
                *errOut = QStringLiteral("pipe2() failed for ffmpeg");
            return false;
        }

        std::string sizeStr = std::to_string(width) + "x" + std::to_string(height);
        std::string outUtf8 = outPath.toUtf8().toStdString();

        std::string vf;
        if (rotationDeg == 90)
            vf = "transpose=1";
        else if (rotationDeg == 180)
            vf = "hflip,vflip";
        else if (rotationDeg == 270)
            vf = "transpose=2";
        if (mirrored) {
            if (!vf.empty())
                vf += ",";
            vf += "hflip";
        }

        std::vector<std::string> argvStorage = {
            "/usr/bin/ffmpeg",
            "-y",
            "-loglevel", "error",
            "-f", "rawvideo",
            "-pixel_format", "nv21",
            "-video_size", sizeStr,
            "-framerate", "25",
            "-i", "pipe:0",
        };
        if (!vf.empty()) {
            argvStorage.push_back("-vf");
            argvStorage.push_back(vf);
        }
        argvStorage.insert(argvStorage.end(), {
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-crf", "23",
            "-pix_fmt", "yuv420p",
            "-movflags", "+faststart",
            outUtf8,
        });

        std::vector<char *> argv;
        argv.reserve(argvStorage.size() + 1);
        for (auto &s : argvStorage)
            argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            if (errOut)
                *errOut = QStringLiteral("fork() failed for ffmpeg");
            return false;
        }
        if (pid == 0) {
            dup2(pipefd[0], STDIN_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(argv[0], argv.data());
            _exit(127);
        }

        close(pipefd[0]);
        m_writeFd = pipefd[1];
        m_pid = pid;
        m_frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
        m_outPath = outPath;
        m_active.store(true);
        m_stopRequested = false;
        m_writerThread = std::thread(&VideoRecorder::writerLoop, this);
        return true;
    }

    void pushFrame(const void *data, uint32_t size)
    {
        if (!m_active.load(std::memory_order_relaxed))
            return;
        if (!data || size < m_frameBytes)
            return;

        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock() || m_stopRequested || m_queue.size() >= kMaxQueuedFrames)
            return; // drop frame rather than blocking camera HAL callback

        std::vector<uint8_t> buf(m_frameBytes);
        std::memcpy(buf.data(), data, m_frameBytes);
        m_queue.push_back(std::move(buf));
        lock.unlock();
        m_cv.notify_one();
    }

    QString stop()
    {
        if (!m_active.exchange(false) && m_pid <= 0 && !m_writerThread.joinable())
            return {};

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopRequested = true;
        }
        m_cv.notify_all();

        if (m_writerThread.joinable())
            m_writerThread.join();

        if (m_writeFd >= 0) {
            close(m_writeFd);
            m_writeFd = -1;
        }

        if (m_pid > 0) {
            int status = 0;
            // Wait up to 8s for ffmpeg to finalize faststart MP4 atom.
            for (int i = 0; i < 80; ++i) {
                pid_t r = waitpid(m_pid, &status, WNOHANG);
                if (r == m_pid || r < 0) {
                    m_pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (m_pid > 0) {
                kill(m_pid, SIGTERM);
                waitpid(m_pid, &status, 0);
                m_pid = -1;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.clear();
        }
        const QString saved = m_outPath;
        m_outPath.clear();
        return saved;
    }

    bool isActive() const { return m_active.load(std::memory_order_relaxed); }

private:
    void writerLoop()
    {
        while (true) {
            std::vector<uint8_t> frame;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
                if (m_queue.empty() && m_stopRequested)
                    break;
                frame = std::move(m_queue.front());
                m_queue.pop_front();
            }

            const uint8_t *ptr = frame.data();
            size_t left = frame.size();
            while (left > 0 && m_writeFd >= 0) {
                ssize_t n = write(m_writeFd, ptr, left);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR)
                        continue;
                    m_active.store(false);
                    return;
                }
                ptr += n;
                left -= static_cast<size_t>(n);
            }
        }
    }

    static constexpr size_t kMaxQueuedFrames = 4;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<uint8_t>> m_queue;
    std::thread m_writerThread;
    std::atomic<bool> m_active{false};
    bool m_stopRequested = false;
    int m_writeFd = -1;
    pid_t m_pid = -1;
    size_t m_frameBytes = 0;
    QString m_outPath;
};

// ---------------------------------------------------------------------------
// CameraEngine
// ---------------------------------------------------------------------------

CameraEngine::CameraEngine(QObject *parent)
    : QObject(parent)
    , m_recorder(std::make_unique<VideoRecorder>())
{
    signal(SIGPIPE, SIG_IGN);

    m_recTimer.setInterval(1000);
    connect(&m_recTimer, &QTimer::timeout, this, [this] {
        if (m_recording.load()) {
            ++m_recordingSeconds;
            emit recordingSecondsChanged();
        }
    });

    scanExistingLastPhoto();
}

CameraEngine::~CameraEngine()
{
    stop();
}

QString CameraEngine::cameraLabel() const
{
    return m_facing.load() == 0 ? QStringLiteral("Задняя 13 МП") : QStringLiteral("Фронтальная 8 МП");
}

QString CameraEngine::resolutionLabel() const
{
    switch (m_resolutionPreset.load()) {
    case 0:
        return QStringLiteral("Макс");
    case 1:
        return QStringLiteral("FHD");
    default:
        return QStringLiteral("HD");
    }
}

qreal CameraEngine::zoomFactor() const
{
    const int mz = std::max(1, m_maxZoom.load());
    const qreal t = static_cast<qreal>(m_zoom.load()) / static_cast<qreal>(mz);
    return 1.0 + t * 5.0; // 1.0x .. 6.0x
}

QUrl CameraEngine::lastPhotoUrl() const
{
    if (m_lastPhotoPath.isEmpty())
        return {};
    return QUrl::fromLocalFile(m_lastPhotoPath);
}

QImage CameraEngine::latestFrame() const
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrame;
}

void CameraEngine::scanExistingLastPhoto()
{
    QDir dir(picturesCameraDir());
    if (!dir.exists())
        return;
    const QFileInfoList files = dir.entryInfoList(
        {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg")},
        QDir::Files,
        QDir::Time);
    if (!files.isEmpty()) {
        m_lastPhotoPath = files.first().absoluteFilePath();
        ++m_photoRevision;
    }
}

void CameraEngine::setError(const QString &msg)
{
    QMetaObject::invokeMethod(this, [this, msg] {
        if (m_errorMessage != msg) {
            m_errorMessage = msg;
            emit errorMessageChanged();
        }
    }, Qt::QueuedConnection);
}

void CameraEngine::start()
{
    if (m_running.exchange(true))
        return;
    m_worker = std::thread(&CameraEngine::workerLoop, this);
}

void CameraEngine::stop()
{
    stopVideoInternal();
    if (!m_running.exchange(false))
        return;
    m_cmdCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void CameraEngine::postCommand(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_cmdQueue.push_back(std::move(fn));
    }
    m_cmdCv.notify_one();
}

void CameraEngine::setPaused(bool p)
{
    if (m_paused.exchange(p) == p)
        return;
    emit pausedChanged();

    if (p) {
        stopVideoInternal();
        postCommand([this] {
            closeCameraOnWorker();
        });
    } else {
        const int targetId = m_facing.load();
        postCommand([this, targetId] {
            openCameraOnWorker(targetId);
        });
    }
}

void CameraEngine::switchCamera()
{
    if (m_cameraCount.load() < 2)
        return;
    setFacing(m_facing.load() == 0 ? 1 : 0);
}

void CameraEngine::setFacing(int facing)
{
    const int target = facing == 0 ? 0 : 1;
    if (m_facing.load() == target && m_ctrl != nullptr)
        return;

    stopVideoInternal();
    m_facing.store(target);
    m_zoom.store(0);
    m_flashMode.store(0);
    // Front camera defaults to mirrored selfie preview; back camera is unmirrored.
    m_mirrored.store(target == 1);
    m_rotationDegrees.store(0);

    emit facingChanged();
    emit zoomChanged();
    emit flashModeChanged();
    emit mirroredChanged();
    emit rotationDegreesChanged();

    if (!m_paused.load()) {
        postCommand([this, target] {
            openCameraOnWorker(target);
        });
    }
}

void CameraEngine::cycleFlashMode()
{
    if (!m_flashSupported.load())
        return;
    setFlashMode((m_flashMode.load() + 1) % 4);
}

void CameraEngine::setFlashMode(int mode)
{
    if (!m_flashSupported.load())
        return;
    const int clamped = std::clamp(mode, 0, 3);
    if (m_flashMode.exchange(clamped) != clamped)
        emit flashModeChanged();
    postCommand([this, clamped] {
        applyFlashOnWorker(clamped);
    });
}

void CameraEngine::cycleResolution()
{
    const int next = (m_resolutionPreset.load() + 1) % 3;
    m_resolutionPreset.store(next);
    emit resolutionPresetChanged();
    const int camId = m_facing.load();
    postCommand([this, camId, next] {
        applyPictureSizeOnWorker(camId, next);
    });
}

void CameraEngine::setZoom(int zoom)
{
    const int clamped = std::clamp(zoom, 0, std::max(0, m_maxZoom.load()));
    if (m_zoom.exchange(clamped) == clamped)
        return;
    emit zoomChanged();
    postCommand([this, clamped] {
        applyZoomOnWorker(clamped);
    });
}

void CameraEngine::setZoomNormalized(qreal normalized)
{
    const qreal t = std::clamp(normalized, 0.0, 1.0);
    setZoom(static_cast<int>(std::lround(t * m_maxZoom.load())));
}

void CameraEngine::rotatePreview()
{
    const int next = (m_rotationDegrees.load() + 90) % 360;
    m_rotationDegrees.store(next);
    emit rotationDegreesChanged();
}

void CameraEngine::toggleMirror()
{
    m_mirrored.store(!m_mirrored.load());
    emit mirroredChanged();
}

void CameraEngine::setMode(const QString &mode)
{
    const QString normalized = (mode == QLatin1String("video")) ? QStringLiteral("video") : QStringLiteral("photo");
    if (m_mode == normalized)
        return;
    if (normalized == QLatin1String("photo"))
        stopVideoInternal();
    m_mode = normalized;
    emit modeChanged();
}

void CameraEngine::focusAt(qreal normX, qreal normY)
{
    const qreal nx = std::clamp(normX, 0.0, 1.0);
    const qreal ny = std::clamp(normY, 0.0, 1.0);
    postCommand([this, nx, ny] {
        if (!m_api || !m_ctrl || !m_previewActive.load())
            return;
        if (m_api->set_focus_region) {
            const int cx = static_cast<int>(std::lround(nx * 2000.0 - 1000.0));
            const int cy = static_cast<int>(std::lround(ny * 2000.0 - 1000.0));
            constexpr int kHalfBox = 150;
            FocusRegion region;
            region.left = std::clamp(cx - kHalfBox, -1000, 1000 - 2 * kHalfBox);
            region.top = std::clamp(cy - kHalfBox, -1000, 1000 - 2 * kHalfBox);
            region.right = std::clamp(region.left + 2 * kHalfBox, -1000, 1000);
            region.bottom = std::clamp(region.top + 2 * kHalfBox, -1000, 1000);
            region.weight = 1000;
            m_api->set_focus_region(m_ctrl, &region);
        }
        if (m_api->set_auto_focus_mode)
            m_api->set_auto_focus_mode(m_ctrl, AUTO_FOCUS_MODE_AUTO);
        if (m_api->start_autofocus)
            m_api->start_autofocus(m_ctrl);
    });
}

void CameraEngine::takePhoto()
{
    if (!m_ready.load() || m_busy.load() || m_recording.load())
        return;
    m_busy.store(true);
    emit busyChanged();
    emit shutterTriggered();

    const int rot = m_rotationDegrees.load();
    postCommand([this, rot] {
        if (!m_api || !m_ctrl) {
            m_busy.store(false);
            QMetaObject::invokeMethod(this, [this] { emit busyChanged(); }, Qt::QueuedConnection);
            return;
        }
        if (m_api->set_rotation)
            m_api->set_rotation(m_ctrl, rot % 360);
        m_snapshotPending.store(true);
        m_snapshotDeadlineMs.store(steadyNowMs() + 6000);
        m_previewActive.store(false);
        m_api->take_snapshot(m_ctrl);
    });
}

void CameraEngine::toggleVideo()
{
    if (m_recording.load()) {
        stopVideoInternal();
        return;
    }
    if (!m_ready.load() || m_busy.load())
        return;

    const QString outPath = uniqueMediaPath(videosDir(), QStringLiteral("VID"), QStringLiteral("mp4"));
    QString err;
    if (!m_recorder->start(outPath, kPreviewWidth, kPreviewHeight, m_rotationDegrees.load(), m_mirrored.load(), &err)) {
        emit toastMessage(QStringLiteral("Ошибка записи видео: %1").arg(err));
        return;
    }

    m_recording.store(true);
    m_recordingSeconds = 0;
    m_recTimer.start();
    emit recordingChanged();
    emit recordingSecondsChanged();
    emit toastMessage(QStringLiteral("Запись видео начата"));
}

void CameraEngine::stopVideoInternal()
{
    if (!m_recording.exchange(false))
        return;

    QMetaObject::invokeMethod(this, [this] {
        m_recTimer.stop();
        emit recordingChanged();
    }, Qt::QueuedConnection);

    const QString saved = m_recorder->stop();
    if (!saved.isEmpty() && QFileInfo::exists(saved)) {
        const QString name = QFileInfo(saved).fileName();
        QMetaObject::invokeMethod(this, [this, saved, name] {
            emit videoSaved(saved);
            emit toastMessage(QStringLiteral("Видео сохранено: %1").arg(name));
        }, Qt::QueuedConnection);
    }
}

void CameraEngine::openLastPhoto()
{
    if (m_lastPhotoPath.isEmpty() || !QFileInfo::exists(m_lastPhotoPath)) {
        emit toastMessage(QStringLiteral("Нет сохранённых снимков"));
        return;
    }
    if (!QProcess::startDetached(QStringLiteral("naomi-xdg-open"), {m_lastPhotoPath})) {
        if (!QProcess::startDetached(QStringLiteral("xdg-open"), {m_lastPhotoPath})) {
            emit toastMessage(m_lastPhotoPath);
            return;
        }
    }
    emit toastMessage(QFileInfo(m_lastPhotoPath).fileName());
}

void CameraEngine::workerLoop()
{
    OffscreenEglContext egl;
    std::string err;
    if (!egl.init(&err)) {
        setError(QStringLiteral("EGL ошибка: %1").arg(QString::fromStdString(err)));
        return;
    }

    HybrisCameraApi api;
    if (!api.load(&err)) {
        setError(QStringLiteral("libcamera.so.1 недоступна: %1").arg(QString::fromStdString(err)));
        return;
    }
    m_api = &api;

    const int count = std::max(0, api.get_number_of_devices());
    m_cameraCount.store(count);
    QMetaObject::invokeMethod(this, [this] { emit cameraCountChanged(); }, Qt::QueuedConnection);

    if (count == 0) {
        setError(QStringLiteral("Камеры не обнаружены"));
        m_api = nullptr;
        return;
    }

    m_eglTexture = egl.textureId();

    // Initial camera open if not paused.
    if (!m_paused.load())
        openCameraOnWorker(m_facing.load());

    while (m_running.load()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_cmdMutex);
            m_cmdCv.wait_for(lock, std::chrono::milliseconds(16), [this] {
                return !m_running.load() || !m_cmdQueue.empty() || m_texNeedsUpdate.load(std::memory_order_relaxed);
            });
            if (!m_cmdQueue.empty()) {
                task = std::move(m_cmdQueue.front());
                m_cmdQueue.pop_front();
            }
        }

        if (!m_running.load())
            break;

        if (task)
            task();

        if (m_ctrl && m_texNeedsUpdate.exchange(false, std::memory_order_acq_rel)) {
            if (m_api->update_preview_texture && m_previewActive.load())
                m_api->update_preview_texture(m_ctrl);
        }

        // Snapshot watchdog: if HAL failed to deliver JPEG in 6s, recover preview cleanly.
        if (m_snapshotPending.load() && steadyNowMs() > m_snapshotDeadlineMs.load()) {
            m_snapshotPending.store(false);
            m_busy.store(false);
            QMetaObject::invokeMethod(this, [this] {
                emit busyChanged();
                emit toastMessage(QStringLiteral("Таймаут снимка — перезапуск предпросмотра"));
            }, Qt::QueuedConnection);
            restartPreviewOnWorker();
        }
    }

    closeCameraOnWorker();
    m_eglTexture = 0;
    m_api = nullptr;
    egl.shutdown();
}

void CameraEngine::openCameraOnWorker(int cameraId)
{
    closeCameraOnWorker();
    if (!m_api || m_eglTexture == 0)
        return;

    m_ready.store(false);
    QMetaObject::invokeMethod(this, [this] { emit readyChanged(); }, Qt::QueuedConnection);

    static CameraControlListener listener{};
    listener = CameraControlListener{};
    listener.on_msg_error_cb = &CameraEngine::cbError;
    listener.on_msg_shutter_cb = &CameraEngine::cbShutter;
    listener.on_msg_focus_cb = &CameraEngine::cbFocus;
    listener.on_msg_zoom_cb = [](void *, int32_t) {};
    listener.on_data_raw_image_cb = [](void *, uint32_t, void *) {};
    listener.on_data_compressed_image_cb = &CameraEngine::cbCompressedImage;
    listener.on_preview_texture_needs_update_cb = &CameraEngine::cbTextureNeedsUpdate;
    listener.on_preview_frame_cb = &CameraEngine::cbPreviewFrame;
    listener.context = this;

    m_ctrl = m_api->connect_by_id(cameraId, &listener);
    if (!m_ctrl) {
        setError(QStringLiteral("Не удалось подключиться к камере %1").arg(cameraId));
        return;
    }

    setError(QString());

    m_api->set_preview_texture(m_ctrl, static_cast<int>(m_eglTexture));
    m_api->set_preview_size(m_ctrl, kPreviewWidth, kPreviewHeight);
    applyPictureSizeOnWorker(cameraId, m_resolutionPreset.load());
    if (m_api->set_jpeg_quality)
        m_api->set_jpeg_quality(m_ctrl, 92);
    if (m_api->set_rotation)
        m_api->set_rotation(m_ctrl, 0);

    int maxZ = 99;
    if (m_api->get_max_zoom) {
        m_api->get_max_zoom(m_ctrl, &maxZ);
        if (maxZ < 0)
            maxZ = 0;
    }
    m_maxZoom.store(maxZ);
    QMetaObject::invokeMethod(this, [this] { emit maxZoomChanged(); }, Qt::QueuedConnection);

    bool hasFlash = (cameraId == 0);
    if (m_api->enumerate_supported_flash_modes) {
        int flashCount = 0;
        m_api->enumerate_supported_flash_modes(
            m_ctrl,
            [](void *c, FlashMode) {
                ++(*static_cast<int *>(c));
            },
            &flashCount);
        hasFlash = flashCount > 1;
    }
    m_flashSupported.store(hasFlash);
    QMetaObject::invokeMethod(this, [this] { emit flashSupportedChanged(); }, Qt::QueuedConnection);

    if (hasFlash)
        applyFlashOnWorker(m_flashMode.load());
    if (m_zoom.load() > 0)
        applyZoomOnWorker(m_zoom.load());

    m_api->set_preview_callback_mode(m_ctrl, PREVIEW_CALLBACK_ENABLED);
    m_api->start_preview(m_ctrl);
    m_previewActive.store(true);
    m_ready.store(true);
    QMetaObject::invokeMethod(this, [this] { emit readyChanged(); }, Qt::QueuedConnection);
}

void CameraEngine::closeCameraOnWorker()
{
    m_snapshotPending.store(false);
    m_previewActive.store(false);
    if (m_ctrl && m_api) {
        if (m_flashSupported.load() && m_flashMode.load() == FLASH_MODE_TORCH && m_api->set_flash_mode)
            m_api->set_flash_mode(m_ctrl, FLASH_MODE_OFF);
        m_api->set_preview_callback_mode(m_ctrl, PREVIEW_CALLBACK_DISABLED);
        m_api->stop_preview(m_ctrl);
        m_api->disconnect(m_ctrl);
        m_api->delete_ctrl(m_ctrl);
        m_ctrl = nullptr;
    }
    m_ready.store(false);
    QMetaObject::invokeMethod(this, [this] { emit readyChanged(); }, Qt::QueuedConnection);
}

void CameraEngine::applyPictureSizeOnWorker(int cameraId, int preset)
{
    if (!m_api || !m_ctrl)
        return;
    const auto [w, h] = pictureSizeForPreset(cameraId, preset);
    m_api->set_picture_size(m_ctrl, w, h);
}

void CameraEngine::applyFlashOnWorker(int mode)
{
    if (!m_api || !m_ctrl || !m_api->set_flash_mode || !m_flashSupported.load())
        return;
    m_api->set_flash_mode(m_ctrl, static_cast<FlashMode>(std::clamp(mode, 0, 3)));
}

void CameraEngine::applyZoomOnWorker(int zoom)
{
    if (!m_api || !m_ctrl || !m_api->set_zoom)
        return;
    m_api->set_zoom(m_ctrl, std::clamp(zoom, 0, std::max(0, m_maxZoom.load())));
}

void CameraEngine::restartPreviewOnWorker()
{
    if (!m_api || !m_ctrl)
        return;
    m_api->set_preview_callback_mode(m_ctrl, PREVIEW_CALLBACK_ENABLED);
    m_api->start_preview(m_ctrl);
    m_previewActive.store(true);
}

void CameraEngine::saveJpegBytes(std::vector<uint8_t> bytes)
{
    const QString outPath = uniqueMediaPath(picturesCameraDir(), QStringLiteral("IMG"), QStringLiteral("jpg"));
    QString err;
    if (writeFileAtomic(outPath, bytes.data(), bytes.size(), &err)) {
        const QString fileName = QFileInfo(outPath).fileName();
        QMetaObject::invokeMethod(this, [this, outPath, fileName] {
            m_lastPhotoPath = outPath;
            ++m_photoRevision;
            emit lastPhotoChanged();
            emit photoSaved(outPath);
            emit toastMessage(QStringLiteral("Сохранено: %1").arg(fileName));
        }, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(this, [this, err] {
            emit toastMessage(QStringLiteral("Ошибка сохранения: %1").arg(err));
        }, Qt::QueuedConnection);
    }
}

// Fast 2x downsampled BT.601 NV21 -> RGB32 (1280x720 -> 640x360) for smooth 25-30 FPS preview.
QImage CameraEngine::nv21ToRgb32Half(const uint8_t *nv21, int srcW, int srcH)
{
    const int dstW = srcW / 2;
    const int dstH = srcH / 2;
    QImage img(dstW, dstH, QImage::Format_RGB32);
    const uint8_t *yPlane = nv21;
    const uint8_t *uvPlane = nv21 + static_cast<size_t>(srcW) * static_cast<size_t>(srcH);

    for (int y = 0; y < dstH; ++y) {
        const uint8_t *yRow = yPlane + static_cast<size_t>(y * 2) * static_cast<size_t>(srcW);
        const uint8_t *uvRow = uvPlane + static_cast<size_t>(y) * static_cast<size_t>(srcW);
        auto *dst = reinterpret_cast<uint32_t *>(img.scanLine(y));
        for (int x = 0; x < dstW; ++x) {
            const int yv = static_cast<int>(yRow[x * 2]) - 16;
            // NV21 chroma order is V, U interleaved.
            const int v = static_cast<int>(uvRow[x * 2]) - 128;
            const int u = static_cast<int>(uvRow[x * 2 + 1]) - 128;

            const int c = 298 * (yv < 0 ? 0 : yv);
            const int r = clampByte((c + 409 * v + 128) >> 8);
            const int g = clampByte((c - 100 * u - 208 * v + 128) >> 8);
            const int b = clampByte((c + 516 * u + 128) >> 8);
            dst[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16)
                                 | (static_cast<uint32_t>(g) << 8)
                                 | static_cast<uint32_t>(b);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// libhybris C callbacks
// ---------------------------------------------------------------------------

void CameraEngine::cbError(void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self)
        return;
    self->postCommand([self] {
        if (self->m_snapshotPending.exchange(false)) {
            self->m_busy.store(false);
            QMetaObject::invokeMethod(self, [self] { emit self->busyChanged(); }, Qt::QueuedConnection);
            self->restartPreviewOnWorker();
        }
    });
}

void CameraEngine::cbShutter(void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self] { emit self->shutterTriggered(); }, Qt::QueuedConnection);
}

void CameraEngine::cbFocus(void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self] { emit self->focusCompleted(); }, Qt::QueuedConnection);
}

void CameraEngine::cbCompressedImage(void *data, uint32_t size, void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self || !data || size == 0)
        return;

    std::vector<uint8_t> jpeg(size);
    std::memcpy(jpeg.data(), data, size);

    // Never call android_camera_start_preview synchronously from inside HAL callback.
    self->postCommand([self, bytes = std::move(jpeg)]() mutable {
        self->m_snapshotPending.store(false);
        self->saveJpegBytes(std::move(bytes));
        self->restartPreviewOnWorker();
        self->m_busy.store(false);
        QMetaObject::invokeMethod(self, [self] { emit self->busyChanged(); }, Qt::QueuedConnection);
    });
}

void CameraEngine::cbTextureNeedsUpdate(void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self)
        return;
    self->m_texNeedsUpdate.store(true, std::memory_order_release);
    self->m_cmdCv.notify_one();
}

void CameraEngine::cbPreviewFrame(void *data, uint32_t size, void *ctx)
{
    auto *self = static_cast<CameraEngine *>(ctx);
    if (!self || !data)
        return;

    const uint32_t expected = static_cast<uint32_t>(kPreviewWidth * kPreviewHeight * 3 / 2);
    if (size < expected)
        return;

    if (self->m_recorder && self->m_recorder->isActive())
        self->m_recorder->pushFrame(data, size);

    QImage img = nv21ToRgb32Half(static_cast<const uint8_t *>(data), kPreviewWidth, kPreviewHeight);
    {
        std::lock_guard<std::mutex> lock(self->m_frameMutex);
        self->m_latestFrame = std::move(img);
    }
    emit self->frameReady();
}

// ---------------------------------------------------------------------------
// Headless CLI: --snap & --torch (no Wayland / QGuiApplication required)
// ---------------------------------------------------------------------------

int CameraEngine::runHeadlessSnap(const QString &facing,
                                  const QString &outPath,
                                  int zoom,
                                  const QString &flash,
                                  int width,
                                  int height)
{
    const int camId = (facing.trimmed().toLower() == QLatin1String("front") || facing == QLatin1String("1")) ? 1 : 0;
    if (outPath.isEmpty()) {
        std::fprintf(stderr, "naomi-camera --snap: --out <path> is required\n");
        return 2;
    }

    OffscreenEglContext egl;
    std::string err;
    if (!egl.init(&err)) {
        std::fprintf(stderr, "naomi-camera --snap: %s\n", err.c_str());
        return 1;
    }

    HybrisCameraApi api;
    if (!api.load(&err)) {
        std::fprintf(stderr, "naomi-camera --snap: %s\n", err.c_str());
        return 1;
    }

    const int nDevs = api.get_number_of_devices();
    if (camId >= nDevs) {
        std::fprintf(stderr, "naomi-camera --snap: camera id %d unavailable (count=%d)\n", camId, nDevs);
        return 1;
    }

    struct SnapState {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<uint8_t> jpeg;
        bool done = false;
        bool error = false;
        std::atomic<bool> texDirty{false};
    } state;

    CameraControlListener listener{};
    listener.context = &state;
    listener.on_msg_shutter_cb = [](void *) {};
    listener.on_msg_focus_cb = [](void *) {};
    listener.on_msg_zoom_cb = [](void *, int32_t) {};
    listener.on_data_raw_image_cb = [](void *, uint32_t, void *) {};
    listener.on_preview_frame_cb = [](void *, uint32_t, void *) {};
    listener.on_msg_error_cb = [](void *c) {
        auto *st = static_cast<SnapState *>(c);
        std::lock_guard<std::mutex> lk(st->mtx);
        st->error = true;
        st->done = true;
        st->cv.notify_all();
    };
    listener.on_preview_texture_needs_update_cb = [](void *c) {
        static_cast<SnapState *>(c)->texDirty.store(true, std::memory_order_release);
    };
    listener.on_data_compressed_image_cb = [](void *data, uint32_t size, void *c) {
        auto *st = static_cast<SnapState *>(c);
        std::lock_guard<std::mutex> lk(st->mtx);
        if (data && size > 0) {
            st->jpeg.resize(size);
            std::memcpy(st->jpeg.data(), data, size);
        }
        st->done = true;
        st->cv.notify_all();
    };

    CameraControl *ctrl = api.connect_by_id(camId, &listener);
    if (!ctrl) {
        std::fprintf(stderr, "naomi-camera --snap: android_camera_connect_by_id(%d) failed\n", camId);
        return 1;
    }

    api.set_preview_texture(ctrl, static_cast<int>(egl.textureId()));
    api.set_preview_size(ctrl, kPreviewWidth, kPreviewHeight);
    api.set_picture_size(ctrl, std::max(320, width), std::max(240, height));
    if (api.set_jpeg_quality)
        api.set_jpeg_quality(ctrl, 92);
    if (api.set_rotation)
        api.set_rotation(ctrl, 0);

    int maxZ = 99;
    if (api.get_max_zoom)
        api.get_max_zoom(ctrl, &maxZ);
    if (api.set_zoom && zoom > 0)
        api.set_zoom(ctrl, std::clamp(zoom, 0, std::max(0, maxZ)));

    if (camId == 0 && api.set_flash_mode)
        api.set_flash_mode(ctrl, parseFlashString(flash));

    api.set_preview_callback_mode(ctrl, PREVIEW_CALLBACK_ENABLED);
    api.start_preview(ctrl);

    // Let auto-exposure and auto-white-balance settle for ~600ms while draining texture updates.
    const int64_t settleUntil = steadyNowMs() + 600;
    while (steadyNowMs() < settleUntil) {
        if (state.texDirty.exchange(false, std::memory_order_acq_rel) && api.update_preview_texture)
            api.update_preview_texture(ctrl);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    api.take_snapshot(ctrl);

    const int64_t deadline = steadyNowMs() + 5000;
    while (steadyNowMs() < deadline) {
        if (state.texDirty.exchange(false, std::memory_order_acq_rel) && api.update_preview_texture)
            api.update_preview_texture(ctrl);
        std::unique_lock<std::mutex> lk(state.mtx);
        if (state.done)
            break;
        state.cv.wait_for(lk, std::chrono::milliseconds(30), [&state] { return state.done; });
        if (state.done)
            break;
    }

    if (camId == 0 && api.set_flash_mode)
        api.set_flash_mode(ctrl, FLASH_MODE_OFF);
    api.set_preview_callback_mode(ctrl, PREVIEW_CALLBACK_DISABLED);
    api.stop_preview(ctrl);
    api.disconnect(ctrl);
    api.delete_ctrl(ctrl);
    egl.shutdown();

    if (state.jpeg.empty()) {
        std::fprintf(stderr, "naomi-camera --snap: timed out waiting for JPEG snapshot\n");
        return 1;
    }

    QString writeErr;
    if (!writeFileAtomic(outPath, state.jpeg.data(), state.jpeg.size(), &writeErr)) {
        std::fprintf(stderr, "naomi-camera --snap: %s\n", writeErr.toUtf8().constData());
        return 1;
    }

    std::printf("%s\n", outPath.toUtf8().constData());
    std::fflush(stdout);
    return 0;
}

int CameraEngine::runHeadlessTorch(const QString &arg, int durationSec)
{
    const QString lower = arg.trimmed().toLower();
    const bool turnOffOnly = (lower == QLatin1String("off") || lower == QLatin1String("0"));
    int seconds = durationSec;
    if (!turnOffOnly) {
        bool okNum = false;
        int parsed = lower.toInt(&okNum);
        if (okNum && parsed > 0)
            seconds = parsed;
        if (seconds <= 0)
            seconds = 600;
    }

    g_headlessStop = 0;
    struct sigaction sa{};
    sa.sa_handler = headlessSignalHandler;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    OffscreenEglContext egl;
    std::string err;
    if (!egl.init(&err)) {
        std::fprintf(stderr, "naomi-camera --torch: %s\n", err.c_str());
        return 1;
    }

    HybrisCameraApi api;
    if (!api.load(&err)) {
        std::fprintf(stderr, "naomi-camera --torch: %s\n", err.c_str());
        return 1;
    }

    std::atomic<bool> texDirty{false};
    CameraControlListener listener{};
    listener.context = &texDirty;
    listener.on_msg_error_cb = [](void *) {};
    listener.on_msg_shutter_cb = [](void *) {};
    listener.on_msg_focus_cb = [](void *) {};
    listener.on_msg_zoom_cb = [](void *, int32_t) {};
    listener.on_data_raw_image_cb = [](void *, uint32_t, void *) {};
    listener.on_data_compressed_image_cb = [](void *, uint32_t, void *) {};
    listener.on_preview_frame_cb = [](void *, uint32_t, void *) {};
    listener.on_preview_texture_needs_update_cb = [](void *c) {
        static_cast<std::atomic<bool> *>(c)->store(true, std::memory_order_release);
    };

    CameraControl *ctrl = api.connect_by_id(0, &listener);
    if (!ctrl) {
        std::fprintf(stderr, "naomi-camera --torch: failed to connect back camera 0\n");
        return 1;
    }

    api.set_preview_texture(ctrl, static_cast<int>(egl.textureId()));
    api.set_preview_size(ctrl, kPreviewWidth, kPreviewHeight);
    api.set_preview_callback_mode(ctrl, PREVIEW_CALLBACK_DISABLED);
    api.start_preview(ctrl);

    if (turnOffOnly) {
        if (api.set_flash_mode)
            api.set_flash_mode(ctrl, FLASH_MODE_OFF);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        if (api.set_flash_mode)
            api.set_flash_mode(ctrl, FLASH_MODE_TORCH);

        const int64_t endMs = steadyNowMs() + static_cast<int64_t>(seconds) * 1000;
        while (!g_headlessStop && steadyNowMs() < endMs) {
            if (texDirty.exchange(false, std::memory_order_acq_rel) && api.update_preview_texture)
                api.update_preview_texture(ctrl);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (api.set_flash_mode)
            api.set_flash_mode(ctrl, FLASH_MODE_OFF);
    }

    api.stop_preview(ctrl);
    api.disconnect(ctrl);
    api.delete_ctrl(ctrl);
    egl.shutdown();
    return 0;
}
