#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class VideoRecorder;
struct CameraControl;
struct HybrisCameraApi;

class CameraEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int facing READ facing NOTIFY facingChanged)
    Q_PROPERTY(int cameraCount READ cameraCount NOTIFY cameraCountChanged)
    Q_PROPERTY(QString cameraLabel READ cameraLabel NOTIFY facingChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(int recordingSeconds READ recordingSeconds NOTIFY recordingSecondsChanged)
    Q_PROPERTY(int flashMode READ flashMode NOTIFY flashModeChanged)
    Q_PROPERTY(bool flashSupported READ flashSupported NOTIFY flashSupportedChanged)
    Q_PROPERTY(int resolutionPreset READ resolutionPreset NOTIFY resolutionPresetChanged)
    Q_PROPERTY(QString resolutionLabel READ resolutionLabel NOTIFY resolutionPresetChanged)
    Q_PROPERTY(int zoom READ zoom NOTIFY zoomChanged)
    Q_PROPERTY(int maxZoom READ maxZoom NOTIFY maxZoomChanged)
    Q_PROPERTY(qreal zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(int rotationDegrees READ rotationDegrees NOTIFY rotationDegreesChanged)
    Q_PROPERTY(bool mirrored READ mirrored NOTIFY mirroredChanged)
    Q_PROPERTY(QString lastPhotoPath READ lastPhotoPath NOTIFY lastPhotoChanged)
    Q_PROPERTY(QUrl lastPhotoUrl READ lastPhotoUrl NOTIFY lastPhotoChanged)
    Q_PROPERTY(int photoRevision READ photoRevision NOTIFY lastPhotoChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)

public:
    explicit CameraEngine(QObject *parent = nullptr);
    ~CameraEngine() override;

    int facing() const { return m_facing.load(); }
    int cameraCount() const { return m_cameraCount.load(); }
    QString cameraLabel() const;
    bool ready() const { return m_ready.load(); }
    bool busy() const { return m_busy.load(); }
    bool recording() const { return m_recording.load(); }
    int recordingSeconds() const { return m_recordingSeconds; }
    int flashMode() const { return m_flashMode.load(); }
    bool flashSupported() const { return m_flashSupported.load(); }
    int resolutionPreset() const { return m_resolutionPreset.load(); }
    QString resolutionLabel() const;
    int zoom() const { return m_zoom.load(); }
    int maxZoom() const { return m_maxZoom.load(); }
    qreal zoomFactor() const;
    int rotationDegrees() const { return m_rotationDegrees.load(); }
    bool mirrored() const { return m_mirrored.load(); }
    QString lastPhotoPath() const { return m_lastPhotoPath; }
    QUrl lastPhotoUrl() const;
    int photoRevision() const { return m_photoRevision; }
    QString mode() const { return m_mode; }
    QString errorMessage() const { return m_errorMessage; }
    bool paused() const { return m_paused.load(); }

    QImage latestFrame() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void switchCamera();
    Q_INVOKABLE void setFacing(int facing);
    Q_INVOKABLE void cycleFlashMode();
    Q_INVOKABLE void setFlashMode(int mode);
    Q_INVOKABLE void cycleResolution();
    Q_INVOKABLE void setZoom(int zoom);
    Q_INVOKABLE void setZoomNormalized(qreal normalized);
    Q_INVOKABLE void rotatePreview();
    Q_INVOKABLE void toggleMirror();
    Q_INVOKABLE void setMode(const QString &mode);
    Q_INVOKABLE void focusAt(qreal normX, qreal normY);
    Q_INVOKABLE void takePhoto();
    Q_INVOKABLE void toggleVideo();
    Q_INVOKABLE void openLastPhoto();

    // Headless CLI entrypoints (called before QGuiApplication is created).
    static int runHeadlessSnap(const QString &facing,
                               const QString &outPath,
                               int zoom,
                               const QString &flash,
                               int width,
                               int height);
    static int runHeadlessTorch(const QString &arg, int durationSec);

signals:
    void facingChanged();
    void cameraCountChanged();
    void readyChanged();
    void busyChanged();
    void recordingChanged();
    void recordingSecondsChanged();
    void flashModeChanged();
    void flashSupportedChanged();
    void resolutionPresetChanged();
    void zoomChanged();
    void maxZoomChanged();
    void rotationDegreesChanged();
    void mirroredChanged();
    void lastPhotoChanged();
    void modeChanged();
    void errorMessageChanged();
    void pausedChanged();

    void frameReady();
    void shutterTriggered();
    void photoSaved(const QString &path);
    void videoSaved(const QString &path);
    void focusCompleted();
    void toastMessage(const QString &message);

private:
    void workerLoop();
    void postCommand(std::function<void()> fn);
    void openCameraOnWorker(int cameraId);
    void closeCameraOnWorker();
    void applyPictureSizeOnWorker(int cameraId, int preset);
    void applyFlashOnWorker(int mode);
    void applyZoomOnWorker(int zoom);
    void restartPreviewOnWorker();
    void saveJpegBytes(std::vector<uint8_t> bytes);
    void stopVideoInternal();
    void scanExistingLastPhoto();
    void setError(const QString &msg);

    // libhybris callbacks (may arrive on binder threads)
    static void cbError(void *ctx);
    static void cbShutter(void *ctx);
    static void cbFocus(void *ctx);
    static void cbCompressedImage(void *data, uint32_t size, void *ctx);
    static void cbTextureNeedsUpdate(void *ctx);
    static void cbPreviewFrame(void *data, uint32_t size, void *ctx);

    static QImage nv21ToRgb32Half(const uint8_t *nv21, int srcW, int srcH);

    static constexpr int kPreviewWidth = 1280;
    static constexpr int kPreviewHeight = 720;

    std::thread m_worker;
    std::mutex m_cmdMutex;
    std::condition_variable m_cmdCv;
    std::deque<std::function<void()>> m_cmdQueue;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_texNeedsUpdate{false};

    HybrisCameraApi *m_api = nullptr;
    struct CameraControl *m_ctrl = nullptr;
    uint32_t m_eglTexture = 0;
    std::atomic<bool> m_snapshotPending{false};
    std::atomic<int64_t> m_snapshotDeadlineMs{0};
    std::atomic<bool> m_previewActive{false};

    mutable std::mutex m_frameMutex;
    QImage m_latestFrame;

    std::unique_ptr<VideoRecorder> m_recorder;
    QTimer m_recTimer;
    int m_recordingSeconds = 0;

    std::atomic<int> m_facing{0};
    std::atomic<int> m_cameraCount{2};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_recording{false};
    std::atomic<int> m_flashMode{0};
    std::atomic<bool> m_flashSupported{true};
    std::atomic<int> m_resolutionPreset{0};
    std::atomic<int> m_zoom{0};
    std::atomic<int> m_maxZoom{99};
    std::atomic<int> m_rotationDegrees{0};
    std::atomic<bool> m_mirrored{false};
    std::atomic<bool> m_paused{false};

    QString m_mode = QStringLiteral("photo");
    QString m_lastPhotoPath;
    int m_photoRevision = 0;
    QString m_errorMessage;
};
