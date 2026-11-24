#include "stream_service.h"

#include "gralloc_buffer.h"
#include "nv12_pack.h"
#include "desk_test_input.h"
#include "hard_keyboard.h"
#include "media_keys.h"
#include "stream_input.h"
#include "stream_protocol.h"
#include "stream_screen.h"
#include "venus_encoder.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonArray>
#include <QJsonDocument>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QUrl>
#include <qpa/qplatformscreen.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <list>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

Q_LOGGING_CATEGORY(lcStream, "naomi.stream")

namespace {

constexpr int kInputs = 3;     // буферы кадра у кодера: один пакуется, один сжимается, один в запасе
constexpr int kOutputs = 4;
constexpr int kMaxQueued = 8;  // кадров в очереди к зрителю: больше — связь не успевает, сброс до опорного
constexpr int64_t kOrphanUs = 5000000;   // все пути оборвались: столько ждать зрителя с тем же билетом
constexpr int64_t kForgetUs = 6000000;   // у зрителя нет ни одного пути столько — его экраны «не на связи»
constexpr int64_t kLagKeyUs = 1000000;   // зритель отстал, а стол смотрят другие: опорный ему — в покое или через столько
constexpr int64_t kCursorAtUs = 8000;    // чужой курсор (CursorAt) зрителю — не чаще
constexpr int kPacketsPerTurn = 16;      // сжатых кадров за один круг потока службы — не больше
constexpr int kEmptyMax = 48;            // столько пустых кадров кодера подряд — он сломан: запустить заново
constexpr int64_t kSilentUs = 5000000;   // путь молчит столько — мёртв
constexpr int64_t kDirectJoinUs = 2000000;   // прямое подключение не сказало пропуск за столько — закрыто
// трансляция выключена (Switch 0): тихий путь молчит дольше — мёртв; после «включили» (Switch 1) зритель подключается
// заново, а тихий путь закрывается через kReleaseUs
constexpr int64_t kParkedSilentUs = int64_t(stream::kParkedSilentS) * 1000000;
constexpr int64_t kReleaseUs = 3000000;
// Путь ждёт опорного (подключился, отстал), а кадры зрителю идут другим путём (wantKey): опорный — в покое (нового
// кадра нет kWaitKeyQuietUs, зритель ничего не держит), а покоя нет kWaitKeyMaxUs — всё равно; путь должен подать
// голос после начала ожидания (пинг 10 раз в секунду: мёртвый не просит), и не чаще раза в kWaitKeyEveryUs
constexpr int64_t kWaitKeyQuietUs = 250000;
constexpr int64_t kWaitKeyMaxUs = 5000000;
constexpr int64_t kWaitKeyEveryUs = 2000000;
constexpr int kDeskFps = 90;             // стол — к/с, если зритель не просил иначе (Want)
constexpr int kCleanAfter = 8;           // кадров движения, после которых в покое — чистовой опорный (следы кодера)
constexpr int64_t kBurstGapUs = 100000;  // кадры чаще — одно движение (анимация стола, перетаскивание); тише — конец

int64_t nowUs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// NAOMI_TICK_TRACE=файл — когда пришли такт (k) и пинг (p, по каждому пути) в поток трансляции: мкс CLOCK_MONOTONIC,
// значение, путь (1 — прямой). Пинг зритель шлёт одним и тем же по SSH и напрямую — разность прихода — цена SSH;
// такт против прихода в поток интерфейса (NAOMI_DESK_TRACE, «t») — где теряется ровность (docs/PI.md, «Листание
// ползунком»)
FILE *tickTrace()
{
    static FILE *f = qEnvironmentVariableIsSet("NAOMI_TICK_TRACE") ? fopen(qgetenv("NAOMI_TICK_TRACE").constData(), "w")
                                                                   : nullptr;
    return f;
}

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok ? v : fallback;
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(uint8_t(x >> (8 * i)));
}
void put64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back(uint8_t(x >> (8 * i)));
}
uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t *p) { return get32(p) | uint64_t(get32(p + 4)) << 32; }
float getF(const uint8_t *p)
{
    const uint32_t u = get32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

std::vector<uint8_t> message(uint8_t type, const void *body, size_t n, const std::vector<uint8_t> &head = {})
{
    std::vector<uint8_t> m;
    m.reserve(5 + head.size() + n);
    put32(m, uint32_t(1 + head.size() + n));
    m.push_back(type);
    m.insert(m.end(), head.begin(), head.end());
    const auto *b = static_cast<const uint8_t *>(body);
    m.insert(m.end(), b, b + n);
    return m;
}

// NAL H.264 в Annex B: [начало, конец) каждого (без стартового кода), тип
struct Nal { size_t start, end; int type; };
// тип NAL: H.264 — младшие 5 бит первого байта, HEVC — биты 1…6
std::vector<Nal> nals(const std::vector<uint8_t> &d, bool hevc)
{
    std::vector<Nal> out;
    size_t i = 0;
    auto nextStart = [&](size_t from, size_t &codeLen) -> size_t {
        for (size_t k = from; k + 2 < d.size(); ++k)
            if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) {
                codeLen = 3;
                if (k > 0 && d[k - 1] == 0) {
                    codeLen = 4;
                    return k - 1;
                }
                return k;
            }
        return d.size();
    };
    size_t code = 0;
    i = nextStart(0, code);
    while (i < d.size()) {
        const size_t begin = i + code;
        size_t nextCode = 0;
        const size_t next = nextStart(begin, nextCode);
        if (begin < d.size())
            out.push_back({begin, next, hevc ? (d[begin] >> 1) & 0x3f : d[begin] & 0x1f});
        i = next;
        code = nextCode;
    }
    return out;
}

// когда сработала метка (sync_file), мкс CLOCK_MONOTONIC; 0 — не узнать
int64_t fenceSignalUs(int fd)
{
    if (fd < 0)
        return 0;
    struct { char name[32]; int32_t status; uint32_t flags, num, pad; uint64_t info; } f{};
    struct { char obj[32], drv[32]; int32_t status; uint32_t flags; uint64_t ns; } pts[4]{};
    if (ioctl(fd, 0xC0383E04 /* SYNC_IOC_FILE_INFO */, &f) < 0 || !f.num || f.num > 4)
        return 0;
    f.info = reinterpret_cast<uintptr_t>(pts);
    if (ioctl(fd, 0xC0383E04, &f) < 0)
        return 0;
    uint64_t ns = 0;
    for (uint32_t i = 0; i < f.num; ++i)
        ns = std::max(ns, pts[i].ns);
    return int64_t(ns / 1000);
}

// ---------- путь зрителя: через какой интерфейс (правда — у ядра) ----------
// Зритель приходит через sshd (ssh -W): наш сокет — 127.0.0.1:8478 ← 127.0.0.1:P, где P — порт sshd. На какой
// интерфейс телефона пришло SSH-подключение этого sshd, знает naomid (Net.SshInterface): сокеты sshd читает только
// root. usb0 — кабель, wlan0 — Wi-Fi: значок на столе показывает, по чему картинка идёт на самом деле, а не что
// думает окно. Имя интерфейса нужно и картинке напрямую: она слушает адрес именно этого интерфейса (offerDirect).
// Ответ — 30–40 мс: зовётся своим потоком сразу при подключении (accept, Path::lookup), путь читается, когда
// он пришёл. Раньше звался в Join прямо в потоке трансляции — кадр, что был в кодере, ждал (замер 16 ноября 2026:
// вставили кабель посреди движения — кадр сжимался 49 мс вместо 18, Pi выбросила его и подняла запас на секунды).
uint16_t peerPort(int fd)
{
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    return getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &len) < 0 ? 0 : ntohs(peer.sin_port);
}

std::string sshInterface(uint16_t port)
{
    if (!port)
        return {};
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.naomi.Phone"), QStringLiteral("/org/naomi/Phone/Net"),
                                                    QStringLiteral("org.naomi.Phone.Net"), QStringLiteral("SshInterface"));
    m << uint(port);
    const QDBusMessage r = QDBusConnection::systemBus().call(m, QDBus::Block, 1000);
    return r.type() == QDBusMessage::ReplyMessage ? r.arguments().value(0).toString().toStdString() : std::string();
}

int routeOf(const std::string &iface)
{
    return iface.rfind("usb", 0) == 0 ? stream::RouteCable
         : iface.rfind("wl", 0) == 0 ? stream::RouteWifi : stream::RouteUnknown;
}

// Интерфейс поднят и с несущей. Кабель вынут — у usb0 гаснет несущая (гаджет отключён хозяином), гаджет сброшен —
// usb0 пропадает: пути по кабелю мертвы сразу, а не через 5 с молчания. Имя неизвестно — не судить (true).
bool linkUp(const std::string &iface)
{
    ifreq r{};
    if (iface.empty() || iface.size() >= sizeof r.ifr_name)
        return true;
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return true;
    memcpy(r.ifr_name, iface.c_str(), iface.size());
    const bool up = ioctl(s, SIOCGIFFLAGS, &r) == 0 && (r.ifr_flags & IFF_UP) && (r.ifr_flags & IFF_RUNNING);
    ::close(s);
    return up;
}

// IPv4 Wi-Fi (wlan*, wlp* и другие имена wl*, 0 — нет): зрителю для запасного пути (Address).
in_addr_t wifiAddress()
{
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) < 0)
        return 0;
    in_addr_t a = 0;
    for (ifaddrs *i = list; i && !a; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strncmp(i->ifa_name, "wl", 2))
            a = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr;
    freeifaddrs(list);
    return a;
}

// IPv4 интерфейса (0 — нет)
in_addr_t addressOf(const std::string &iface)
{
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) < 0)
        return 0;
    in_addr_t a = 0;
    for (ifaddrs *i = list; i && !a; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && iface == i->ifa_name)
            a = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr;
    freeifaddrs(list);
    return a;
}

const char *routeName(int route)
{
    return route == stream::RouteCable ? "кабель" : route == stream::RouteWifi ? "Wi-Fi" : "—";
}

} // namespace

// ---------- общее у потоков ----------

struct StreamService::Shared {
    std::mutex m;
    bool want = false;          // зритель есть: снимать кадры
    int source = Phone;         // что смотрит зритель
    int owner = -1;             // чьи буферы выложены (inputs, outputs): источник
    bool buffersReady = false;  // буферы кадра (inputs, outputs) выложены
    bool poolFailed = false;
    VenusEncoder::Queues queues;
    uint64_t generation = 0;   // смена настроек кодера между записью кадра и передачей fd
    int captureIntervalUs = 0;
    bool running = false;       // кодер запущен
    // Стол меняет размер: его буферы кадра — заново, кадры пока не снимать. Счётчик, а не флаг: смен бывает две подряд
    // (главный экран сменился — форма стола от прежнего зрителя, через миллисекунды — от нового), и кадр между ними
    // снимать нельзя: буферы под него выделялись бы по одному размеру, а освобождались следующей сменой — под
    // работающим кодером (21 ноября 2026: кодер отдавал пустые кадры без конца, поток службы зациклился — стол
    // «включён», а ни один экран его не показывает)
    int deskResize = 0;
    int sceneRotation = 0, requestedRotation = 0;   // поворот сцены планшета (поток интерфейса → кодер)
    bool encoderStale = false;  // буферы кадра освобождены под работающим кодером — запустить его заново (поток службы)
    int width[2] = {720, 1920}, height[2] = {1560, 1080};   // кадр потока у каждого источника
    std::vector<VenusEncoder::Buffer> inputs;
    std::vector<VenusEncoder::Buffer> outputs;   // сжатые кадры (вид для процессора — map)
    std::vector<bool> freeSlot;
    struct Job { int slot; int fence; int64_t captureUs; int frameFence; uint64_t tick = 0; uint8_t away = 0; };
    std::deque<Job> jobs;
    struct Sound { int64_t first; int64_t read; std::vector<uint8_t> pcm; };   // read — когда снят; pcm — f32 стерео
    std::deque<Sound> sound;    // звук стола зрителю: захват (AudioCapture) → поток службы
    std::atomic<bool> sendSound{false};   // звук на «Монитор» — куски зрителю (захват идёт и без этого)
    // рядом могут понадобиться декодеры (у Android есть окна, и он не спит) — кодер оставляет им место
    // (StreamService::setDecodersNear, VenusEncoder::sharedOperatingRate)
    std::atomic<bool> decodersNear{false};
    std::string mouse, screen, pad;   // настройки стола — JSON сообщений Mouse, Screen и Pad ("" — стол их ещё не дал)
    uint8_t cursor = stream::CursorArrow;   // форма курсора стола под мышью (Cursor) — с настройками, после Join
    bool cursorDirty = false;               // сменилась — зрителю одна она, по всем путям
    int idleTest = -1;                      // «Тест» без дока — зрителю IdleTest, по всем путям: 255 — как в
                                            // настройках, 0…100 — приглушить до этой доли («луна»); −1 — нет
    int brightnessStep = 0;                 // клавиши яркости — зрителю Brightness, по всем путям (шаги копятся)
    int layoutWant = -1;                    // стол просит раскладку (0 — EN, 1 — RU) — зрителю Layout, по всем путям
    // выход из стола без дела (checkIdleExit): последний ввод зрителя (мышь сдвинулась, кнопка, колесо, клавиша, касание)
    // и последний не тихий кусок звука стола, мкс
    std::atomic<int64_t> lastInputUs{0}, lastSoundUs{0};
    bool settingsDirty = false; // сменились — зрителю по всем путям
    // Экраны (docs/SCREENS.md) — что выбрано на телефоне: поток интерфейса → поток службы (planDirty — сменилось)
    bool planDirty = false;
    bool planEnabled = true;            // стол включён (плитка «трансляция»)
    std::vector<std::string> planOn;    // экраны, на которые он выведен
    std::string planMain;               // главный: форма стола — его
    // обои заставки без дока — экранам, что рисуют её сами (Wallpaper): хэш и JPEG (пусто — обоев нет)
    std::string wallHash;
    std::vector<uint8_t> wallJpeg;
    bool wallDirty = false;
    // яркость экранов, у которых её можно ставить (планшет): экран → сам ли ведёт, уровень % (ScreenLight)
    std::map<std::string, std::pair<bool, int>> lights;
    bool lightsDirty = false;
    // экраны, что надо включить или уже не надо (ScreenWake): зритель, что их несёт, экран, включить ли — поток
    // интерфейса → поток службы
    std::vector<std::tuple<std::string, std::string, bool>> wakes;
    uint64_t drops = 0;         // кадры оболочки, на которые не нашлось свободного буфера
    int64_t lastCaptureUs = 0;
    int wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    void wake()
    {
        const uint64_t one = 1;
        (void)!write(wakeFd, &one, sizeof one);
    }
    ~Shared() { ::close(wakeFd); }
};

// ---------- поток отрисовки ----------

struct StreamService::Gl {
    bool ready = false;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    QSize source;               // кадр источника, пиксели
    int width = 0, height = 0;  // под какой кадр потока выделены буферы
    int rotation = 0;
    GLuint srcTex = 0, srcFbo = 0;   // копия кадра окна телефона (кадр окна нельзя читать шейдером)
    std::vector<GrallocBuffer> in;
    std::vector<GrallocBuffer> out;
    std::vector<GrallocBuffer> inExtra, outExtra;
    VenusEncoder::Queues queues;
    Nv12Packer packer;
};

// Поток, где рисуется источник (телефон — поток отрисовки Qt Quick, стол — поток интерфейса), сразу после его кадра,
// в его контексте GL. Буферы кадра и упаковщик — у каждого источника свои (их контекст); кодеру отдаются буферы того,
// кого смотрят. Другой источник свои держит до конца сеанса — вернуться к нему быстро.
void StreamService::capture(int source)
{
    Shared &s = *m_shared;
    Gl &g = *m_gl[source];
    std::unique_lock lock(s.m);
    if (!s.want) {
        if (g.ready) {
            if (s.owner == source) {
                s.buffersReady = false;
                s.inputs.clear();
                s.outputs.clear();
                s.owner = -1;
            }
            lock.unlock();
            releaseGl(g);
        }
        return;
    }
    if (s.source != source || (source == Desktop && s.deskResize) || s.sceneRotation != s.requestedRotation)
        return;
    const auto queues = s.queues;
    if (!queues.inputs || !queues.outputs || s.poolFailed)
        return;
    const uint64_t generation = s.generation;
    const int w = s.width[source], h = s.height[source];
    const int rotation = source == Phone ? s.sceneRotation : 0;
    QQuickWindow *window = source == Desktop ? m_desktop->window() : m_window.data();
    if (g.ready && (g.width != w || g.height != h || g.rotation != rotation
                    || g.queues.inputs != queues.inputs || g.queues.outputs != queues.outputs
                    || g.queues.inputExtra != queues.inputExtra || g.queues.packet != queues.packet
                    || g.queues.packetExtra != queues.packetExtra)) {
        if (s.running)
            return;
        releaseGl(g);
    }
    if (!g.ready) {
        lock.unlock();
        g.dpy = eglGetCurrentDisplay();
        const auto &L = queues.input;
        // буфер кадра для кодера — RGBA, ширина равна строке: gralloc строку иногда выравнивает (1920 × 1080: 512 → 768
        // пикселей) — упаковщик считает байты по строке буфера и заполняет её всю
        g.in.resize(queues.inputs);
        bool ok = true;
        for (GrallocBuffer &b : g.in)
            ok = ok
                 && grallocAllocateLinear(b, L.size, L.stride / 4,
                                          kGrallocHwRender | kGrallocHwTexture | kGrallocHwVideoEncoder
                                              | kGrallocSwReadRarely)
                 && grallocBindGl(b, g.dpy) && b.stride == g.in[0].stride && b.size >= L.size;
        ok = ok && g.packer.init(w, h, L.stride, L.yScan, L.size, g.in[0].stride, rotation);
        // сжатые кадры: буфер не меньше того, что просит кодер (720 × 1560 — 848 КиБ); в потоке службы gralloc
        // буфер не выделял (13 ноября 2026) — все буферы выделяет поток источника
        g.out.resize(queues.outputs);
        for (GrallocBuffer &b : g.out)
            ok = ok
                 && queues.packet <= INT_MAX && grallocAllocate(b, int(queues.packet), 1, kGrallocBlob,
                                    kGrallocSwReadOften | kGrallocHwVideoEncoder)
                 && grallocMapForCpu(b);
        g.inExtra.resize(queues.inputExtra ? queues.inputs : 0);
        g.outExtra.resize(queues.packetExtra ? queues.outputs : 0);
        for (GrallocBuffer &b : g.inExtra)
            ok = ok && grallocAllocateExtra(b, queues.inputExtra);
        for (GrallocBuffer &b : g.outExtra)
            ok = ok && grallocAllocateExtra(b, queues.packetExtra);
        if (source == Phone) {   // у стола кадр уже текстура — копия не нужна
            glGenTextures(1, &g.srcTex);
            glBindTexture(GL_TEXTURE_2D, g.srcTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, g.source.width(), g.source.height());
            glGenFramebuffers(1, &g.srcFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g.srcFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.srcTex, 0);
            ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_FRAMEBUFFER, QOpenGLContext::currentContext()->defaultFramebufferObject());
        }
        if (!ok) {
            qCWarning(lcStream, "буферы кадра для кодера не выделены (%s, %dx%d: строка %d — у буферов %d, %zu байт "
                      "из %zu) — трансляции не будет", source == Desktop ? "стол" : "телефон", w, h, L.stride,
                      g.in.empty() ? -1 : g.in[0].stride * 4, g.in.empty() ? size_t(0) : g.in[0].size, L.size);
            releaseGl(g);
            lock.lock();
            if (generation == s.generation) {
                s.poolFailed = true;
                s.wake();
            }
            return;
        }
        g.ready = true;
        g.width = w;
        g.height = h;
        g.rotation = rotation;
        g.queues = queues;
        lock.lock();
        if (!s.want || s.source != source || generation != s.generation)
            return;   // пока выделяли, зритель ушёл или переключился — буферы останутся до конца сеанса
    }
    // Пока выделяли буферы, стол снова сменил размер (или смена ждёт своей очереди): эти буферы — под прежний кадр,
    // кодеру их не давать — запустился бы под новый размер с чужими буферами («кодер не запустился» или пустые кадры).
    // Их освободит идущая смена размера (onDesktopSize), следующий кадр выделит новые
    if ((source == Desktop && s.deskResize) || g.width != s.width[source] || g.height != s.height[source])
        return;
    if (s.owner != source) {   // свои буферы — кодеру (впервые или снова к этому источнику)
        const auto buffers = [](const auto &frames, const auto &extra) {
            std::vector<VenusEncoder::Buffer> out;
            for (size_t i = 0; i < frames.size(); ++i) {
                const auto &b = frames[i];
                out.push_back({b.fd, b.size, b.map, extra.empty() ? -1 : extra[i].fd,
                               extra.empty() ? 0 : extra[i].size});
            }
            return out;
        };
        s.inputs = buffers(g.in, g.inExtra);
        s.outputs = buffers(g.out, g.outExtra);
        s.freeSlot.assign(g.in.size(), true);
        s.owner = source;
        s.buffersReady = true;
        s.wake();
    }
    if (!s.running || (s.captureIntervalUs && nowUs() - s.lastCaptureUs < s.captureIntervalUs))
        return;
    int slot = -1;
    for (int i = 0; i < int(s.freeSlot.size()); ++i)
        if (s.freeSlot[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        ++s.drops;
        return;
    }
    s.freeSlot[slot] = false;
    lock.unlock();

    static const auto createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    static const auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    static const auto dupFence =
        reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));

    const int64_t t = nowUs();
    window->beginExternalCommands();
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    // Метка «кадр нарисован» — для журнала: сколько ждём сам кадр, сколько — упаковку. Только по просьбе
    // (NAOMI_STREAM_FENCE=1): ради неё кадр и упаковка уходили видеочипу двумя заданиями (glFlush между ними), и под
    // нагрузкой каждое стояло в его очереди отдельно; без неё кадр стола — одно задание
    static const bool split = qEnvironmentVariableIntValue("NAOMI_STREAM_FENCE") != 0;
    int frameFence = -1;
    if (EGLSyncKHR fs = split && createSync ? createSync(g.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr) : EGL_NO_SYNC_KHR;
        fs != EGL_NO_SYNC_KHR) {
        glFlush();
        frameFence = dupFence(g.dpy, fs);
        destroySync(g.dpy, fs);
    }
    GLuint frame = g.srcTex;
    if (source == Phone) {   // кадр окна → своя текстура, затем — упаковка в буфер кодера
        const GLuint fb0 = QOpenGLContext::currentContext()->defaultFramebufferObject();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.srcFbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, g.source.width(), g.source.height(), 0, 0, g.source.width(), g.source.height(),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        frame = m_desktop->texture();
    }
    g.packer.pack(frame, true, g.in[slot].fbo);   // у кадра окна и цели Qt Quick первая строка — низ
    int fence = -1;
    EGLSyncKHR sync = createSync ? createSync(g.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr) : EGL_NO_SYNC_KHR;
    glFlush();
    if (sync != EGL_NO_SYNC_KHR) {
        fence = dupFence(g.dpy, sync);
        destroySync(g.dpy, sync);
    }
    if (fence < 0)
        glFinish();   // метки нет — ждём видеочип здесь
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fb));
    window->endExternalCommands();

    lock.lock();
    if (!s.running || generation != s.generation) {
        for (int fd : {fence, frameFence})
            if (fd >= 0)
                ::close(fd);
        return;
    }
    // кадр стола по такту зрителя — с его значением (Echo вернёт зрителю: к какому обновлению монитора кадр) и с
    // меткой экрана без дока (Away), если она ждёт кадра
    s.jobs.push_back({slot, fence, t, frameFence, source == Desktop ? std::exchange(m_tickValue, 0) : 0,
                      source == Desktop ? std::exchange(m_awayMark, 0) : uint8_t(0)});
    s.lastCaptureUs = t;
    s.wake();
}

void StreamService::releaseGl(Gl &g)
{
    for (auto *pool : {&g.in, &g.out, &g.inExtra, &g.outExtra}) {
        for (auto &b : *pool)
            grallocFree(b, g.dpy);
        pool->clear();
    }
    g.packer.release();
    if (g.srcFbo)
        glDeleteFramebuffers(1, &g.srcFbo);
    if (g.srcTex)
        glDeleteTextures(1, &g.srcTex);
    g.srcFbo = g.srcTex = 0;
    g.ready = false;
}

// ---------- поток службы: связь и кодер ----------

class StreamService::Worker
{
public:
    Worker(StreamService *service, std::shared_ptr<Shared> shared)
        : m_service(service), m_s(std::move(shared))
    {
        m_bitrate = envInt("NAOMI_STREAM_KBPS", 60000) * 1000;
        m_maxQp = envInt("NAOMI_STREAM_MAXQP", 22);
        // NAOMI_STREAM_CTRL="0xID=число,…" — добавочные настройки кодера (разведка качества, как --ctrl у
        // naomi-venc-bench): ставятся после своих, их и перекрывают
        for (const QString &kv : qEnvironmentVariable("NAOMI_STREAM_CTRL").split(',', Qt::SkipEmptyParts)) {
            const QStringList p = kv.split('=');
            bool okId = false, okValue = false;
            const uint id = p.value(0).trimmed().toUInt(&okId, 0);
            const int value = p.value(1).trimmed().toInt(&okValue, 0);
            if (okId && okValue)
                m_extra.push_back({id, value});
        }
        {
            std::lock_guard lock(m_s->m);
            m_enabled = m_s->planEnabled;
            m_on = m_s->planOn;
            m_main = m_s->planMain;
            m_s->planDirty = false;
            m_lights = m_s->lights;
            m_s->lightsDirty = false;
        }
        m_thread = std::thread([this] { run(); });
    }
    ~Worker()
    {
        m_quit = true;
        m_s->wake();
        m_thread.join();
    }

private:
    // Путь — одно подключение зрителя (свой SSH: по кабелю или по Wi-Fi). Зритель — сеанс с билетом (Join): у него
    // бывает несколько путей сразу, и каждый кадр идёт по всем (зритель берёт пришедший первым, ввод от него тоже
    // приходит по всем — с номером, выполняется один раз): обрыв пути ничего не прерывает. Основной (Viewer::activeFd)
    // — тот, что выбрал зритель (Use): по нему значок пути.
    struct Out { std::vector<uint8_t> data; size_t sent = 0; bool video = false; bool audio = false; };
    struct Path {
        int fd = -1;
        std::future<std::string> lookup;   // интерфейс SSH-подключения ещё ищется (sshInterface) — путь не читается
        int viewer = 0;              // чей он (Viewer::id): сказал Join; 0 — ещё нет
        bool use = false;            // просил в Join: основной сразу
        bool joined = false;         // в сеансе своего зрителя: по нему идут кадры и ввод
        int route = 0;               // stream::Route*: через какой интерфейс пришёл (usb0, wlan0)
        std::string iface;           // он сам (путь через SSH)
        bool direct = false;         // картинка напрямую (DirectJoin): по нему только кадры и Hello, ввод — нет
        int64_t lastIn = nowUs();    // когда от него было слово: зритель пингует каждый путь 10 раз в секунду
        bool needKey = false;        // кадры по нему пропадали — до опорного видео по нему не идёт
        int64_t waitKey = 0;         // ждёт опорного в покое с этой минуты (wantKey; 0 — не ждёт)
        int64_t waitMax = kWaitKeyMaxUs;   // покоя нет столько — опорный всё равно
        int64_t keyAskAt = 0;        // когда просил ему опорный в покое
        // Стол этому зрителю не показывают (выключен или выведен на другие экраны): 1 — путь тихий (Switch 0; дальше
        // только Ping → Pong раз в kParkedPingS, Screens и Wake), 2 — зрителю ушёл Switch 1, он подключится заново,
        // путь закроется в closeAt
        int park = 0;
        int64_t closeAt = 0;
        std::vector<uint8_t> in;
        std::deque<Out> out;
    };
    // Экран — то, на чём показывают стол: монитор приставки, окно Mac, планшет (docs/SCREENS.md)
    // light — яркость ставится со стола; wake — не на связи его включает сам зритель (ScreenWake: телевизор)
    struct Screen { std::string id, name, kind; int w = 0, h = 0; bool light = false, wake = false; };
    // Зритель — приёмник со своим билетом: несёт один или несколько экранов (приставка — монитор и планшет). Сеанс у
    // него идёт (live), пока стол показывает хоть один его экран; иначе его пути тихие
    struct Viewer {
        int id = 0;
        uint64_t token = 0;
        std::string name;                // «pi», «mac» — из Screens
        std::vector<Screen> screens;
        std::string wall;                // какие обои заставки у него уже есть (хэш)
        bool known = false;              // назвал экраны: до того его пути ждут
        // его клавиатура и мышь управляют столом, на чьём бы экране тот ни был (приставка; Screens "input"): сеанс идёт
        // и когда стол показывают только чужие экраны — тогда без кадров, один ввод
        bool input = false;
        bool live = false;               // сеанс идёт
        bool showing = false;            // и стол показывает хоть один его экран: ему идут кадры
        int activeFd = -1;               // основной путь (Use)
        int64_t orphanSince = 0;         // все пути сеанса оборвались — ждём Join с тем же билетом (kOrphanUs)
        int64_t goneSince = 0;           // путей нет вовсе — помним kForgetUs
        std::set<uint32_t> seen;         // номера ввода, уже выполненные (ввод идёт по всем путям)
        // что он просит (Want, Mode, DeskSize); новый сеанс — снова H.264, 90 к/с, экран телефона
        bool hevc = false, audio = false, lock = false;
        bool wanted = false;             // Want в этом сеансе уже был
        int deskFps = kDeskFps, encodeFps = kDeskFps;
        int mode = Phone;
        bool sized = false;
        std::string sizedFor;            // для какого главного экрана названа форма стола (у приставки их два)
        int deskW = 1280, deskH = 720, deskScale = 1500;
        int buttons = 0;                 // кнопки его мыши (Pointer)
        bool touching = false;           // его палец на экране (Touch)
        int64_t cursorSentUs = 0;        // когда ему ушёл чужой курсор (CursorAt)
    };

    void run();
    void accept();
    void acceptDirect();
    void offerDirect(size_t i);                  // DirectAsk: слушать адрес кабеля, пропуск — зрителю (Direct)
    void handleDirect(size_t i, uint8_t type, const uint8_t *p, size_t n);
    void closeDirectListener();
    void sendAddress(int path);                  // адрес Wi-Fi телефона (Address) — по пути, −1 — по всем
    void sendSettings(int path);                 // настройки стола (Mouse, Screen) — по пути, −1 — по всем
    in_addr_t m_wifiAddr = 0;                    // какой адрес Wi-Fi зрителю уже назван
    int64_t m_wifiCheckUs = 0;
    bool readPath(size_t i);                     // false — путь закрыт
    void handle(size_t i, uint8_t type, const uint8_t *p, size_t n);
    // экраны и зрители
    Viewer *viewer(int id);
    Viewer *viewerOf(const Path &p) { return viewer(p.viewer); }
    Viewer *carrier(const std::string &screen);  // кто несёт экран (известный зритель)
    long indexOf(int fd) const;
    int liveCount() const;
    bool screenOn(const std::string &id) const;  // стол включён и выведен на этот экран
    bool shows(const Viewer &v) const;           // стол выведен хоть на один его экран
    bool shouldBeLive(const Viewer &v);
    Viewer *mainViewer();                        // чей экран главный (из идущих сеансов); нет — первый идущий
    void onScreens(size_t i, const uint8_t *p, size_t n);
    void admit(size_t i);                        // путь известного зрителя: в сеанс или тихий
    void joinLive(size_t i);
    void startViewer(Viewer &v);
    void parkViewer(Viewer &v);                  // стол на его экраны больше не выведен: пути — тихие
    void dropViewer(Viewer &v);                  // пропал: сеанс его кончился
    void afterViewerLeft();
    void stopSession();                          // зрителей не осталось: кодер и кадры — прочь
    void reconcile();                            // план или экраны сменились: кому сеанс, кому тихо; главный; Show
    void refresh();                              // главный, общие просьбы (Want, Mode, DeskSize), Show, экраны — интерфейсу
    void applyWants();
    void applySource();
    void applySize();
    void applyNativeRotation();
    void sendShow(int path = -1);                // −1 — всем путям всех зрителей, если сменилось
    void sendWall(Viewer &v, int path);          // обои заставки, если у него не те
    void sendLights(Viewer &v);                  // яркость его экранов (ScreenLight)
    void sendScreenWake(const std::string &viewer, const std::string &screen, bool on);   // «включи экран» тому, кто его несёт
    std::map<std::string, std::pair<bool, int>> m_lights;   // экран → сам ли ведёт яркость, уровень % (копия Shared::lights)
    void postScreens();
    void wake(Viewer &v, const uint8_t *p, size_t n);
    void sendCursorAt(const Viewer &from, double x, double y);
    void park(size_t i);                         // путь — тихий (Switch 0)
    void activate(size_t i);
    void closePath(size_t i);
    void prepareEncoder();
    void startEncoder();
    void hello(int path = -1);                   // −1 — всем путям сеансов, с новым номером; путь — только ему, с тем же
    void switchSource(int source);
    void stopEncoding();                                     // кодер и кадры в пути — прочь (новый размер или источник)
    void resizeDesktop(int logicalWidth, int logicalHeight, int scaleMilli);
    void want(bool hevc, int fps, int encodeFps);
    void onPacket(VenusEncoder::Packet &pkt);
    void send(std::vector<uint8_t> msg, int path);           // служебное: по пути (номер в m_paths)
    void sendAll(const std::vector<uint8_t> &msg);           // по всем путям сеансов (Hello)
    void sendVideo(const std::vector<uint8_t> &msg, bool key, const std::vector<uint8_t> &echo,
                   const std::vector<uint8_t> &away);
    void sendSound();                                        // звук стола (Shared::sound) — путями видео
    bool directJoined(int viewerId) const;                   // у зрителя есть прямой путь: его SSH кабеля видео не везёт
    bool videoPath(const Path &p) const;                     // по пути идёт видео (присоединён, не SSH при прямом)
    bool carries(const Path &p) const;                       // путь везёт зрителю каждый кадр сейчас
    // путь — с опорного; true — опорный просили сейчас. lagging — путь отстал (а не новый)
    bool wantKey(Path &p, bool lagging = false);
    void flush(Path &path);
    template <typename F> void post(F &&f) { QMetaObject::invokeMethod(m_service, std::forward<F>(f), Qt::QueuedConnection); }

    StreamService *m_service;
    std::shared_ptr<Shared> m_s;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    // план экранов (копия Shared::plan*): стол включён, на какие экраны выведен, главный
    bool m_enabled = true;
    std::vector<std::string> m_on;
    std::string m_main;
    int m_listen = -1;
    // картинка напрямую (docs/STREAM.md, «Картинка напрямую»): слушает адрес кабеля, пока сеанс; пропуск — одноразовый
    int m_directListen = -1;
    in_addr_t m_directAddr = 0;
    std::string m_directIface;                   // интерфейс, где слушает (usb0) — у прямого пути он же
    int m_directViewer = 0;                      // кому выдан пропуск
    int64_t m_linkCheckUs = 0;                   // когда проверять, не вынут ли кабель (linkUp)
    std::array<uint8_t, 16> m_directPass{};
    bool m_directPassValid = false;
    std::vector<Path> m_paths;                   // подключения: и сеансов, и тихие, и ещё не сказавшие, чьи
    std::list<Viewer> m_viewers;
    int m_nextViewer = 1;
    bool m_session = false;                      // идёт хоть один сеанс: кадры снимаются
    int m_mainViewer = 0;                        // зритель, чей экран главный (Viewer::id; 0 — нет)
    bool m_mainPending = false;                  // он на связи, но сеанс его ещё не начался (подключается)
    std::string m_mainScreen;                    // главный экран: выбранный, а нет его на связи — первый из выведенных
    int m_tickViewer = 0;                        // чей такт взят последним — ему Echo
    std::string m_shown;                         // последний Show (JSON): сменился — всем
    QByteArray m_screensPosted;                  // что знает интерфейс (postScreens)
    int m_bitrate = 60'000'000;
    int m_maxQp = 22;
    // общее из просьб зрителей (applyWants): HEVC — если просит хоть один; частота стола — главного
    bool m_hevc = false;
    int m_deskFps = kDeskFps;
    bool m_audioViewer = false;                  // хоть один зритель играет звук (Want audio)
    int m_statSound = 0, m_statSoundOld = 0;     // за секунду: кусков звука (и не посланных — старых), дольше всего
                                                 // ждал отправки и от отсчёта
    double m_statSoundWait = 0, m_statSoundAge = 0;
    int m_encodeFps = kDeskFps;                  // частота, которую кодеру называют (его частоты — под неё)
    QSize m_deskLogical{1280, 720};              // стол: точки и масштаб (DeskSize)
    int m_deskScaleMilli = 1500;
    bool m_encHevc = false;                      // чем сжимает идущий кодер
    std::vector<std::pair<uint32_t, int32_t>> m_extra;   // NAOMI_STREAM_CTRL
    VenusEncoder m_enc;
    VenusEncoder::Settings m_encSettings;
    bool m_encoding = false;
    int m_emptyRun = 0;            // пустых кадров кодера подряд
    int m_encRestarts = 0;         // сколько раз кодер запущен заново из-за них за последние 10 с
    int64_t m_encRestartUs = 0;
    uint8_t m_brightnessSeq = 0;   // номер просьбы Brightness
    int operatingRate(int width, int height, bool desk) const;
    void applyOperatingRate();
    void retryEncoder(const char *why);
    int m_encWidth = 0, m_encHeight = 0;         // кадр идущего кодера
    bool m_encDesk = false;
    int m_encRate = 0;                           // его «скорость работы»
    std::vector<Shared::Job> m_pending;          // ждут видеочип
    std::map<int64_t, int64_t> m_inEncoder;      // метка захвата → когда отдан кодеру
    std::map<int64_t, uint64_t> m_tickOf;        // метка захвата → такт зрителя, по которому кадр (Echo)
    std::map<int64_t, uint8_t> m_awayOf;         // метка захвата → метка экрана без дока (Away: фаза + 1)
    bool m_lockViewer = false;                   // хоть один зритель — с экраном без дока (Want lock)
    // номер Hello: по всем путям, зритель берёт один раз — не новее взятого значит копию или опоздавший по
    // медленному пути (Wi-Fi после кабеля). Начало — секунды с включения × 16: у перезапущенной оболочки номер
    // больше прежнего (Hello реже 16 в секунду), int хватит на 4 года работы без перезагрузки
    uint32_t m_helloGen = uint32_t(nowUs() / 1000000 * 16);
    std::vector<uint8_t> m_config;               // SPS и PPS (у HEVC и VPS): перед каждым опорным кадром
    bool m_refreshed = true;                     // покой: кадр повторён для чёткости
    bool m_refreshPending = false;               // свой кадр (повтор, чистовой) попрошен, ещё не пришёл
    int m_movedFrames = 0;                       // кадров движения с прошлого чистового опорного
    int m_burst = 0;                             // кадров подряд, без перерыва больше kBurstGapUs
    int64_t m_wheelUs = 0, m_keyUs = 0;          // последняя прокрутка и клавиша зрителей
    int64_t m_lastSeen = 0;
    // за секунду
    int64_t m_statFrom = 0;
    int m_statFrames = 0;
    size_t m_statBytes = 0;
    std::vector<double> m_statEncode;   // от кадра оболочки до сжатого
    std::vector<double> m_statGpu;      // от кадра оболочки до конца упаковки (fence)
    std::vector<double> m_statVpu;      // в кодере
    std::vector<double> m_statFrame;    // от конца кадра в оболочке до кадра, нарисованного видеочипом
    std::vector<double> m_statPack;     // упаковка на видеочипе
};

void StreamService::Worker::run()
{
    pthread_setname_np(pthread_self(), "naomi-stream");
    m_listen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const int one = 1;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(stream::kPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // только этот телефон: зритель приходит через SSH
    if (bind(m_listen, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0 || listen(m_listen, 4) < 0) {
        qCWarning(lcStream, "порт %d: %s — трансляции не будет", stream::kPort, strerror(errno));
        return;
    }
    qCInfo(lcStream, "ждёт зрителя на 127.0.0.1:%d", stream::kPort);
    m_statFrom = nowUs();

    while (!m_quit) {
        std::vector<pollfd> fds;
        fds.push_back({m_s->wakeFd, POLLIN, 0});
        fds.push_back({m_listen, POLLIN, 0});
        fds.push_back({m_directListen, POLLIN, 0});   // −1 — poll пропускает
        const size_t pathAt = fds.size(), pathCount = m_paths.size();
        for (const Path &p : m_paths)
            fds.push_back({p.fd, short((p.lookup.valid() ? 0 : POLLIN) | (p.out.empty() ? 0 : POLLOUT)), 0});
        if (m_encoding)
            fds.push_back({m_enc.pollFd(), POLLIN | POLLOUT, 0});
        for (const auto &j : m_pending)
            fds.push_back({j.fence, POLLIN, 0});
        // без зрителя — спит, пока не подключится (в покое оболочка не тратит процессор); со зрителем — 10 раз
        // в секунду: счёт и повтор кадра в покое. Трансляция выключена и зрители ждут на тихих путях — раз в
        // kParkedPingS: не умер ли тихий путь (молчит kParkedSilentUs), не вынут ли кабель
        const bool busy = m_session
                          || std::any_of(m_paths.begin(), m_paths.end(), [](const Path &p) { return p.park != 1; })
                          || std::any_of(m_viewers.begin(), m_viewers.end(), [](const Viewer &v) { return v.goneSince; });
        poll(fds.data(), fds.size(), busy ? 100 : m_paths.empty() ? -1 : stream::kParkedPingS * 1000);

        if (fds[0].revents) {
            uint64_t n;
            (void)!read(m_s->wakeFd, &n, sizeof n);
        }
        sendSound();
        for (Path &p : m_paths)   // интерфейс нашёлся — путь читается со следующего круга
            if (p.lookup.valid() && p.lookup.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                p.iface = p.lookup.get();
                p.route = routeOf(p.iface);
            }
        // план экранов сменился (поток интерфейса): кому сеанс, кому тихо; обои заставки сменились — кто рисует её сам
        {
            std::unique_lock lock(m_s->m);
            const bool plan = std::exchange(m_s->planDirty, false), wall = std::exchange(m_s->wallDirty, false);
            const bool lights = std::exchange(m_s->lightsDirty, false);
            if (lights)
                m_lights = m_s->lights;
            const std::vector<std::tuple<std::string, std::string, bool>> wakes = std::exchange(m_s->wakes, {});
            if (plan) {
                m_enabled = m_s->planEnabled;
                m_on = m_s->planOn;
                m_main = m_s->planMain;
            }
            lock.unlock();
            if (plan)
                reconcile();
            if (wall)
                for (Viewer &v : m_viewers)
                    if (v.known)
                        sendWall(v, -1);
            if (lights)
                for (Viewer &v : m_viewers)
                    if (v.known)
                        sendLights(v);
            for (const auto &[viewer, screen, on] : wakes)
                sendScreenWake(viewer, screen, on);
        }
        applyNativeRotation();
        applyOperatingRate();
        // пути — по номеру сокета: сообщение одного пути может закрыть другие (Join нового зрителя), номера в списке
        // при этом сдвигаются
        std::vector<std::pair<int, short>> events;
        for (size_t k = pathAt; k < pathAt + pathCount; ++k)
            if (fds[k].revents)
                events.push_back({fds[k].fd, fds[k].revents});
        for (const auto &[fd, ev] : events) {
            long k = indexOf(fd);
            if (k < 0)
                continue;
            if (ev & (POLLERR | POLLHUP)) {
                closePath(size_t(k));
                continue;
            }
            if ((ev & POLLIN) && !readPath(size_t(k)))
                continue;
            k = indexOf(fd);
            if (k >= 0 && (ev & POLLOUT))
                flush(m_paths[size_t(k)]);
        }
        if (fds[1].revents & POLLIN)
            accept();
        if (fds[2].revents & POLLIN)
            acceptDirect();
        // Кабель вынут (у usb0 погасла несущая или его нет) — пути по нему закрыть сразу, 4 раза в секунду проверка
        if (nowUs() - m_linkCheckUs > 250000) {
            m_linkCheckUs = nowUs();
            for (size_t k = m_paths.size(); k-- > 0;)
                if ((m_paths[k].joined || m_paths[k].park || m_paths[k].viewer) && m_paths[k].route == stream::RouteCable
                    && !linkUp(m_paths[k].iface)) {
                    qCInfo(lcStream, "кабель вынут (%s) — путь %s закрыт", m_paths[k].iface.c_str(),
                           m_paths[k].direct ? "напрямую" : "кабель");
                    closePath(k);
                }
        }
        // Путь молчит 5 с — мёртв (TCP по нему sshd заметит только через часы). Закрыть: видео — по живому, а
        // мёртвые не копятся
        for (size_t k = m_paths.size(); k-- > 0;)
            if (m_paths[k].joined && nowUs() - m_paths[k].lastIn > kSilentUs) {
                qCInfo(lcStream, "путь %s молчит — закрыт", m_paths[k].direct ? "напрямую" : routeName(m_paths[k].route));
                closePath(k);
            } else if (m_paths[k].direct && !m_paths[k].joined && nowUs() - m_paths[k].lastIn > kDirectJoinUs) {
                qCInfo(lcStream, "прямое подключение без пропуска — закрыто");
                closePath(k);
            } else if (m_paths[k].park == 1 && nowUs() - m_paths[k].lastIn > kParkedSilentUs) {
                qCInfo(lcStream, "тихий путь %s молчит — закрыт", routeName(m_paths[k].route));
                closePath(k);
            } else if (m_paths[k].park == 2 && nowUs() > m_paths[k].closeAt) {
                closePath(k);   // зритель уже подключился заново (или подключится): тихий путь больше не нужен
            } else if (!m_paths[k].joined && !m_paths[k].park && !m_paths[k].direct && !m_paths[k].lookup.valid()
                       && nowUs() - m_paths[k].lastIn > 2 * kSilentUs) {
                closePath(k);   // подключился и не сказал, чей он
            }
        // Зрители: все пути сеанса оборвались, а он не вернулся с тем же билетом — сеанс его кончился; путей нет вовсе
        // kForgetUs — его экраны больше не на связи
        {
            bool changed = false;
            for (auto it = m_viewers.begin(); it != m_viewers.end();) {
                Viewer &v = *it;
                if (v.live && v.activeFd < 0 && v.orphanSince && nowUs() - v.orphanSince > kOrphanUs) {
                    qCInfo(lcStream, "зритель %s не вернулся", v.name.c_str());
                    dropViewer(v);
                    changed = true;
                }
                const int id = v.id;
                if (!v.live && v.goneSince && nowUs() - v.goneSince > kForgetUs
                    && std::none_of(m_paths.begin(), m_paths.end(), [id](const Path &p) { return p.viewer == id; })) {
                    if (v.known)
                        qCInfo(lcStream, "зритель %s ушёл — его экраны не на связи", v.name.c_str());
                    it = m_viewers.erase(it);
                    changed = true;
                    continue;
                }
                ++it;
            }
            if (changed)
                reconcile();
        }
        // настройки стола сменились (окно «Настройки») — зрителям; форма курсора — одна, сразу
        {
            std::unique_lock lock(m_s->m);
            const bool dirty = m_s->settingsDirty, cursorDirty = m_s->cursorDirty;
            const int idleTest = m_s->idleTest;
            const uint8_t cursor = m_s->cursor;
            const int layoutWant = m_s->layoutWant, brightnessStep = m_s->brightnessStep;
            m_s->brightnessStep = 0;
            m_s->cursorDirty = false;
            m_s->idleTest = -1;
            m_s->layoutWant = -1;
            lock.unlock();
            if (idleTest >= 0) {
                const uint8_t level = uint8_t(idleTest);
                sendAll(message(stream::IdleTest, &level, idleTest <= 100 ? 1 : 0));
            }
            if (brightnessStep) {
                const uint8_t b[2] = {++m_brightnessSeq, uint8_t(int8_t(std::clamp(brightnessStep, -10, 10)))};
                sendAll(message(stream::Brightness, b, 2));
            }
            if (layoutWant >= 0) {
                const uint8_t ru = uint8_t(layoutWant);
                sendAll(message(stream::Layout, &ru, 1));
            }
            if (dirty)
                sendSettings(-1);
            else if (cursorDirty)
                sendAll(message(stream::Cursor, &cursor, 1));
        }
        if (!m_session)
            continue;
        // адрес Wi-Fi сменился (подключился, сменил сеть) — зрителю, раз в 2 с проверка
        if (nowUs() - m_wifiCheckUs > 2000000) {
            m_wifiCheckUs = nowUs();
            if (wifiAddress() != m_wifiAddr)
                sendAddress(-1);
        }

        // Сначала очередь драйвера, затем выделение dma-buf в потоке GL и запуск кодера.
        {
            std::unique_lock lock(m_s->m);
            const bool start = m_s->buffersReady && !m_encoding;
            const bool failed = m_s->poolFailed;
            for (auto &j : m_s->jobs)
                m_pending.push_back(j);
            m_s->jobs.clear();
            lock.unlock();
            if (failed)
                retryEncoder("не получил буферы");
            else if (!m_enc.queues().inputs)
                prepareEncoder();
            else if (start)
                startEncoder();
        }
        // видеочип закончил упаковку — кадр в кодер
        for (size_t i = 0; i < m_pending.size();) {
            const auto &j = m_pending[i];
            bool ready = j.fence < 0;
            if (!ready) {
                pollfd p{j.fence, POLLIN, 0};
                ready = poll(&p, 1, 0) > 0;
            }
            if (!ready) {
                ++i;
                continue;
            }
            const int64_t frameDone = fenceSignalUs(j.frameFence), packDone = fenceSignalUs(j.fence);
            if (j.fence >= 0)
                ::close(j.fence);
            if (j.frameFence >= 0)
                ::close(j.frameFence);
            const int64_t queued = nowUs();
            if (m_encoding && j.away == 1 + stream::AwayFinal)
                m_enc.requestKeyFrame();   // последний кадр ухода — опорный: зритель запомнит его как обои
            if (m_encoding) {
                if (!m_enc.encode(j.slot, j.captureUs)) {
                    m_pending.erase(m_pending.begin() + long(i));
                    retryEncoder("не принял кадр");
                    break;
                }
                m_inEncoder[j.captureUs] = queued;
                if (j.tick)
                    m_tickOf[j.captureUs] = j.tick;
                if (j.away)
                    m_awayOf[j.captureUs] = j.away;
                m_statGpu.push_back((queued - j.captureUs) / 1000.0);
                if (frameDone > 0 && packDone > 0) {
                    m_statFrame.push_back((frameDone - j.captureUs) / 1000.0);
                    m_statPack.push_back((packDone - frameDone) / 1000.0);
                }
            }
            m_pending.erase(m_pending.begin() + long(i));
        }
        // буферы кадра освободили под работающим кодером (onDesktopSize) — кодер заново, с новыми
        {
            std::unique_lock lock(m_s->m);
            const bool stale = std::exchange(m_s->encoderStale, false);
            lock.unlock();
            if (stale && m_encoding) {
                qCWarning(lcStream, "буферы кадра сменились под кодером — кодер заново");
                stopEncoding();
                hello();
            }
        }
        if (m_encoding) {
            // Не больше kPacketsPerTurn за круг, и пустые — на счёт: кодер, которому достались не те буферы, отдаёт
            // пустой кадр на каждый возвращённый буфер без конца — поток службы крутился в этом цикле и не делал больше
            // ничего (ни план экранов, ни ввод, ни пинги). Пустых подряд kEmptyMax — кодер сломан: заново
            VenusEncoder::Packet pkt;
            int taken = 0;
            while (taken < kPacketsPerTurn && m_enc.takePacket(pkt)) {
                ++taken;
                if (pkt.data.empty()) {
                    ++m_emptyRun;
                } else {
                    m_emptyRun = 0;
                    onPacket(pkt);
                }
            }
            if (m_emptyRun >= kEmptyMax) {
                m_emptyRun = 0;
                retryEncoder("отдаёт пустые кадры");
                continue;
            }
            std::vector<int> freed;
            for (int i; (i = m_enc.takeFreeInput()) >= 0;)
                freed.push_back(i);
            if (!freed.empty()) {
                std::lock_guard lock(m_s->m);
                for (int i : freed)
                    if (i < int(m_s->freeSlot.size()))
                        m_s->freeSlot[i] = true;
            }
        }
        const int64_t now = nowUs();
        // Покой после движения: ещё один кадр той же картинки — кодер дотягивает её чёткость (в движении кадры
        // сжаты грубее, а в покое оболочка новых не рисует). Свой повтор — не движение: следующего не просим.
        // Было движение (кадров ≥ kCleanAfter: окно тащили, листали) и потом тихо 0,5 с — чистовой опорный кадр:
        // кодер в движении помечает куски «как в прошлом, со сдвигом» без поправки, у тонкого текста на белом
        // остаются следы, а в покое он эти куски считает неизменными и не трогает — следы стояли (замер 15 ноября
        // 2026, docs/PI.md, «Следы при перетаскивании»). Опорный — весь кадр заново, без ссылок на прошлые; он
        // дорогой (2400 × 1350 — ~390 КБ, ~95 мс до экрана), поэтому не сразу: «навёл — схватил» его не ждёт.
        // Кроме конца анимации стола (меню, окно появилось, свернулось — 140–180 мс, кадр за кадром): после неё
        // зритель ничего не держит (кнопки, пальца), не крутит колесо и не печатает — опорный через 0,1 с. Иначе
        // тающее меню 0,6 с стояло «рассыпанным» с синим отливом: тонкие перемены прозрачного кодер в P-кадрах
        // не передаёт, а повтор той же картинки он пропускает целиком при любом потолке QP (замер 15 ноября 2026,
        // docs/PI.md, «Анимации и след прозрачного»). Перетаскивание и прокрутка ждут, как прежде, 0,5 с.
        int64_t last;
        {
            std::lock_guard lock(m_s->m);
            last = m_s->lastCaptureUs;
        }
        if (last != m_lastSeen) {
            if (!std::exchange(m_refreshPending, false)) {
                m_burst = last - m_lastSeen > kBurstGapUs ? 1 : m_burst + 1;
                m_refreshed = false;
                ++m_movedFrames;
            }
            m_lastSeen = last;
        }
        const bool held = std::any_of(m_viewers.begin(), m_viewers.end(),
                                      [](const Viewer &v) { return v.live && (v.buttons || v.touching); });
        const bool calm = !held && now - m_wheelUs > 1000000 && now - m_keyUs > 1000000;
        if (m_encoding && !m_refreshed && m_burst >= kCleanAfter && calm && now - last > kBurstGapUs) {
            m_burst = m_movedFrames = 0;
            m_refreshed = m_refreshPending = true;
            m_enc.requestKeyFrame();
            post([svc = m_service] { svc->requestFrame(); });
        } else if (m_encoding && !m_refreshed && now - last > 150000) {
            m_refreshed = m_refreshPending = true;
            post([svc = m_service] { svc->requestFrame(); });
        } else if (m_encoding && m_refreshed && !m_refreshPending && m_movedFrames >= kCleanAfter
                   && now - last > 500000) {
            m_movedFrames = 0;
            m_refreshPending = true;
            m_enc.requestKeyFrame();
            post([svc = m_service] { svc->requestFrame(); });
        }
        // Путь ждёт опорного в покое (wantKey: подключился или отстал, а кадры зрителю идут другим путём) — покой:
        // нового кадра нет kWaitKeyQuietUs и зритель ничего не держит; он же — чистовой опорный (следующего не
        // нужно). Покоя нет kWaitKeyMaxUs (видео в браузере) — опорный всё равно, путь не ждёт без конца.
        const bool quiet = calm && now - last > kWaitKeyQuietUs;
        bool askKey = false;
        for (Path &p : m_paths)
            if (const Viewer *v = viewerOf(p); !v || !v->showing)
                continue;   // кадры ему не идут — и опорный ни к чему
            else if (videoPath(p) && p.needKey && p.waitKey && p.lastIn > p.waitKey && now - p.keyAskAt > kWaitKeyEveryUs
                && (quiet || now - p.waitKey > p.waitMax)) {
                p.keyAskAt = now;
                askKey = true;
                qCInfo(lcStream, "путь %s — опорный %s", p.direct ? "напрямую" : routeName(p.route),
                       quiet ? "в покое" : "без покоя: ждал");
            }
        if (m_encoding && askKey) {
            m_enc.requestKeyFrame();
            if (quiet) {
                m_refreshed = m_refreshPending = true;
                m_burst = m_movedFrames = 0;
                post([svc = m_service] { svc->requestFrame(); });
            }
        }
        if (now - m_statFrom >= 1000000) {
            const double sec = (now - m_statFrom) / 1e6;
            auto med = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v.empty() ? 0.0 : v[v.size() / 2];
            };
            const double median = med(std::move(m_statEncode));
            const double fps = m_statFrames / sec, mbps = m_statBytes * 8 / sec / 1e6;
            // как naomi-frames: строка в журнал раз в секунду, пока идут кадры
            if (m_statFrames) {
                uint64_t drops;
                {
                    std::lock_guard lock(m_s->m);
                    drops = m_s->drops;
                }
                // части «кадр» и «упаковка» — только с NAOMI_STREAM_FENCE=1 (capture)
                const QString parts = m_statFrame.empty() ? QString()
                    : QStringLiteral(": кадр %1, упаковка %2").arg(med(std::move(m_statFrame)), 0, 'f', 1)
                                                               .arg(med(std::move(m_statPack)), 0, 'f', 1);
                qCInfo(lcStream, "кадров %d, %.2f Мбит/с; от кадра оболочки до сжатого %.1f мс (видеочип %.1f%s; кодер "
                       "%.1f); пропущено без буфера: %llu", m_statFrames, mbps, median, med(std::move(m_statGpu)),
                       qPrintable(parts), med(std::move(m_statVpu)), (unsigned long long)drops);
            }
            if (m_statSound || m_statSoundOld)   // звук стола: сколько кусков ушло, сколько старых — нет, дольше всего ждал
                qCInfo(lcStream, "звук: кусков %d (старых, не посланы: %d), ждал отправки до %.1f мс, от первого отсчёта "
                       "до отправки до %.1f мс", m_statSound, m_statSoundOld, m_statSoundWait, m_statSoundAge);
            m_statSound = m_statSoundOld = 0;
            m_statSoundWait = m_statSoundAge = 0;
            m_statGpu.clear();
            m_statVpu.clear();
            m_statFrame.clear();
            m_statPack.clear();
            post([svc = m_service, fps, mbps, median] { svc->onStats(fps, mbps, median); });
            m_statFrom = now;
            m_statFrames = 0;
            m_statBytes = 0;
        }
    }
    stopSession();
    for (const Path &p : m_paths)
        ::close(p.fd);
    m_paths.clear();
    ::close(m_listen);
}

void StreamService::Worker::accept()
{
    const int c = ::accept4(m_listen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0)
        return;
    const int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Неотправленного в ядре — не больше 64 КБ: очередь кадров — у нас (Path::out), и звук её обгоняет; иначе ядро
    // забирало опорный кадр целиком, и звук ждал за ним десятки миллисекунд (docs/PI.md, «Звук на мониторе»)
    const int lowat = 64 * 1024;
    setsockopt(c, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof lowat);
    Path p;
    p.fd = c;
    // через какой интерфейс — у naomid, своим потоком: поток трансляции не ждёт (кадры идут); читать путь —
    // когда ответ придёт, чей он — скажет первым сообщением (Join)
    p.lookup = std::async(std::launch::async, [s = m_s, port = peerPort(c)] {
        std::string iface = sshInterface(port);
        s->wake();
        return iface;
    });
    m_paths.push_back(std::move(p));
}

void StreamService::Worker::sendAddress(int path)
{
    m_wifiAddr = wifiAddress();
    std::vector<uint8_t> body(4);
    memcpy(body.data(), &m_wifiAddr, 4);
    const std::vector<uint8_t> msg = message(stream::Address, body.data(), body.size());
    if (path < 0)
        sendAll(msg);
    else
        send(msg, path);
}

void StreamService::Worker::sendSettings(int path)
{
    std::unique_lock lock(m_s->m);
    if (path < 0)
        m_s->settingsDirty = false;
    const std::pair<stream::Type, std::string> all[] = {{stream::Mouse, m_s->mouse}, {stream::Screen, m_s->screen},
                                                        {stream::Pad, m_s->pad},
                                                        {stream::Cursor, std::string(1, char(m_s->cursor))}};
    lock.unlock();
    for (const auto &[type, j] : all) {
        if (j.empty())
            continue;
        const std::vector<uint8_t> msg = message(type, j.data(), j.size());
        // тихому пути — только Screen: где часы его заставки без дока
        if (path >= 0) {
            if (!m_paths[size_t(path)].park || type == stream::Screen)
                send(msg, path);
            continue;
        }
        sendAll(msg);
        if (type == stream::Screen)
            for (size_t k = 0; k < m_paths.size(); ++k)
                if (m_paths[k].park == 1)
                    send(msg, int(k));
    }
}

// ---------- картинка напрямую (docs/STREAM.md, «Картинка напрямую») ----------
// Приставке на Pi SSH на больших кадрах даёт ~80 Мбит/с из 330 голого кабеля USB 2: опорный кадр 390 КБ — 34–40 мс
// против ~10. Зритель, пришедший через SSH по кабелю, просит DirectAsk — телефон слушает адрес этого интерфейса
// (не Wi-Fi, не все адреса) и отдаёт по SSH одноразовый пропуск в 16 случайных байт. Прямое подключение без него
// закрывается через 2 с и сеанса не трогает (чужой не выбьет зрителя, как новый Join); с ним — путь только для
// картинки: Video, Echo, Hello туда, а ввод с него не выполняется — клавиши и мышь только через SSH.
void StreamService::Worker::offerDirect(size_t i)
{
    Viewer *v = viewerOf(m_paths[i]);
    if (!v || !v->live || !m_paths[i].joined || m_paths[i].route != stream::RouteCable)
        return;   // Wi-Fi и прочее — только через SSH
    // просят снова — у зрителя прямого нет (оборвался): свой конец закрыть сразу, кадры — по SSH с опорного, а не
    // в мёртвый путь, пока тот не замолчит (kSilentUs)
    for (size_t k = m_paths.size(); k-- > 0;)
        if (m_paths[k].direct && m_paths[k].viewer == v->id) {
            closePath(k);
            if (k < i)
                --i;
        }
    m_directViewer = v->id;
    const Path &path = m_paths[i];
    const in_addr_t addr = addressOf(path.iface);
    if (!addr) {
        qCInfo(lcStream, "картинка напрямую: у %s нет адреса", path.iface.c_str());
        return;
    }
    if (m_directListen >= 0 && m_directAddr != addr)
        closeDirectListener();
    if (m_directListen < 0) {
        m_directListen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int one = 1;
        setsockopt(m_directListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(stream::kDirectPort);
        a.sin_addr.s_addr = addr;
        if (bind(m_directListen, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0 || listen(m_directListen, 2) < 0) {
            qCWarning(lcStream, "картинка напрямую: порт %d: %s", stream::kDirectPort, strerror(errno));
            closeDirectListener();
            return;
        }
        m_directAddr = addr;
    }
    m_directIface = path.iface;
    if (getrandom(m_directPass.data(), m_directPass.size(), 0) != ssize_t(m_directPass.size()))
        return;
    m_directPassValid = true;
    std::vector<uint8_t> body(4);
    memcpy(body.data(), &addr, 4);   // как в сети: байты адреса по порядку
    body.push_back(uint8_t(stream::kDirectPort));
    body.push_back(uint8_t(stream::kDirectPort >> 8));
    body.insert(body.end(), m_directPass.begin(), m_directPass.end());
    send(message(stream::Direct, body.data(), body.size()), int(i));
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, text, sizeof text);
    qCInfo(lcStream, "картинка напрямую: жду на %s:%d (%s)", text, stream::kDirectPort, path.iface.c_str());
}

void StreamService::Worker::acceptDirect()
{
    const int c = ::accept4(m_directListen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0)
        return;
    const int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Темп — не выше 300 Мбит/с: всплеск на полной скорости кабеля (~330) теряет пакеты — 3–7 повторов на 100
    // всплесков по 320 КБ, а потеря в хвосте кадра — ожидание повтора TCP 200 мс и больше (кадр шёл 212 и 657 мс).
    // С потолком — 0 повторов на 300 всплесках, худший 15–20 мс, медиана 9,7 мс против 8,6 (docs/STREAM.md)
    const unsigned pacing = 300'000'000 / 8;
    setsockopt(c, SOL_SOCKET, SO_MAX_PACING_RATE, &pacing, sizeof pacing);
    // неотправленного в ядре — до 128 КБ (3,5 мс провода): звук обгоняет очередь кадров у нас, а не ждёт в ядре
    const int lowat = 128 * 1024;
    setsockopt(c, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof lowat);
    Path p;
    p.fd = c;
    p.direct = true;   // пропуск — первым словом (DirectJoin), иначе закрыт через kDirectJoinUs
    m_paths.push_back(std::move(p));
}

void StreamService::Worker::handleDirect(size_t i, uint8_t type, const uint8_t *p, size_t n)
{
    if (!m_paths[i].joined) {
        uint8_t diff = 1;
        Viewer *v = viewer(m_directViewer);
        if (type == stream::DirectJoin && n >= 16 && v && v->live && m_directPassValid) {
            diff = 0;
            for (size_t k = 0; k < 16; ++k)
                diff |= p[k] ^ m_directPass[k];
        }
        if (diff) {
            qCInfo(lcStream, "прямое подключение — пропуск не тот, закрыто");
            closePath(i);
            return;
        }
        m_directPassValid = false;   // одноразовый
        const int fd = m_paths[i].fd;
        for (size_t k = m_paths.size(); k-- > 0;)   // прямой путь у зрителя — один: прошлый прочь
            if (m_paths[k].direct && m_paths[k].viewer == v->id && m_paths[k].fd != fd)
                closePath(k);
        Path &d = *std::find_if(m_paths.begin(), m_paths.end(), [fd](const Path &x) { return x.fd == fd; });
        d.viewer = v->id;
        d.joined = true;
        d.route = stream::RouteCable;
        d.iface = m_directIface;
        // этому пути кадров не было — с опорного; по SSH кадры с этой минуты не идут. Идут копии по Wi-Fi — опорный
        // в покое (вставили кабель посреди движения — картинка до тех пор по Wi-Fi), нет — сейчас
        if (wantKey(d))
            post([svc = m_service] { svc->requestFrame(); });
        qCInfo(lcStream, "картинка — напрямую по кабелю, ввод — через SSH%s", d.waitKey ? " (с опорного в покое)" : "");
        return;
    }
    if (type == stream::Ping && n >= 8) {   // живость пути (и сверка часов — по нему точнее)
        if (FILE *f = tickTrace())
            fprintf(f, "p,%lld,%llu,1\n", (long long)nowUs(), (unsigned long long)get64(p)), fflush(f);
        std::vector<uint8_t> body;
        put64(body, get64(p));
        put64(body, uint64_t(nowUs()));
        send(message(stream::Pong, body.data(), body.size()), int(i));
    }
    // остальное по прямому пути не выполняется: ввод — только через SSH
}

void StreamService::Worker::closeDirectListener()
{
    if (m_directListen >= 0)
        ::close(m_directListen);
    m_directListen = -1;
    m_directAddr = 0;
    m_directPassValid = false;
    m_directViewer = 0;
}

// ---------- зрители и экраны (docs/SCREENS.md) ----------

StreamService::Worker::Viewer *StreamService::Worker::viewer(int id)
{
    for (Viewer &v : m_viewers)
        if (v.id == id)
            return &v;
    return nullptr;
}

StreamService::Worker::Viewer *StreamService::Worker::carrier(const std::string &screen)
{
    for (Viewer &v : m_viewers)
        if (v.known && std::any_of(v.screens.begin(), v.screens.end(), [&](const Screen &s) { return s.id == screen; }))
            return &v;
    return nullptr;
}

long StreamService::Worker::indexOf(int fd) const
{
    for (size_t k = 0; k < m_paths.size(); ++k)
        if (m_paths[k].fd == fd)
            return long(k);
    return -1;
}

int StreamService::Worker::liveCount() const
{
    return int(std::count_if(m_viewers.begin(), m_viewers.end(), [](const Viewer &v) { return v.live; }));
}

bool StreamService::Worker::screenOn(const std::string &id) const
{
    return m_enabled && std::find(m_on.begin(), m_on.end(), id) != m_on.end();
}

bool StreamService::Worker::shows(const Viewer &v) const
{
    return std::any_of(v.screens.begin(), v.screens.end(), [this](const Screen &s) { return screenOn(s.id); });
}

// Сеанс зрителю — когда стол показывает его экран; а у кого клавиатура и мышь стола (приставка) — и когда стол
// показывает чужой экран: он им управляет (docs/SCREENS.md, «Ввод»)
bool StreamService::Worker::shouldBeLive(const Viewer &v)
{
    if (shows(v))
        return true;
    if (!v.input || !m_enabled)
        return false;
    return std::any_of(m_on.begin(), m_on.end(), [&](const std::string &id) {
        const Viewer *c = carrier(id);
        return c && c != &v;
    });
}

StreamService::Worker::Viewer *StreamService::Worker::mainViewer()
{
    if (Viewer *v = viewer(m_mainViewer); v && v->live)
        return v;
    if (m_mainPending)   // зритель главного экрана уже подключается: форму стола и частоту назовёт он — не менять зря
        return nullptr;
    for (Viewer &v : m_viewers)   // главного нет на связи — первый, кому стол показывают
        if (v.live && shows(v))
            return &v;
    for (Viewer &v : m_viewers)
        if (v.live)
            return &v;
    return nullptr;
}

// Screens: кто этот зритель и какие экраны несёт. Второе слово пути (за Join) — по нему путь идёт в сеанс или
// становится тихим; потом — когда экраны сменились (к приставке подключился планшет)
void StreamService::Worker::onScreens(size_t i, const uint8_t *p, size_t n)
{
    const int fd = m_paths[i].fd;
    if (!m_paths[i].viewer) {   // раньше Join: зритель без билета
        Viewer fresh;
        fresh.id = m_nextViewer++;
        m_viewers.push_back(fresh);
        m_paths[i].viewer = fresh.id;
    }
    Viewer &v = *viewerOf(m_paths[i]);
    const QJsonObject o = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(p), qsizetype(n))).object();
    std::vector<Screen> screens;
    QStringList said;
    for (const QJsonValue &x : o.value(QStringLiteral("screens")).toArray()) {
        const QJsonObject j = x.toObject();
        Screen sc{j.value(QStringLiteral("id")).toString().left(32).toStdString(),
                  j.value(QStringLiteral("name")).toString().left(48).toStdString(),
                  j.value(QStringLiteral("kind")).toString().left(16).toStdString(),
                  std::clamp(j.value(QStringLiteral("w")).toInt(), 0, 16384),
                  std::clamp(j.value(QStringLiteral("h")).toInt(), 0, 16384),
                  j.value(QStringLiteral("light")).toInt() != 0, j.value(QStringLiteral("wake")).toInt() != 0};
        if (sc.id.empty() || std::any_of(screens.begin(), screens.end(), [&](const Screen &s) { return s.id == sc.id; }))
            continue;
        said << QStringLiteral("%1 (%2, %3×%4)").arg(QString::fromStdString(sc.id), QString::fromStdString(sc.name))
                    .arg(sc.w).arg(sc.h);
        screens.push_back(std::move(sc));
    }
    if (screens.empty())
        screens.push_back({"screen", "", "", 0, 0});
    v.name = o.value(QStringLiteral("viewer")).toString().left(32).toStdString();
    v.wall = o.value(QStringLiteral("wall")).toString().left(16).toStdString();
    v.input = o.value(QStringLiteral("input")).toInt() != 0;
    const bool same = v.known && v.screens.size() == screens.size()
                      && std::equal(screens.begin(), screens.end(), v.screens.begin(), [](const Screen &a, const Screen &b) {
                             return a.id == b.id && a.name == b.name && a.kind == b.kind && a.w == b.w && a.h == b.h
                                    && a.light == b.light && a.wake == b.wake;
                         });
    if (!same)
        qCInfo(lcStream, "зритель %s: экраны — %s", v.name.c_str(), qPrintable(said.join(QStringLiteral(", "))));
    v.screens = std::move(screens);
    v.known = true;
    // Экран несёт один зритель: тот же id у прошлого (окно Mac открыли заново — билет новый; приставку перезапустили)
    // — прошлый его теряет; остался без экранов — уходит совсем
    for (Viewer &old : m_viewers) {
        if (&old == &v || !old.known)
            continue;
        const size_t had = old.screens.size();
        old.screens.erase(std::remove_if(old.screens.begin(), old.screens.end(), [&](const Screen &s) {
            return std::any_of(v.screens.begin(), v.screens.end(), [&](const Screen &mine) { return mine.id == s.id; });
        }), old.screens.end());
        if (old.screens.size() == had || !old.screens.empty())
            continue;
        qCInfo(lcStream, "зритель %s пришёл заново — прошлый отключён", v.name.c_str());
        const int oldId = old.id;
        if (old.live)
            dropViewer(old);
        for (size_t k = m_paths.size(); k-- > 0;)
            if (m_paths[k].viewer == oldId) {
                ::close(m_paths[k].fd);
                m_paths.erase(m_paths.begin() + long(k));
            }
        old.known = false;
        old.goneSince = 1;   // забыть на ближайшем круге
    }
    reconcile();
    const long k = indexOf(fd);
    if (k < 0)
        return;
    if (!m_paths[size_t(k)].joined && !m_paths[size_t(k)].park)
        admit(size_t(k));
    else
        sendWall(v, int(k));
    sendLights(v);
}

// Путь зрителя, чьи экраны известны: стол показывает хоть один из них — в сеанс, нет — путь тихий
void StreamService::Worker::admit(size_t i)
{
    Viewer &v = *viewerOf(m_paths[i]);
    v.goneSince = 0;
    if (shouldBeLive(v))
        joinLive(i);
    else
        park(i);
    if (const long k = indexOf(m_paths[i].fd); k >= 0)
        sendWall(v, int(k));
}

// Путь — в сеанс своего зрителя. Первый путь зрителя — сеанс его начинается (startViewer); сеансы уже идут (стол
// смотрят другие) — кодер и буферы не трогаются, ему — Hello и опорный кадр. Ещё один путь того же зрителя — как
// всегда: переход между кабелем и Wi-Fi без перезапуска.
void StreamService::Worker::joinLive(size_t i)
{
    Viewer &v = *viewerOf(m_paths[i]);
    const bool first = !v.live, running = m_session;
    if (first)
        startViewer(v);
    Path &p = m_paths[i];
    p.joined = true;
    p.park = 0;
    v.orphanSince = 0;
    if (first) {
        // стол уже смотрят: этому зрителю кадров не было — с опорного, сейчас (если стол выведен на его экран)
        if (running && shows(v) && wantKey(p))
            post([svc = m_service] { svc->requestFrame(); });
        hello(running ? int(i) : -1);
        qCInfo(lcStream, "зритель %s подключился, %.1f Мбит/с, путь: %s%s", v.name.c_str(), m_bitrate / 1e6,
               routeName(p.route), running ? " (стол уже смотрят)" : "");
    } else {
        qCInfo(lcStream, "путь зрителя %s: %s%s", v.name.c_str(), routeName(p.route), p.use ? "" : " (запасной)");
        // путь присоединился к живому сеансу — декодер зрителя продолжает. Wi-Fi медленнее кабеля: его кадры — копии,
        // приходят позже тех же по кабелю, годятся с любого места (выдернут кабель — картинка уже идёт по нему). Кабель
        // быстрее: его кадр мог бы обогнать предыдущий по Wi-Fi, а тот зритель выбросит как старый (след до опорного) —
        // по кабелю с опорного, в покое (wantKey). Все пути оборвались и зритель вернулся — что-то не дошло: с опорного
        if (v.activeFd < 0) {
            p.needKey = true;
            hello(int(i));   // вернулся, оборвав все пути (и то, что знал о потоке, забыл): что идёт — ему снова
        } else if (p.route == stream::RouteCable) {
            wantKey(p);
        }
    }
    std::vector<uint8_t> r{uint8_t(p.route)};
    send(message(stream::Route, r.data(), r.size()), int(i));
    sendAddress(int(i));
    sendSettings(int(i));
    sendShow(int(i));
    if (p.use || v.activeFd < 0)
        activate(i);
    if (first)
        refresh();
}

// Сеанс зрителя начинается. Первый из зрителей — начинаются и кадры: буферы, кодер (их запустит первый кадр)
void StreamService::Worker::startViewer(Viewer &v)
{
    const bool firstLive = !m_session;
    v.live = true;
    v.seen.clear();
    v.hevc = v.audio = v.lock = false;   // что просил в прошлом сеансе — не в этот: скажет снова (Want, Mode, DeskSize)
    v.wanted = false;
    v.deskFps = v.encodeFps = kDeskFps;
    v.mode = Phone;
    v.sized = false;
    v.buttons = 0;
    v.touching = false;
    v.activeFd = -1;
    v.orphanSince = 0;
    v.showing = false;
    if (!firstLive)
        return;
    m_session = true;
    m_hevc = false;
    m_deskFps = m_encodeFps = kDeskFps;
    m_lockViewer = m_audioViewer = false;
    m_tickViewer = 0;
    m_awayOf.clear();
    {
        std::lock_guard lock(m_s->m);
        m_s->want = true;
        m_s->source = Phone;   // пока зритель не выбрал (Mode)
        m_s->owner = -1;
        m_s->buffersReady = false;
    }
    post([svc = m_service] {
        svc->onDesktopFps(kDeskFps, false);
        svc->onViewer(true, QString());
        svc->onLockViewer(false, true);   // кто он — скажет Want; окно без заставки — стол как есть
        svc->onSource(Phone);
    });
}

// Стол на экраны этого зрителя больше не выведен (или выключен): его пути по SSH не закрыть, а сделать тихими — он
// узнает (Switch 0) и подождёт на одном из них; закрой телефон путь, зритель стучался бы снова каждые 1,5 с
void StreamService::Worker::parkViewer(Viewer &v)
{
    qCInfo(lcStream, "зритель %s: стол на его экраны не выведен — пути тихие", v.name.c_str());
    for (size_t k = m_paths.size(); k-- > 0;) {
        if (m_paths[k].viewer != v.id)
            continue;
        if (m_paths[k].direct) {
            ::close(m_paths[k].fd);
            m_paths.erase(m_paths.begin() + long(k));
        } else if (m_paths[k].joined) {
            park(k);
        }
    }
    if (m_directViewer == v.id)
        closeDirectListener();
    v.live = false;
    v.activeFd = -1;
    v.orphanSince = 0;
    v.buttons = 0;
    v.touching = false;
    afterViewerLeft();
}

// Зритель пропал (не вернулся, пришёл заново с другим билетом): его пути сеанса — закрыть
void StreamService::Worker::dropViewer(Viewer &v)
{
    for (size_t k = m_paths.size(); k-- > 0;)
        if (m_paths[k].viewer == v.id && (m_paths[k].joined || m_paths[k].direct)) {
            ::close(m_paths[k].fd);
            m_paths.erase(m_paths.begin() + long(k));
        }
    if (m_directViewer == v.id)
        closeDirectListener();
    const int id = v.id;
    if (std::none_of(m_paths.begin(), m_paths.end(), [id](const Path &p) { return p.viewer == id; }) && !v.goneSince)
        v.goneSince = nowUs();
    v.live = false;
    v.activeFd = -1;
    v.orphanSince = 0;
    v.buttons = 0;
    v.touching = false;
    afterViewerLeft();
}

void StreamService::Worker::afterViewerLeft()
{
    if (!liveCount())
        stopSession();
}

// План экранов или сами экраны сменились: у кого стол показывает хоть один экран — тому сеанс (ждущему на тихом пути
// — Switch 1: подключится заново сам), у кого ни один — пути тихие. Потом — главный, общие просьбы, Show
void StreamService::Worker::reconcile()
{
    for (Viewer &v : m_viewers)
        if (v.known && v.live && !shouldBeLive(v))
            parkViewer(v);
    // Show — раньше, чем звать ждущих (Switch 1): подключаясь, зритель уже знает, какие его экраны покажут стол и чей
    // главный (форму стола назовёт сразу нужную)
    refresh();
    const uint8_t on = 1;
    for (Viewer &v : m_viewers) {
        if (!v.known || v.live || !shouldBeLive(v))
            continue;
        for (size_t k = 0; k < m_paths.size(); ++k)
            if (m_paths[k].viewer == v.id && m_paths[k].park == 1) {
                m_paths[k].park = 2;
                m_paths[k].closeAt = nowUs() + kReleaseUs;
                send(message(stream::Switch, &on, 1), int(k));
                qCInfo(lcStream, "стол — на экран зрителя %s: ждущему по пути %s — подключайся", v.name.c_str(),
                       routeName(m_paths[k].route));
            }
    }
}

// Главный экран — выбранный, если стол на него выведен и он на связи; иначе первый такой из выведенных. Форму стола
// (DeskSize), частоту (Want fps) и такт (Tick) задаёт зритель, чей это экран; кодек и звук — если просит хоть один
void StreamService::Worker::refresh()
{
    auto there = [this](const std::string &id) { return screenOn(id) && carrier(id); };
    std::string screen;
    if (there(m_main))
        screen = m_main;
    else
        for (const std::string &id : m_on)
            if (there(id)) {
                screen = id;
                break;
            }
    Viewer *c = screen.empty() ? nullptr : carrier(screen);
    const int mv = c && c->live ? c->id : 0;
    m_mainPending = c && !c->live;
    if (screen != m_mainScreen || mv != m_mainViewer) {
        m_mainScreen = screen;
        m_mainViewer = mv;
        qCInfo(lcStream, "главный экран: %s%s", screen.empty() ? "нет" : screen.c_str(),
               screen.empty() || mv ? "" : " (его зритель ещё подключается)");
    }
    // Сеанс идёт, а стол на экраны зрителя вывели только сейчас (или убрали: остался один ввод) — кадры ему с опорного
    // (или больше не идут)
    for (Viewer &v : m_viewers) {
        const bool showing = v.live && shows(v);
        if (showing == v.showing)
            continue;
        v.showing = showing;
        bool key = false;
        for (Path &p : m_paths) {
            if (p.viewer != v.id || !p.joined)
                continue;
            if (!showing) {
                for (auto it = p.out.begin(); it != p.out.end();)
                    it = it->video && it->sent == 0 ? p.out.erase(it) : it + 1;
                p.waitKey = 0;
            } else if (videoPath(p)) {
                key |= wantKey(p);
            }
        }
        if (key)
            post([svc = m_service] { svc->requestFrame(); });
    }
    if (m_session) {
        applyWants();
        applySource();
        applySize();
        Viewer *m = mainViewer();
        if (!m)   // главный подключается — значок и имя пока по тому, кто есть
            for (Viewer &v : m_viewers)
                if (v.live && !m)
                    m = &v;
        if (m) {
            const long k = indexOf(m->activeFd);
            const int route = k < 0 ? 0 : m_paths[size_t(k)].route;
            // по Wi-Fi идёт основной путь хоть одного зрителя — радио держать без сна (holdWifi)
            bool wifi = false;
            QStringList names;
            for (const Viewer &v : m_viewers) {
                if (!v.live)
                    continue;
                names << QString::fromStdString(v.name);
                const long a = indexOf(v.activeFd);
                wifi |= a >= 0 && m_paths[size_t(a)].route != stream::RouteCable;
            }
            post([svc = m_service, name = QString::fromStdString(m->name), names, route, wifi] {
                svc->onViewers(name, names, wifi);
                svc->onRoute(route);
            });
        }
    }
    sendShow();
    postScreens();
}

void StreamService::Worker::applyWants()
{
    bool hevc = false, audio = false, lock = false;
    int enc = 0;
    for (const Viewer &v : m_viewers) {
        if (!v.live)
            continue;
        hevc |= v.hevc;
        audio |= v.audio;
        lock |= v.lock;
        enc = std::max(enc, v.encodeFps);
    }
    if (audio != m_audioViewer) {
        m_audioViewer = audio;
        qCInfo(lcStream, "звук стола зрители %s", audio ? "играют" : "не играют");
        post([svc = m_service, audio] { svc->onAudioViewer(audio); });
    }
    if (lock != m_lockViewer) {
        m_lockViewer = lock;
        // первый зритель с заставкой — стол начинает без панели и окон (он покажет сам, Reveal); пришёл к идущему
        // столу — стол как есть
        const bool fresh = liveCount() == 1;
        if (lock && fresh)
            qCInfo(lcStream, "зритель с экраном без дока: сеанс — без панели и окон, покажет сам");
        post([svc = m_service, lock, fresh] { svc->onLockViewer(lock, fresh); });
    }
    // частота стола — главного; он ещё не сказал своей (Want — следом за Join) — прежняя: кодер дважды не перезапускать
    const Viewer *m = mainViewer();
    const int fps = m && m->wanted ? m->deskFps : m_deskFps;
    want(hevc, fps, std::max(enc, fps));
}

// Что идёт: рабочий стол, если его просит хоть один зритель; иначе экран телефона
void StreamService::Worker::applySource()
{
    const bool desk = std::any_of(m_viewers.begin(), m_viewers.end(),
                                  [](const Viewer &v) { return v.live && v.mode == Desktop; });
    switchSource(desk ? Desktop : Phone);
}

// Форма стола — главного зрителя, и только та, что он назвал для нынешнего главного экрана: у приставки экранов два
// (монитор и планшет), и форма от прошлого главного — не та; новую она пришлёт сама, как узнает (Show). Было: главный
// сменился — стол на миг принимал прежнюю форму, через миллисекунды — нужную: две смены размера подряд
void StreamService::Worker::applySize()
{
    if (const Viewer *m = mainViewer(); m && m->sized && (m->sizedFor == m_mainScreen || m->screens.size() <= 1))
        resizeDesktop(m->deskW, m->deskH, m->deskScale);
}

// Show — что показывает стол и кто главный: всем зрителям, и ждущим на тихих путях (docs/SCREENS.md)
void StreamService::Worker::sendShow(int path)
{
    QJsonArray live;
    for (const std::string &id : m_on)
        if (m_enabled && carrier(id))
            live.append(QString::fromStdString(id));
    const QByteArray j = QJsonDocument(QJsonObject{{"live", live}, {"main", QString::fromStdString(m_mainScreen)},
                                                   {"desk", m_enabled ? 1 : 0}}).toJson(QJsonDocument::Compact);
    const std::vector<uint8_t> msg = message(stream::Show, j.constData(), size_t(j.size()));
    if (path >= 0) {
        send(msg, path);
        return;
    }
    if (j.toStdString() == m_shown)
        return;
    m_shown = j.toStdString();
    qCInfo(lcStream, "экраны: %s", j.constData());
    for (size_t k = 0; k < m_paths.size(); ++k)
        if (!m_paths[k].direct && m_paths[k].viewer && (m_paths[k].joined || m_paths[k].park == 1))
            send(msg, int(k));
}

// Обои заставки без дока — зрителю, у которого не те (хэш в Screens): по пути path, −1 — по любому его пути через SSH
void StreamService::Worker::sendWall(Viewer &v, int path)
{
    std::unique_lock lock(m_s->m);
    if (m_s->wallHash.empty() || m_s->wallHash == v.wall)
        return;
    std::vector<uint8_t> body(16, 0);
    memcpy(body.data(), m_s->wallHash.data(), std::min<size_t>(16, m_s->wallHash.size()));
    body.insert(body.end(), m_s->wallJpeg.begin(), m_s->wallJpeg.end());
    const std::string hash = m_s->wallHash;
    lock.unlock();
    if (path < 0)
        for (size_t k = 0; k < m_paths.size() && path < 0; ++k)
            if (m_paths[k].viewer == v.id && !m_paths[k].direct && (m_paths[k].joined || m_paths[k].park == 1))
                path = int(k);
    if (path < 0)
        return;
    v.wall = hash;
    send(message(stream::Wallpaper, body.data(), body.size()), path);
    qCInfo(lcStream, "обои заставки — зрителю %s (%zu КБ)", v.name.c_str(), (body.size() - 16) / 1024);
}

// Яркость экранов зрителя (ScreenLight) — тем, у которых её можно ставить: по всем его путям через SSH, и тихим
void StreamService::Worker::sendLights(Viewer &v)
{
    for (const Screen &s : v.screens) {
        const auto it = m_lights.find(s.id);
        if (!s.light || it == m_lights.end())
            continue;
        const QByteArray j = QJsonDocument(QJsonObject{{"id", QString::fromStdString(s.id)},
                                                       {"auto", it->second.first ? 1 : 0},
                                                       {"level", it->second.second}}).toJson(QJsonDocument::Compact);
        const std::vector<uint8_t> msg = message(stream::ScreenLight, j.constData(), size_t(j.size()));
        for (size_t k = 0; k < m_paths.size(); ++k)
            if (m_paths[k].viewer == v.id && !m_paths[k].direct && (m_paths[k].joined || m_paths[k].park == 1))
                send(msg, int(k));
    }
}

// «Включи экран» (ScreenWake; on = false — уже не надо) — зрителю, что его несёт: по всем его путям через SSH, и
// тихим. Экрана на связи нет (телевизор спит или показывает другое) — зрителя ищем по имени, которое экран называл,
// когда был на связи
void StreamService::Worker::sendScreenWake(const std::string &viewer, const std::string &screen, bool on)
{
    const QByteArray j = QJsonDocument(QJsonObject{{"id", QString::fromStdString(screen)}, {"on", on ? 1 : 0}})
                             .toJson(QJsonDocument::Compact);
    const std::vector<uint8_t> msg = message(stream::ScreenWake, j.constData(), size_t(j.size()));
    int sent = 0;
    for (const Viewer &v : m_viewers) {
        if (!v.known || v.name != viewer)
            continue;
        for (size_t k = 0; k < m_paths.size(); ++k)
            if (m_paths[k].viewer == v.id && !m_paths[k].direct && (m_paths[k].joined || m_paths[k].park == 1)) {
                send(msg, int(k));
                ++sent;
            }
    }
    qCInfo(lcStream, "экран %s: %s", screen.c_str(),
           sent ? qPrintable((on ? QStringLiteral("прошу зрителя %1 включить его")
                                 : QStringLiteral("зрителю %1 — включать уже не надо")).arg(QString::fromStdString(viewer)))
                : qPrintable(QStringLiteral("зрителя %1 нет на связи — включить некому").arg(QString::fromStdString(viewer))));
}

// Экраны на связи — интерфейсу («Экраны» в «Настройках» и на столе)
void StreamService::Worker::postScreens()
{
    QJsonArray list;
    for (const Viewer &v : m_viewers) {
        if (!v.known)
            continue;
        int route = 0;
        for (const Path &p : m_paths)
            if (p.viewer == v.id && !p.direct && (p.fd == v.activeFd || !route))
                route = p.route;
        for (const Screen &s : v.screens)
            list.append(QJsonObject{{"id", QString::fromStdString(s.id)}, {"name", QString::fromStdString(s.name)},
                                    {"kind", QString::fromStdString(s.kind)}, {"w", s.w}, {"h", s.h},
                                    {"viewer", QString::fromStdString(v.name)},
                                    {"light", s.light}, {"wake", s.wake},
                                    {"live", v.live && screenOn(s.id)},
                                    {"route", route == stream::RouteCable ? "cable" : route == stream::RouteWifi ? "wifi" : ""}});
    }
    const QByteArray j = QJsonDocument(list).toJson(QJsonDocument::Compact) + QByteArray::fromStdString(m_mainScreen);
    if (j == m_screensPosted)
        return;
    m_screensPosted = j;
    post([svc = m_service, list = list.toVariantList(), main = QString::fromStdString(m_mainScreen)] {
        svc->onScreens(list, main);
    });
}

// На экране зрителя нажали клавишу, щёлкнули, дважды коснулись — стол сюда; что это значит, решает поток интерфейса
void StreamService::Worker::wake(Viewer &v, const uint8_t *p, size_t n)
{
    std::string id(reinterpret_cast<const char *>(p), std::min<size_t>(n, 32));
    if (id.empty() && !v.screens.empty())
        id = v.screens.front().id;
    qCInfo(lcStream, "экран %s будит стол (зритель %s)", id.c_str(), v.name.c_str());
    post([svc = m_service, id = QString::fromStdString(id)] { svc->onWake(id); });
}

// Мышь стола ведёт зритель from — остальным, кто смотрит стол: где курсор (у них своего в этом месте нет)
void StreamService::Worker::sendCursorAt(const Viewer &from, double x, double y)
{
    const int64_t now = nowUs();
    uint8_t body[9] = {1};
    const float fx = float(x), fy = float(y);
    memcpy(body + 1, &fx, 4);
    memcpy(body + 5, &fy, 4);
    for (Viewer &o : m_viewers) {
        if (&o == &from || !o.live || o.mode != Desktop || now - o.cursorSentUs < kCursorAtUs)
            continue;
        const long k = indexOf(o.activeFd);
        if (k < 0)
            continue;
        o.cursorSentUs = now;
        send(message(stream::CursorAt, body, sizeof body), int(k));
    }
}

// Путь — тихий (Switch 0): стол этому зрителю не показывают. Путь не закрывается — зритель держит его и ждёт; закрой
// телефон путь, зритель стучался бы снова каждые 1,5 с, и каждый раз — новый вход по SSH (ключи, шифр, процесс sshd)
void StreamService::Worker::park(size_t i)
{
    Path &p = m_paths[i];
    p.park = 1;
    p.joined = false;
    p.closeAt = 0;
    for (auto it = p.out.begin(); it != p.out.end();)   // кадры и звук, ещё не начатые, — прочь
        it = (it->video || it->audio) && it->sent == 0 ? p.out.erase(it) : it + 1;
    const uint8_t off = 0;
    send(message(stream::Switch, &off, 1), int(i));
    sendShow(int(i));
    sendSettings(int(i));   // тихому — только Screen: часы заставки
    qCInfo(lcStream, "путь %s ждёт тихо", routeName(p.route));
}

// Основной путь зрителя — этот (Use): значок пути. Кадры и так идут по всем его путям — сеанс, кодер, буферы, декодер
// зрителя не трогаются. По этому пути кадры пропадали (или он вернулся после обрыва всех) — опорный кадр сейчас.
void StreamService::Worker::activate(size_t i)
{
    Viewer *v = viewerOf(m_paths[i]);
    if (!v || m_paths[i].fd == v->activeFd)
        return;
    const long old = indexOf(v->activeFd);
    const int was = old < 0 ? 0 : m_paths[size_t(old)].route;
    v->activeFd = m_paths[i].fd;
    // по этому пути кадры пропадали — опорный (wantKey: сейчас, если зрителю их больше не везёт никто, иначе в
    // покое). Кадры SSH кабеля при прямом пути везёт прямой — опорный, если ждёт он
    Path *carrierPath = &m_paths[i];
    if (!videoPath(*carrierPath))
        for (Path &p : m_paths)
            if (p.direct && p.joined && p.viewer == v->id)
                carrierPath = &p;
    const bool key = carrierPath->needKey && wantKey(*carrierPath);
    const int route = m_paths[i].route;
    qCInfo(lcStream, "основной путь зрителя %s — %s (был: %s)%s", v->name.c_str(), routeName(route), routeName(was),
           key ? ", с опорного кадра" : "");
    const bool main = mainViewer() == v;
    post([svc = m_service, route, key, main] {
        if (main)
            svc->onRoute(route);
        if (key)
            svc->requestFrame();   // опорный кадр — сразу, и в покое
    });
    postScreens();
}

void StreamService::Worker::closePath(size_t i)
{
    const int fd = m_paths[i].fd, vid = m_paths[i].viewer;
    const bool joined = m_paths[i].joined, direct = m_paths[i].direct;
    ::close(fd);
    m_paths.erase(m_paths.begin() + long(i));
    Viewer *v = viewer(vid);
    if (!v)
        return;
    if (std::none_of(m_paths.begin(), m_paths.end(), [vid](const Path &p) { return p.viewer == vid; }))
        v->goneSince = nowUs();
    if (!joined || !v->live)
        return;
    if (direct) {
        // картинка — снова по SSH кабеля: ему кадров не было — с опорного. Шли копии по Wi-Fi — зритель их и так
        // получает без перерыва: опорный — в покое (wantKey), и только если SSH кабеля жив (выдернули кабель — нет:
        // путь Wi-Fi не трогать, без шва)
        const bool copies = std::any_of(m_paths.begin(), m_paths.end(), [&](const Path &p) {
            return p.viewer == vid && p.route != stream::RouteCable && carries(p);
        });
        bool now = false;
        for (Path &p : m_paths)
            if (p.viewer == vid && p.joined && p.route == stream::RouteCable)
                now |= wantKey(p);
        if (now)
            post([svc = m_service] { svc->requestFrame(); });
        qCInfo(lcStream, "прямой путь закрыт — картинка %s", copies ? "идёт по Wi-Fi" : "снова через SSH");
        return;
    }
    if (fd == v->activeFd) {
        v->activeFd = -1;
        // запасной путь есть — основной он (зритель обычно переводит сам, раньше; это — если не успел)
        for (size_t k = 0; k < m_paths.size(); ++k)
            if (m_paths[k].viewer == vid && m_paths[k].joined) {
                activate(k);
                return;
            }
        qCInfo(lcStream, "путь зрителя %s оборвался — ждём его %d с", v->name.c_str(), int(kOrphanUs / 1000000));
        v->orphanSince = nowUs();
        if (mainViewer() == v)
            post([svc = m_service] { svc->onRoute(stream::RouteUnknown); });
    }
}

// что сейчас идёт: размер, к/с, кодек, источник. Всем путям сеансов — с новым номером (поток новый: декодеры заново);
// одному пути (зритель пришёл к идущему столу) — с прежним: остальным это не новость
void StreamService::Worker::hello(int path)
{
    int w, h, source;
    {
        std::lock_guard lock(m_s->m);
        source = m_s->source;
        w = m_s->width[source];
        h = m_s->height[source];
    }
    const bool native = m_service->m_localDesktop;
    QJsonObject json{{"width", w}, {"height", h}, {"fps", native ? std::min(m_deskFps, 60) : source == Desktop ? m_deskFps : 90},
                     {"codec", m_hevc ? "hvc1" : "avc1.42E033"},
                     {"bitrate", m_bitrate}, {"name", "NaomiPhone"},
                     {"mode", native || source == Desktop ? "desktop" : "phone"},
                     {"gen", int(path < 0 ? ++m_helloGen : m_helloGen)}};
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const std::vector<uint8_t> msg = message(stream::Hello, bytes.constData(), size_t(bytes.size()));
    if (path < 0)
        sendAll(msg);
    else
        send(msg, path);
}

// Зритель выбрал, что смотреть: кодер — заново под размер источника (у стола и телефона разные), новый Hello;
// связь не рвётся. Буферы прошлого источника остаются у него до конца сеанса.
void StreamService::Worker::switchSource(int source)
{
    {
        std::lock_guard lock(m_s->m);
        if (m_s->source == source)
            return;
    }
    {
        std::lock_guard lock(m_s->m);
        m_s->source = source;
    }
    stopEncoding();
    hello();
    qCInfo(lcStream, "зрители смотрят %s", source == Desktop ? "рабочий стол" : "экран телефона");
    post([svc = m_service, source] { svc->onSource(source); });
}

void StreamService::Worker::stopEncoding()
{
    m_enc.close();
    m_encoding = false;
    {
        std::lock_guard lock(m_s->m);
        m_s->running = false;
        m_s->buffersReady = false;
        m_s->poolFailed = false;
        m_s->queues = {};
        ++m_s->generation;
        m_s->owner = -1;
        for (auto &j : m_s->jobs)
            for (int fd : {j.fence, j.frameFence})
                if (fd >= 0)
                    ::close(fd);
        m_s->jobs.clear();
    }
    for (auto &j : m_pending)
        for (int fd : {j.fence, j.frameFence})
            if (fd >= 0)
                ::close(fd);
    m_pending.clear();
    m_inEncoder.clear();
    // кадры прошлого потока, ещё не ушедшие, — выбросить: новый поток начнётся с опорного кадра
    for (Path &p : m_paths)
        for (auto it = p.out.begin(); it != p.out.end();)
            it = it->video && it->sent == 0 ? p.out.erase(it) : it + 1;
}

void StreamService::Worker::applyNativeRotation()
{
    {
        std::lock_guard lock(m_s->m);
        if (m_s->requestedRotation == m_s->sceneRotation)
            return;
    }
    stopEncoding();
    {
        std::lock_guard lock(m_s->m);
        if (m_s->requestedRotation % 180 != m_s->sceneRotation % 180)
            std::swap(m_s->width[Phone], m_s->height[Phone]);
        m_s->sceneRotation = m_s->requestedRotation;
    }
    if (m_session)
        hello();
}

// Форма и масштаб стола (DeskSize). Окно Mac шлёт только высоту: ширина 1280 точек, масштаб 1,5 (кадр 1920), высота —
// под окно, от 16:9 до 4:3. Приставка на Pi — ещё ширину и масштаб под свой монитор (docs/PI.md, «Размер и масштаб»).
// Точки — кратные 4, кадр — точки × масштаб, чётный. Здесь — только кодер и размер потока; стол и его буферы кадра
// меняет поток интерфейса (onDesktopSize), до того кадры стола не снимаются (deskResize). Смотрят стол — новый Hello:
// зритель готовит декодер под новый размер, поток начнётся с опорного кадра. Связь не рвётся.
void StreamService::Worker::resizeDesktop(int logicalWidth, int logicalHeight, int scaleMilli)
{
    const int w = std::clamp(logicalWidth / 4 * 4, 1024, 2048), h = std::clamp(logicalHeight / 4 * 4, 720, 1280);
    const int milli = std::clamp(scaleMilli, 1000, 2000);
    const double scale = milli / 1000.0;
    const int fw = int(std::lround(w * scale / 2)) * 2, fh = int(std::lround(h * scale / 2)) * 2;   // как у стола
    if (fw > 2560 || fh > 1600) {
        qCWarning(lcStream, "стол %dx%d при масштабе %.2f — кадр %dx%d больше 2560x1600: не беру", w, h, scale, fw, fh);
        return;
    }
    int source;
    {
        std::lock_guard lock(m_s->m);
        if (m_s->width[Desktop] == fw && m_s->height[Desktop] == fh && m_deskLogical == QSize(w, h)
            && m_deskScaleMilli == milli)
            return;
        source = m_s->source;
        m_s->width[Desktop] = fw;
        m_s->height[Desktop] = fh;
        ++m_s->deskResize;
    }
    m_deskLogical = QSize(w, h);
    m_deskScaleMilli = milli;
    if (source == Desktop)
        stopEncoding();
    qCInfo(lcStream, "рабочий стол: %dx%d точек, масштаб %.2f, кадр %dx%d", w, h, scale, fw, fh);
    post([svc = m_service, w, h, scale] { svc->onDesktopSize(QSize(w, h), scale); });
    if (source == Desktop)
        hello();
}

// Зритель просит кодек и частоту стола (Want): приставка на Pi — HEVC (аппаратного H.264 у Pi 5 нет) под частоту
// монитора. Стол рисует не чаще (поток интерфейса), кодер — заново под них, новый Hello. Кодер ещё не шёл —
// возьмёт их при запуске. encodeFps — нижняя граница «скорости работы» кодера (operatingRate): когда-то её называли
// кодеру частотой кадров, чтобы он шёл быстрее; теперь кодеру называется настоящая частота (startEncoder)
void StreamService::Worker::want(bool hevc, int fps, int encodeFps)
{
    if (hevc == m_hevc && fps == m_deskFps && encodeFps == m_encodeFps)
        return;
    const bool announce = hevc != m_hevc || fps != m_deskFps;
    m_hevc = hevc;
    m_deskFps = fps;
    m_encodeFps = encodeFps;
    qCInfo(lcStream, "зрители просят: %s, стол %d к/с, кодеру — как для %d", hevc ? "HEVC" : "H.264", fps, encodeFps);
    post([svc = m_service, fps] { svc->onDesktopFps(fps, true); });
    if (m_enc.queues().inputs) {
        stopEncoding();
        hello();
        post([svc = m_service] { svc->requestFrame(); });
    } else if (announce && m_session) {
        hello();
    }
}

// «Скорость работы» кодера: чем она выше, тем быстрее сжатие (docs/STREAM.md, «Кодер: честный замер») — наибольшая,
// какую драйвер берёт для такого кадра, пока видеокодек больше никому не нужен; рядом могут понадобиться декодеры
// (decodersNear) — с местом для них («Кодек на всех»). Зритель назвал больше (Want encodeFps) — не меньше его.
// NAOMI_STREAM_RATE — своя (разведка)
int StreamService::Worker::operatingRate(int width, int height, bool desk) const
{
    if (const int rate = envInt("NAOMI_STREAM_RATE", 0))
        return rate;
    const int top = VenusEncoder::maxOperatingRate(width, height);
    const int rate = m_s->decodersNear ? VenusEncoder::sharedOperatingRate(width, height) : top;
    return std::clamp(std::max(rate, desk ? std::max(m_deskFps, m_encodeFps) : 90), 30, top);
}

// Кодер не запустился или сломался (пустые кадры): заново — буферы кадра выложит следующий кадр, зрителям — новый поток
// с опорного. Не вышло трижды за 10 с — сеансы закрыть: зрители подключатся заново сами
void StreamService::Worker::retryEncoder(const char *why)
{
    const int64_t t = nowUs();
    if (t - m_encRestartUs > 10000000)
        m_encRestarts = 0;
    m_encRestartUs = t;
    if (++m_encRestarts > 3) {
        qCWarning(lcStream, "кодер %s — снова; сеансы закрыты, зрители подключатся заново", why);
        m_encRestarts = 0;
        for (Viewer &v : m_viewers)
            if (v.live)
                dropViewer(v);
        refresh();
        return;
    }
    qCWarning(lcStream, "кодер %s — запускаю его заново (%d-й раз)", why, m_encRestarts);
    stopEncoding();
    hello();
    post([svc = m_service] { svc->requestFrame(); });
}

// декодеры стали нужны или больше не нужны — скорость идущему кодеру меняется на ходу
void StreamService::Worker::applyOperatingRate()
{
    if (!m_encoding)
        return;
    const int rate = m_service->m_localDesktop ? m_encSettings.fps : operatingRate(m_encWidth, m_encHeight, m_encDesk);
    if (rate == m_encRate)
        return;
    if (m_enc.setOperatingRate(rate))
        qCInfo(lcStream, "кодер: скорость работы %d → %d (%s)", m_encRate, rate,
               m_s->decodersNear ? "место декодерам" : "кодек свободен");
    m_encRate = rate;
}

void StreamService::Worker::prepareEncoder()
{
    VenusEncoder::Settings st;
    bool desk;
    {
        std::lock_guard lock(m_s->m);
        desk = m_s->source == Desktop;
        if (desk && m_s->deskResize)
            return;
        st.width = m_s->width[m_s->source];
        st.height = m_s->height[m_s->source];
    }
    // Названная кодеру частота — настоящая: экран телефона — 90, стол — частота его кадров (Want fps). Раньше столу
    // называли больше, чем шло (Want encodeFps: 120 при 75 кадрах — чтобы кодер шёл быстрее); с 17 ноября 2026 это
    // делает «скорость работы», а названное зрителем — её нижняя граница (operatingRate). На время сжатия и размер
    // кадра названная частота не влияет (замер 20 ноября 2026). NAOMI_STREAM_FPS — своя
    const bool native = m_service->m_localDesktop;
    st.fps = native ? std::min(m_deskFps, 60) : desk ? m_deskFps : 90;
    if (const int fps = envInt("NAOMI_STREAM_FPS", 0))   // разведка: своя названная частота
        st.fps = fps;
    st.operatingRate = native ? st.fps : operatingRate(st.width, st.height, desk);
    st.hevc = m_hevc;
    // Цель битрейта — та же при любой названной частоте: размер кадра у этого кодера идёт за целью, а не за целью на
    // кадр (замер 20 ноября 2026, окно тащат 40 с: 75 к/с и 60 Мбит/с — кадр 26 КБ, как при названных 120; цель
    // 37,5 — 17–18 КБ: кодер сжимал сильнее). Больше цель — крупнее кадр и дольше путь по кабелю (15 ноября 2026)
    st.bitrate = m_bitrate;
    st.maxQp = m_maxQp;
    st.extra = m_extra;
    if (!m_enc.open(st) || !m_enc.prepareQueues(kInputs, kOutputs)) {
        retryEncoder("не подготовил очередь");
        return;
    }
    m_encSettings = st;
    m_encDesk = desk;
    {
        std::lock_guard lock(m_s->m);
        m_s->queues = m_enc.queues();
        m_s->captureIntervalUs = native ? 1000000 / std::max(1, st.fps) : 0;
    }
    post([svc = m_service] { svc->requestFrame(); });
}

void StreamService::Worker::startEncoder()
{
    std::vector<VenusEncoder::Buffer> inputs, outputs;
    {
        std::lock_guard lock(m_s->m);
        inputs = m_s->inputs;
        outputs = m_s->outputs;
        m_s->freeSlot.assign(inputs.size(), true);
    }
    if (!m_enc.start(inputs, outputs)) {
        m_enc.close();
        retryEncoder("не запустился");
        return;
    }
    const auto &st = m_encSettings;
    m_encoding = true;
    m_emptyRun = 0;
    m_encWidth = st.width;
    m_encHeight = st.height;
    m_encRate = st.operatingRate;
    m_config.clear();
    {
        std::lock_guard lock(m_s->m);
        m_s->running = true;
    }
    m_encHevc = st.hevc;
    qCInfo(lcStream, "кодер запущен: %s %dx%d, %d к/с, скорость работы %d, цель %.1f Мбит/с", st.hevc ? "HEVC" : "H.264",
            st.width, st.height, st.fps, st.operatingRate, st.bitrate / 1e6);
    post([svc = m_service] { svc->requestFrame(); });
}

// Зрителей не осталось: кодер и кадры — прочь
void StreamService::Worker::stopSession()
{
    // сначала кодер (отпускает буферы), потом — «зрителя нет»: поток отрисовки освободит их со следующим кадром
    if (m_encoding) {
        m_enc.close();
        m_encoding = false;
    }
    m_enc.close();
    {
        std::lock_guard lock(m_s->m);
        m_s->want = false;
        m_s->running = false;
        m_s->buffersReady = false;
        m_s->poolFailed = false;
        m_s->queues = {};
        ++m_s->generation;
        for (auto &j : m_s->jobs)
            for (int fd : {j.fence, j.frameFence})
                if (fd >= 0)
                    ::close(fd);
        m_s->jobs.clear();
    }
    for (auto &j : m_pending)
        for (int fd : {j.fence, j.frameFence})
            if (fd >= 0)
                ::close(fd);
    m_pending.clear();
    m_inEncoder.clear();
    closeDirectListener();
    if (!m_session)
        return;
    m_session = false;
    m_audioViewer = false;
    m_tickViewer = 0;
    qCInfo(lcStream, "стол никто не смотрит");
    post([svc = m_service] {
        svc->onViewer(false, QString());
        svc->onRoute(stream::RouteUnknown);
        svc->onAudioViewer(false);
    });
}

void StreamService::Worker::onPacket(VenusEncoder::Packet &pkt)
{
    const int64_t encoded = nowUs();
    if (auto it = m_inEncoder.find(pkt.timestampUs); it != m_inEncoder.end()) {
        m_statEncode.push_back((encoded - pkt.timestampUs) / 1000.0);
        m_statVpu.push_back((encoded - it->second) / 1000.0);
        m_inEncoder.erase(it);
    }
    // H.264: срез 1, опорный 5, SPS 7, PPS 8; HEVC: срезы 0…31, опорные (IRAP) 16…23, VPS 32, SPS 33, PPS 34
    const bool hevc = m_encHevc;
    bool slice = false, idr = false, sps = false;
    std::vector<uint8_t> config;
    for (const Nal &n : nals(pkt.data, hevc)) {
        slice |= hevc ? n.type <= 31 : n.type == 1 || n.type == 5;
        idr |= hevc ? n.type >= 16 && n.type <= 23 : n.type == 5;
        sps |= n.type == (hevc ? 33 : 7);
        if (hevc ? n.type >= 32 && n.type <= 34 : n.type == 7 || n.type == 8) {
            const uint8_t code[] = {0, 0, 0, 1};
            config.insert(config.end(), code, code + 4);
            config.insert(config.end(), pkt.data.begin() + long(n.start), pkt.data.begin() + long(n.end));
        }
    }
    if (sps)
        m_config = config;
    if (!slice)
        return;   // только параметры — уйдут с опорным кадром
    if (idr && !sps)
        pkt.data.insert(pkt.data.begin(), m_config.begin(), m_config.end());
    std::vector<uint8_t> head;
    put64(head, uint64_t(pkt.timestampUs));
    put64(head, uint64_t(encoded));
    head.push_back(idr ? 1 : 0);
    ++m_statFrames;
    m_statBytes += pkt.data.size();
    const std::vector<uint8_t> msg = message(stream::Video, pkt.data.data(), pkt.data.size(), head);
    // метка экрана без дока — перед кадром, в той же посылке (зрителям с заставкой); кадр с меткой кодер не выдал —
    // она у следующего
    uint8_t away = 0;
    for (auto it = m_awayOf.begin(); it != m_awayOf.end() && it->first <= pkt.timestampUs;) {
        away = it->second;
        it = m_awayOf.erase(it);
    }
    std::vector<uint8_t> awayMsg, echoMsg;
    if (away) {
        std::vector<uint8_t> mark;
        put64(mark, uint64_t(pkt.timestampUs));
        mark.push_back(uint8_t(away - 1));
        awayMsg = message(stream::Away, mark.data(), mark.size());
    }
    // кадр по такту — Echo перед ним, в той же посылке (по каждому пути — вместе): зрителю, чей такт
    if (auto it = m_tickOf.find(pkt.timestampUs); it != m_tickOf.end()) {
        std::vector<uint8_t> echo;
        put64(echo, uint64_t(pkt.timestampUs));
        put64(echo, it->second);
        echoMsg = message(stream::Echo, echo.data(), echo.size());
        m_tickOf.erase(m_tickOf.begin(), std::next(it));   // и старше (кадры, что кодер не выдал)
    }
    sendVideo(msg, idr, echoMsg, awayMsg);
}

void StreamService::Worker::send(std::vector<uint8_t> msg, int path)
{
    if (path < 0 || size_t(path) >= m_paths.size())
        return;
    Path &p = m_paths[size_t(path)];
    p.out.push_back({std::move(msg), 0, false});
    flush(p);
}

void StreamService::Worker::sendAll(const std::vector<uint8_t> &msg)
{
    for (size_t k = 0; k < m_paths.size(); ++k)
        if (m_paths[k].joined)
            send(msg, int(k));
}

// Кадр — по всем путям всех сеансов; Echo (перед кадром) — зрителю, по чьему такту кадр, метка Away — зрителям с
// заставкой без дока. Путь не успевает (больше kMaxQueued кадров в очереди) — его неначатые кадры выбрасываются,
// дальше по нему — с опорного (wantKey: сразу, если главный зритель остался без кадров, иначе в покое).
void StreamService::Worker::sendVideo(const std::vector<uint8_t> &msg, bool key, const std::vector<uint8_t> &echo,
                                      const std::vector<uint8_t> &away)
{
    // есть прямой путь — кадры по нему, а не по SSH того же кабеля (там им тесно — весь смысл прямого); по путям
    // Wi-Fi — копии, как всегда: выдернули кабель — картинка уже идёт по Wi-Fi, без шва (docs/PI.md)
    for (size_t k = 0; k < m_paths.size(); ++k) {
        Path &p = m_paths[k];
        const Viewer *v = viewerOf(p);
        if (!v || !v->showing || !videoPath(p) || (p.needKey && !key))
            continue;
        p.needKey = false;
        p.waitKey = 0;
        p.waitMax = kWaitKeyMaxUs;
        int queued = 0;
        for (const Out &o : p.out)
            queued += o.video;
        if (queued >= kMaxQueued) {
            for (auto it = p.out.begin(); it != p.out.end();)
                it = it->video && it->sent == 0 ? p.out.erase(it) : it + 1;
            const bool now = wantKey(p, true);
            qCInfo(lcStream, "путь %s не успевает — кадры сброшены, по нему — с опорного%s",
                   p.direct ? "напрямую" : routeName(p.route), now ? "" : " (в покое: кадры идут другим путём или другим зрителям)");
            continue;
        }
        const bool withEcho = v->id == m_tickViewer && !echo.empty(), withAway = v->lock && !away.empty();
        if (!withEcho && !withAway) {
            p.out.push_back({msg, 0, true});
        } else {
            std::vector<uint8_t> all;
            all.reserve(msg.size() + echo.size() + away.size());
            if (withEcho)
                all.insert(all.end(), echo.begin(), echo.end());
            if (withAway)
                all.insert(all.end(), away.begin(), away.end());
            all.insert(all.end(), msg.begin(), msg.end());
            p.out.push_back({std::move(all), 0, true});
        }
        flush(p);
    }
}

// Звук стола — по всем путям сеанса (зритель берёт первую копию по метке): и по SSH кабеля, который при картинке
// напрямую кадров не везёт, — звук по нему не стоит за кадрами вовсе; по путям с кадрами — вперёд неначатых (опорный
// кадр — 600 КБ, 16 мс кабеля, по Wi-Fi — десятки; ядро держит не больше 64–128 КБ, остальное — в Path::out).
// Путь не успевает (больше kMaxSound кусков ждут) — старые куски прочь; кусок старше 50 мс (захват отдал скопившееся
// при запуске) не шлётся: приставка его всё равно не успеет сыграть, а задержку из-за него растила бы.
void StreamService::Worker::sendSound()
{
    std::deque<Shared::Sound> chunks;
    {
        std::lock_guard lock(m_s->m);
        chunks.swap(m_s->sound);
    }
    if (chunks.empty() || !m_audioViewer)
        return;
    constexpr int kMaxSound = 30;   // кусков по 10 мс
    const int64_t now = nowUs();
    for (const Shared::Sound &c : chunks) {
        if (now - c.first > 50000) {
            ++m_statSoundOld;
            continue;
        }
        ++m_statSound;
        m_statSoundWait = std::max(m_statSoundWait, (now - c.read) / 1000.0);       // снят → отдан связи
        m_statSoundAge = std::max(m_statSoundAge, (now - c.first) / 1000.0);        // первый отсчёт → отдан
        std::vector<uint8_t> head;
        put64(head, uint64_t(c.first));
        put32(head, 48000);
        head.push_back(2);
        head.push_back(stream::AudioF32);
        const std::vector<uint8_t> msg = message(stream::Audio, c.pcm.data(), c.pcm.size(), head);
        for (Path &p : m_paths) {
            if (!p.joined)
                continue;
            if (const Viewer *v = viewerOf(p); !v || !v->audio)
                continue;
            int waiting = 0;
            auto at = p.out.end();
            for (auto it = p.out.begin(); it != p.out.end(); ++it) {
                waiting += it->audio && it->sent == 0;
                if (at == p.out.end() && it->video && it->sent == 0)
                    at = it;
            }
            if (waiting >= kMaxSound) {   // самый старый неначатый кусок — прочь; место перед кадрами — заново
                p.out.erase(std::find_if(p.out.begin(), p.out.end(), [](const Out &o) { return o.audio && o.sent == 0; }));
                at = std::find_if(p.out.begin(), p.out.end(), [](const Out &o) { return o.video && o.sent == 0; });
            }
            p.out.insert(at, Out{msg, 0, false, true});
            flush(p);
        }
    }
}

bool StreamService::Worker::directJoined(int viewerId) const
{
    return std::any_of(m_paths.begin(), m_paths.end(),
                       [viewerId](const Path &p) { return p.direct && p.joined && p.viewer == viewerId; });
}

bool StreamService::Worker::videoPath(const Path &p) const
{
    return p.joined && (p.direct || p.route != stream::RouteCable || !directJoined(p.viewer));
}

bool StreamService::Worker::carries(const Path &p) const
{
    return videoPath(p) && !p.needKey;
}

// Путь будет с опорного. Кадры зрителю и так идут другим его путём (вставили кабель — по Wi-Fi; Wi-Fi отстал — по
// кабелю) — опорный подождёт покоя (run): в движении он дорог — сжимается ~55 мс, кадры за ним опаздывают, приёмник
// их выбрасывает и поднимает запас (замер 16 ноября 2026: кабель вставили посреди перетаскивания — два опорных,
// 4 кадра выброшено, задержка 43 → 70 мс на секунды). Других путей нет — опорный сейчас: иначе картинка стоит. Кроме
// отставшего (lagging) зрителя, когда стол смотрят и другие, а главный — не он: его опорный всем остальным — рывок;
// ему — в покое или через kLagKeyUs.
bool StreamService::Worker::wantKey(Path &p, bool lagging)
{
    p.needKey = true;
    // зрителю кадры сейчас не идут вовсе (стол показывают только чужие экраны, его дело — ввод): опорный не нужен;
    // выведут стол на его экран — попросит refresh()
    if (const Viewer *v = viewerOf(p); v && !v->showing) {
        p.waitKey = 0;
        return false;
    }
    if (std::any_of(m_paths.begin(), m_paths.end(),
                    [&](const Path &o) { return &o != &p && o.viewer == p.viewer && carries(o); })) {
        if (!p.waitKey)
            p.waitKey = nowUs();
        return false;
    }
    if (lagging && p.viewer != m_mainViewer
        && std::any_of(m_paths.begin(), m_paths.end(), [&](const Path &o) { return o.viewer != p.viewer && carries(o); })) {
        if (!p.waitKey)
            p.waitKey = nowUs();
        p.waitMax = kLagKeyUs;
        return false;
    }
    p.waitKey = 0;
    if (m_encoding)
        m_enc.requestKeyFrame();
    return true;
}

void StreamService::Worker::flush(Path &p)
{
    while (!p.out.empty()) {
        Out &o = p.out.front();
        const ssize_t n = ::send(p.fd, o.data.data() + o.sent, o.data.size() - o.sent, MSG_NOSIGNAL);
        if (n < 0)
            return;   // EAGAIN — дошлётся по POLLOUT; ошибка — путь закроется по POLLERR / POLLHUP
        o.sent += size_t(n);
        if (o.sent < o.data.size())
            return;
        p.out.pop_front();
    }
}

bool StreamService::Worker::readPath(size_t i)
{
    uint8_t buf[4096];
    for (;;) {
        const ssize_t n = recv(m_paths[i].fd, buf, sizeof buf, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            closePath(i);
            return false;
        }
        if (n < 0)
            break;
        m_paths[i].in.insert(m_paths[i].in.end(), buf, buf + n);
        m_paths[i].lastIn = nowUs();
    }
    // разбор: сообщение может сменить сеанс (Join) — путь ищется заново по номеру сокета
    const int fd = m_paths[i].fd;
    size_t at = 0;
    for (;;) {
        auto it = std::find_if(m_paths.begin(), m_paths.end(), [fd](const Path &p) { return p.fd == fd; });
        if (it == m_paths.end())
            return false;
        i = size_t(it - m_paths.begin());
        std::vector<uint8_t> &in = m_paths[i].in;
        if (in.size() - at < 5)
            break;
        const uint32_t len = get32(in.data() + at);
        if (len == 0 || len > 1 << 20) {
            closePath(i);
            return false;
        }
        if (in.size() - at - 4 < len)
            break;
        const std::vector<uint8_t> msg(in.begin() + long(at) + 4, in.begin() + long(at) + 4 + len);
        at += 4 + len;
        handle(i, msg[0], msg.data() + 1, msg.size() - 1);
    }
    auto it = std::find_if(m_paths.begin(), m_paths.end(), [fd](const Path &p) { return p.fd == fd; });
    if (it == m_paths.end())
        return false;
    it->in.erase(it->in.begin(), it->in.begin() + long(at));
    return true;
}

void StreamService::Worker::handle(size_t i, uint8_t type, const uint8_t *p, size_t n)
{
    StreamInput *input = m_service->m_input;
    if (m_paths[i].direct) {
        handleDirect(i, type, p, n);
        return;
    }
    const int fd = m_paths[i].fd;
    if (type == stream::Screens) {   // кто он и какие экраны несёт — с любого пути, и с тихого
        onScreens(i, p, n);
        return;
    }
    if (m_paths[i].park) {   // стол этому зрителю не показывают: тихий путь — только пинг (жив ли) и «стол сюда»
        if (type == stream::Ping && n >= 8) {
            std::vector<uint8_t> body;
            put64(body, get64(p));
            put64(body, uint64_t(nowUs()));
            send(message(stream::Pong, body.data(), body.size()), int(i));
        } else if (type == stream::Wake && m_paths[i].park == 1) {
            if (Viewer *v = viewerOf(m_paths[i]))
                wake(*v, p, n);
        }
        return;
    }
    if (!m_paths[i].joined) {
        // первое слово пути — Join: чей он (билет). Экраны зрителя уже известны (ещё один его путь, или он вернулся)
        // — сразу в сеанс или в тихие; нет — ждём Screens следующим словом
        if (type == stream::Join && n >= 9 && !m_paths[i].viewer) {
            const uint64_t token = get64(p);
            Viewer *v = nullptr;
            for (Viewer &x : m_viewers)
                if (token && x.token == token)
                    v = &x;
            if (!v) {
                Viewer fresh;
                fresh.id = m_nextViewer++;
                fresh.token = token;
                m_viewers.push_back(fresh);
                v = &m_viewers.back();
            }
            m_paths[i].viewer = v->id;
            m_paths[i].use = p[8] & 1;
            v->goneSince = 0;
            if (v->known)
                admit(i);
            return;
        }
        if (type == stream::Join)
            return;
        // слово без Screens: зритель без имени и экранов (старое окно) — один экран «screen»
        if (!m_paths[i].viewer) {
            Viewer fresh;
            fresh.id = m_nextViewer++;
            m_viewers.push_back(fresh);
            m_paths[i].viewer = fresh.id;
        }
        Viewer &anon = *viewerOf(m_paths[i]);
        if (!anon.known) {
            anon.known = true;
            anon.name = "viewer";
            anon.screens = {{"screen", "", "", 0, 0}};
            reconcile();
        }
        long k = indexOf(fd);
        if (k >= 0 && !m_paths[size_t(k)].joined && !m_paths[size_t(k)].park)
            admit(size_t(k));
        k = indexOf(fd);
        if (k < 0 || !m_paths[size_t(k)].joined)
            return;
        i = size_t(k);
    }
    Viewer *owner = viewerOf(m_paths[i]);
    if (!owner || !owner->live)
        return;
    Viewer &v = *owner;
    switch (type) {
    case stream::Join:
        break;
    case stream::Use:
        activate(i);
        break;
    case stream::DirectAsk:
        offerDirect(i);
        break;
    case stream::Input:   // ввод — по всем путям, с номером: выполнить один раз (какой копией — неважно)
        if (n >= 5) {
            const uint32_t seq = get32(p);
            if (!v.seen.insert(seq).second)
                break;
            if (v.seen.size() > 4096)
                v.seen.erase(v.seen.begin());
            if (p[4] != stream::Input)
                handle(i, p[4], p + 5, n - 5);
        }
        break;
    case stream::Ping:
        if (FILE *f = tickTrace(); f && n >= 8)
            fprintf(f, "p,%lld,%llu,0\n", (long long)nowUs(), (unsigned long long)get64(p)), fflush(f);
        if (n >= 8) {
            std::vector<uint8_t> body;
            put64(body, get64(p));
            put64(body, uint64_t(nowUs()));
            send(message(stream::Pong, body.data(), body.size()), int(i));
        }
        break;
    case stream::Touch:
        m_s->lastInputUs = nowUs();
        if (n >= 10) {
            const int phase = p[0], id = p[1];
            v.touching = phase < 2;
            const double x = getF(p + 2), y = getF(p + 6);
            post([input, phase, id, x, y] { input->touch(phase, id, x, y); });
        }
        break;
    case stream::Scroll:
        m_s->lastInputUs = m_wheelUs = nowUs();
        if (n >= 17) {
            const double x = getF(p), y = getF(p + 4), dx = getF(p + 8), dy = getF(p + 12);
            const int phase = p[16];
            post([input, x, y, dx, dy, phase] { input->scroll(x, y, dx, dy, phase); });
        }
        break;
    case stream::Key:
        m_s->lastInputUs = m_keyUs = nowUs();
        if (n >= 9) {
            const bool down = p[0];
            const int key = int(get32(p + 1)), mods = int(get32(p + 5));
            const QString text = QString::fromUtf8(reinterpret_cast<const char *>(p + 9), qsizetype(n - 9));
            post([input, down, key, mods, text] { input->key(down, key, mods, text); });
        }
        break;
    case stream::Nav:
        m_s->lastInputUs = nowUs();
        if (n >= 1) {
            const int action = p[0];
            post([input, action] { input->nav(action); });
        }
        break;
    case stream::Tick: {   // [u64 значение зрителя — вернётся в Echo с кадром]; такт — только главного экрана
        if (&v != mainViewer())
            break;
        m_tickViewer = v.id;
        const uint64_t value = n >= 8 ? get64(p) : 0;
        if (FILE *f = tickTrace())
            fprintf(f, "k,%lld,%llu,%d\n", (long long)nowUs(), (unsigned long long)value, int(m_paths[i].direct)),
                fflush(f);
        post([svc = m_service, value] { svc->onTick(value); });
        break;
    }
    case stream::Reveal:
        post([svc = m_service] { svc->onReveal(); });
        break;
    case stream::AwayShown:
        post([svc = m_service] { svc->onAwayShown(); });
        break;
    case stream::Wake:   // сеанс идёт (стол на другом экране этого зрителя), а будят ещё один его экран
        wake(v, p, n);
        break;
    case stream::KeyFrame:
        if (m_encoding) {
            m_enc.requestKeyFrame();
            post([svc = m_service] { svc->requestFrame(); });
        }
        break;
    case stream::Viewer:   // имя зрителя — теперь в Screens; это — от старого окна
        if (v.name.empty() || v.name == "viewer") {
            v.name = QString::fromUtf8(reinterpret_cast<const char *>(p), qsizetype(n)).left(32).toStdString();
            refresh();
        }
        break;
    case stream::Monitor: {   // монитор зрителя (приставка): «Настройки» стола показывают его
        const QVariantMap m = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(p), qsizetype(n)))
                                  .object().toVariantMap();
        qCInfo(lcStream, "монитор зрителя: %s", QByteArray(reinterpret_cast<const char *>(p), qsizetype(n)).constData());
        post([svc = m_service, m] { svc->onMonitor(m); });
        break;
    }
    case stream::Brightness:   // яркость монитора зрителя после клавиши — плашка на столе
        if (n >= 1) {
            const int percent = p[0];
            post([svc = m_service, percent] { emit svc->brightnessShown(percent); });
        }
        break;
    case stream::PadTouch:   // пальцы на трекпаде зрителя — «Проверке» в «Настройках» стола
        if (n >= 3 && n >= 3 + size_t(p[2]) * 3) {
            QVariantList fingers;
            for (int i = 0; i < p[2]; ++i)
                fingers.append(QVariantMap{{QStringLiteral("x"), p[3 + i * 3] / 255.0}, {QStringLiteral("y"), p[4 + i * 3] / 255.0},
                                           {QStringLiteral("off"), p[5 + i * 3] != 0}});
            const QVariantMap touch{{QStringLiteral("mode"), int(p[0])}, {QStringLiteral("buttons"), int(p[1])},
                                    {QStringLiteral("fingers"), fingers}};
            post([svc = m_service, touch] { svc->onPadTouch(touch); });
        }
        break;
    case stream::PadAction:   // смахивание трекпада зрителя — действие стола (меню N)
        if (n >= 1) {
            const int action = p[0];
            post([svc = m_service, action] { emit svc->padAction(action); });
        }
        break;
    case stream::Layout:   // раскладка клавиатуры зрителя: при подключении и когда сменилась
        if (n >= 1) {
            const QString layout = p[0] ? QStringLiteral("RU") : QStringLiteral("EN");
            post([svc = m_service, layout] { svc->onLayout(layout); });
        }
        break;
    case stream::Mode:   // что смотреть этому зрителю; идёт стол, если его просит хоть один (applySource)
        if (n >= 1 && (p[0] == 0 || m_service->m_desktop)) {
            v.mode = p[0] ? Desktop : Phone;
            applySource();
        }
        break;
    case stream::DeskSize:   // u16 высота [, u16 ширина, u16 масштаб × 1000]; без них — окно Mac: 1280, 1,5, высота 720…960
        if (n >= 2 && m_service->m_desktop) {   // форма стола — главного экрана: у прочих помнится на случай, если станут им
            v.sized = true;
            // для какого экрана форма: зритель с двумя экранами (приставка) говорит сам — хвостом сообщения; иначе —
            // для его единственного
            v.sizedFor = n > 6 ? std::string(reinterpret_cast<const char *>(p + 6), std::min<size_t>(n - 6, 32))
                               : v.screens.empty() ? std::string() : v.screens.front().id;
            v.deskW = n >= 6 ? p[2] | p[3] << 8 : 1280;
            v.deskH = n >= 6 ? p[0] | p[1] << 8 : std::clamp(p[0] | p[1] << 8, 720, 960);
            v.deskScale = n >= 6 ? p[4] | p[5] << 8 : 1500;
            if (&v == mainViewer())
                applySize();
        }
        break;
    case stream::Want: {
        const QJsonObject w = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(p), qsizetype(n))).object();
        v.deskFps = std::clamp(w.value(QStringLiteral("fps")).toInt(kDeskFps), 30, 120);
        v.encodeFps = std::clamp(w.value(QStringLiteral("encodeFps")).toInt(v.deskFps), v.deskFps, 240);
        v.hevc = w.value(QStringLiteral("codec")).toString() == QStringLiteral("hevc");
        v.lock = !m_service->m_localDesktop && w.value(QStringLiteral("lock")).toBool();
        v.audio = w.value(QStringLiteral("audio")).toBool();
        v.wanted = true;
        applyWants();
        break;
    }
    case stream::Pointer:   // приставка шлёт, только когда мышь сдвинулась или кнопка сменилась
        m_s->lastInputUs = nowUs();
        if (n >= 9) {
            // кнопку держит другой зритель (тащит окно) — его движение этот не сбивает
            if (std::any_of(m_viewers.begin(), m_viewers.end(),
                            [&](const Viewer &o) { return &o != &v && o.live && o.buttons; }))
                break;
            const int buttons = v.buttons = p[0];
            const double x = getF(p + 1), y = getF(p + 5);
            post([input, buttons, x, y] { input->pointer(buttons, x, y); });
            if (liveCount() > 1)
                sendCursorAt(v, x, y);
        }
        break;
    default:
        break;
    }
}

// ---------- поток интерфейса ----------

StreamService::StreamService(QObject *parent)
    : QObject(parent)
    , m_shared(std::make_shared<Shared>())
    , m_gl{std::make_unique<Gl>(), std::make_unique<Gl>()}
    , m_input(new StreamInput(this))
{
    m_shared->width[Phone] = envInt("NAOMI_STREAM_WIDTH", 720);
    m_enabled = QSettings(QStringLiteral("naomi"), QStringLiteral("shell")).value(QStringLiteral("stream/enabled"), true).toBool();
    if (!m_enabled)
        qCInfo(lcStream, "трансляция выключена (плитка «трансляция») — зрители будут ждать тихо");
    {
        QSettings st(QStringLiteral("naomi"), QStringLiteral("shell"));
        m_adopt = !st.contains(QStringLiteral("screens/on"));
        m_on = st.value(QStringLiteral("screens/on")).toStringList();
        m_main = st.value(QStringLiteral("screens/main")).toString();
        m_order = st.value(QStringLiteral("screens/order")).toStringList();
        m_known = QJsonDocument::fromJson(st.value(QStringLiteral("screens/known")).toByteArray()).object().toVariantMap();
        m_on.removeAll(QString());
        m_last = st.value(QStringLiteral("screens/last")).toString();
        if (m_enabled && m_on.isEmpty() && !m_adopt) {   // стол не выведен ни на один экран — значит, выключен
            m_enabled = false;
            st.setValue(QStringLiteral("stream/enabled"), false);
        }
        qCInfo(lcStream, "экраны: стол выведен на [%s], главный — %s", qPrintable(m_on.join(QStringLiteral(", "))),
               m_main.isEmpty() ? "нет" : qPrintable(m_main));
        m_lights = QJsonDocument::fromJson(st.value(QStringLiteral("screens/light")).toByteArray()).object().toVariantMap();
        std::lock_guard lock(m_shared->m);
        for (auto it = m_lights.constBegin(); it != m_lights.constEnd(); ++it)
            m_shared->lights[it.key().toStdString()] = {it->toMap().value(QStringLiteral("auto"), true).toBool(),
                                                        it->toMap().value(QStringLiteral("level"), 60).toInt()};
    }
    pushPlan();
    m_revealTimer.setSingleShot(true);
    m_revealTimer.setInterval(4000);
    connect(&m_revealTimer, &QTimer::timeout, this, [this] {
        if (!m_deskAway || !m_connected || m_leaving)
            return;
        qCInfo(lcStream, "стол ждал, пока экран покажет обои, 4 с — возвращается сам");
        setAway(false, true);
        m_awayMark = 1 + stream::AwayBack;
        requestFrame();
    });
    connect(m_input, &StreamInput::activity, this, &StreamService::activity);
    connect(m_input, &StreamInput::volumeKey, this, &StreamService::volumeKey);
    connect(m_input, &StreamInput::screenshotKey, this, &StreamService::screenshotKey);
    connect(m_input, &StreamInput::pointerSent, this, &StreamService::checkCursor);
    new DeskTestInput(m_input, this);   // проверочная мышь стола и «Экраны» по SSH (desk_test_input.h)
    // клавиши плеера — играющему (MPRIS); клавиша блокировки — выключатель трансляции, как «закрыть сессию» в меню
    // стола и плитка в шторке телефона: стол смотрят — выключить, выключена — включить (зритель вернётся сам); зритель
    // смотрит экран телефона — не трогать. «Луна» (F6 у Apple) — то же, но монитор приставки, уходя, приглушается
    // (docs/DESKTOP.md, «Клавиши плеера»)
    auto *media = new MediaKeys(this);
    connect(m_input, &StreamInput::mediaKey, media, &MediaKeys::press);
    connect(m_input, &StreamInput::brightnessKey, this, [this](int step) {   // монитор — у приставки: шаг ей
        if (!m_lockViewer || !m_connected)
            return;
        {
            std::lock_guard lock(m_shared->m);
            m_shared->brightnessStep += step;
        }
        m_shared->wake();
    });
    connect(m_input, &StreamInput::lockKey, this, [this](bool dim) {
        if (m_enabled && !(m_connected && m_source == Desktop))
            return;
        qCInfo(lcStream, "клавиша %s: стол %s", dim ? "«луна»" : "блокировки", m_enabled ? "выключается" : "включается");
        if (dim && m_enabled && m_lockViewer)   // «луна»: монитор приставки — до 5 % сразу, что бы ни стояло в настройках
            leaveIdle(5);
        else
            setEnabled(!m_enabled);
    });
    // клавиатура самого телефона (Bluetooth): клавиши — путём клавиш зрителя; раскладка у неё и у зрителя одна
    m_keyboard = new HardKeyboard(this);
    connect(m_keyboard, &HardKeyboard::key, m_input, &StreamInput::key);
    connect(m_keyboard, &HardKeyboard::presentChanged, this, &StreamService::layoutChanged);
    connect(m_keyboard, &HardKeyboard::layoutToggled, this, [this](bool ru) {
        if (m_layout.isEmpty() || !m_connected) {
            emit layoutChanged();
            return;
        }
        m_keyboard->setRussian(m_layout == "RU");   // держит зритель: сменит он — и ответит (onLayout)
        setLayout(ru ? QStringLiteral("RU") : QStringLiteral("EN"));
    });
    m_cursorPoll.setInterval(50);
    connect(&m_cursorPoll, &QTimer::timeout, this, &StreamService::checkCursor);
    m_gpuIdle.setSingleShot(true);
    m_gpuIdle.setInterval(3000);
    connect(&m_gpuIdle, &QTimer::timeout, this, [this] { holdGpu(false); });
    m_wifiIdle.setSingleShot(true);
    m_wifiIdle.setInterval(10000);
    connect(&m_wifiIdle, &QTimer::timeout, this, [this] {
        m_moving = false;
        holdWifi();
    });
    // выход из стола без дела — проверка раз в 5 с, пока он выбран (NAOMI_DESK_IDLE_EXIT=с — свой срок, для проверок)
    m_idleExitEnv = envInt("NAOMI_DESK_IDLE_EXIT", 0);
    m_idleExitTimer.setInterval(m_idleExitEnv > 0 && m_idleExitEnv < 30 ? 1000 : 5000);
    connect(&m_idleExitTimer, &QTimer::timeout, this, &StreamService::checkIdleExit);
    if (m_idleExitEnv > 0)
        m_idleExitTimer.start();
    m_leaveTimer.setSingleShot(true);
    connect(&m_leaveTimer, &QTimer::timeout, this, [this] {
        qCInfo(lcStream, "стол уходил — ответа экрана нет, дальше так");
        finishLeave();
    });
}

StreamService::~StreamService()
{
    m_capture.stop();
    m_worker.reset();
}

void StreamService::setSendAudio(bool on)
{
    if (on == m_sendAudio)
        return;
    m_sendAudio = on;
    updateCapture();
    emit changed();
}

void StreamService::setShotSelecting(bool on)
{
    if (on == m_shotSelecting)
        return;
    m_shotSelecting = on;
    m_input->setShotSelecting(on);
    emit shotSelectingChanged();
}

// Ключи и порядок — как в сообщении Mouse (stream_protocol.h); всё числами, булевы — 0 и 1
void StreamService::setMouse(const QVariantMap &mouse)
{
    if (mouse == m_mouse)
        return;
    m_mouse = mouse;
    QJsonObject o;
    for (const char *key : {"speed", "accel", "natural", "lines", "smooth", "soft", "wheelAccel"}) {
        const QVariant v = mouse.value(QLatin1String(key));
        if (v.isValid())
            o.insert(QLatin1String(key), v.toInt());
    }
    const QByteArray j = QJsonDocument(o).toJson(QJsonDocument::Compact);
    {
        std::lock_guard lock(m_shared->m);
        m_shared->mouse = j.toStdString();
        m_shared->settingsDirty = true;
    }
    m_shared->wake();
    qCInfo(lcStream, "мышь стола: %s", j.constData());
    emit mouseChanged();
}

// Ключи — как в сообщении Pad (stream_protocol.h); всё числами, булевы — 0 и 1
void StreamService::setTrackpad(const QVariantMap &trackpad)
{
    if (trackpad == m_trackpad)
        return;
    m_trackpad = trackpad;
    QJsonObject o;
    for (const char *key : {"speed", "accel", "tap", "right", "drag", "hold", "natural", "sspeed", "inertia", "pinch", "jitter",
                            "palm", "typing", "pin", "swSide", "swUp", "swDown", "show"}) {
        const QVariant v = trackpad.value(QLatin1String(key));
        if (v.isValid())
            o.insert(QLatin1String(key), v.toInt());
    }
    const QByteArray j = QJsonDocument(o).toJson(QJsonDocument::Compact);
    {
        std::lock_guard lock(m_shared->m);
        m_shared->pad = j.toStdString();
        m_shared->settingsDirty = true;
    }
    m_shared->wake();
    qCInfo(lcStream, "трекпад стола: %s", j.constData());
    emit trackpadChanged();
}

void StreamService::onPadTouch(const QVariantMap &touch)
{
    m_padTouch = touch;
    emit padTouchChanged();
}

// Ключи — как в сообщении Screen (stream_protocol.h); всё числами, булевы — 0 и 1
void StreamService::setScreen(const QVariantMap &screen)
{
    if (screen == m_screen)
        return;
    if (const int exitS = screen.value(QStringLiteral("exit")).toInt(); exitS != m_screen.value(QStringLiteral("exit")).toInt())
        qCInfo(lcStream, "выход из стола без дела: %s", exitS > 0 ? qPrintable(QStringLiteral("через %1 мин").arg(exitS / 60))
                                                                   : "никогда");
    m_screen = screen;
    QJsonObject o;
    for (const char *key : {"width", "sharp", "hz", "clock", "clockX", "clockY", "sleep", "dim", "dimLevel", "wakeMove"}) {
        const QVariant v = screen.value(QLatin1String(key));
        if (v.isValid())
            o.insert(QLatin1String(key), v.toInt());
    }
    const QByteArray j = QJsonDocument(o).toJson(QJsonDocument::Compact);
    {
        std::lock_guard lock(m_shared->m);
        m_shared->screen = j.toStdString();
        m_shared->settingsDirty = true;
    }
    m_shared->wake();
    qCInfo(lcStream, "экран стола: %s", j.constData());
    if (m_idleExitEnv > 0 || m_screen.value(QStringLiteral("exit")).toInt() > 0)
        m_idleExitTimer.start();
    else
        m_idleExitTimer.stop();
    emit screenChanged();
}

// Выход из стола без дела («Настройки» → «Без дока», exit в screen, с; 0 — никогда; телефону, приставке не шлётся):
// приставка смотрит стол, а ни ввода (мышь, клавиши, колесо), ни звука стола столько — стол уходит, как «закрыть
// сессию» (setEnabled(false), её часы и сон или приглушение — дальше как всегда); вернуть — клавишей или щелчком у
// приставки. Звук считается: играет музыка или видео — человек тут, хоть и не трогает мышь. Отсчёт — с последнего
// ввода или звука, но не раньше, чем стол показан (вернулся — заново)
void StreamService::checkIdleExit()
{
    const int exitS = m_idleExitEnv > 0 ? m_idleExitEnv : m_screen.value(QStringLiteral("exit")).toInt();
    if (exitS <= 0 || !m_enabled || !m_lockViewer || !m_connected || m_source != Desktop || m_deskAway || m_leaving) {
        m_idleFrom = 0;
        return;
    }
    const int64_t now = nowUs();
    if (!m_idleFrom)
        m_idleFrom = now;
    const int64_t last = std::max({m_idleFrom, m_shared->lastInputUs.load(), m_shared->lastSoundUs.load()});
    if (now - last < int64_t(exitS) * 1000000)
        return;
    qCInfo(lcStream, "без дела %d с — ни мыши, ни клавиш, ни звука: стол уходит", exitS);
    m_idleFrom = 0;
    setEnabled(false);
}

// Форма курсора стола (docs/DESKTOP.md, «Курсор»): её ставит Qt окну стола — края окон (MouseArea.cursorShape),
// поля ввода, вкладки; над страницей браузера — его собственная (WebLink.cursorShape). Сменилась — зрителю (Cursor):
// приставка рисует свой набор, окно Mac — системный курсор. Проверка — после каждого движения мыши и раз в 50 мс:
// страница меняет форму и сама (ссылка появилась под стоящей мышью)
void StreamService::checkCursor()
{
    const StreamScreen *screen = m_localDesktop ? m_localDesktop : m_source == Desktop ? m_desktop : nullptr;
    if (!screen || !m_connected)
        return;
    uint8_t shape = stream::CursorArrow;
    switch (screen->window()->cursor().shape()) {
    case Qt::PointingHandCursor: case Qt::OpenHandCursor: shape = stream::CursorHand; break;
    case Qt::IBeamCursor: shape = stream::CursorText; break;
    case Qt::SizeHorCursor: case Qt::SplitHCursor: shape = stream::CursorSizeH; break;
    case Qt::SizeVerCursor: case Qt::SplitVCursor: shape = stream::CursorSizeV; break;
    case Qt::SizeFDiagCursor: shape = stream::CursorSizeFDiag; break;
    case Qt::SizeBDiagCursor: shape = stream::CursorSizeBDiag; break;
    case Qt::SizeAllCursor: shape = stream::CursorMove; break;
    case Qt::ClosedHandCursor: case Qt::DragMoveCursor: case Qt::DragCopyCursor: case Qt::DragLinkCursor:
        shape = stream::CursorGrab; break;
    case Qt::WaitCursor: case Qt::BusyCursor: shape = stream::CursorBusy; break;
    case Qt::ForbiddenCursor: shape = stream::CursorNo; break;
    case Qt::CrossCursor: shape = stream::CursorCross; break;
    case Qt::BlankCursor: shape = stream::CursorHidden; break;
    default: break;
    }
    if (shape == m_cursor)
        return;
    m_cursor = shape;
    {
        std::lock_guard lock(m_shared->m);
        m_shared->cursor = shape;
        m_shared->cursorDirty = true;
    }
    m_shared->wake();
}

void StreamService::onMonitor(const QVariantMap &monitor)
{
    if (monitor == m_monitor)
        return;
    m_monitor = monitor;
    emit monitorChanged();
}

void StreamService::onLayout(const QString &layout)
{
    if (layout == m_layout)
        return;
    if (!layout.isEmpty()) {
        qCInfo(lcStream, "раскладка зрителя: %s", qPrintable(layout));
        m_keyboard->setRussian(layout == "RU");   // клавиатура телефона печатает той же
    }
    m_layout = layout;
    emit layoutChanged();
}

QString StreamService::layout() const
{
    if (!m_layout.isEmpty())
        return m_layout;
    return !m_keyboard->present() ? QString() : m_keyboard->russian() ? QStringLiteral("RU") : QStringLiteral("EN");
}

void StreamService::setLayout(const QString &layout)
{
    if ((layout != "EN" && layout != "RU") || layout == this->layout())
        return;
    if (m_layout.isEmpty() || !m_connected) {   // зритель раскладку не держит — клавиатуре телефона
        if (m_keyboard->present()) {
            m_keyboard->setRussian(layout == "RU");
            emit layoutChanged();
        }
        return;
    }
    {
        std::lock_guard lock(m_shared->m);
        m_shared->layoutWant = layout == "RU" ? 1 : 0;
    }
    m_shared->wake();
}

void StreamService::injectKey(bool down, int qtKey, int modifiers, const QString &text)
{
    if (m_input)
        m_input->key(down, qtKey, modifiers, text);
}

void StreamService::onAudioViewer(bool on)
{
    if (on == m_audio)
        return;
    m_audio = on;
    updateCapture();
    emit changed();
}

// Монитор выхода «Монитор» (null-sink naomi.desk у PulseAudio, его держит naomid): куски по 10 мс, 48 кГц стерео float —
// как играет Chromium, без пересчёта частоты. Тишина (все нули — выход ничего не играет) зрителю не идёт: приставка сама
// играет тишину, а связь не занята (звук — 1,5 Мбит/с по каждому пути)
void StreamService::updateCapture()
{
    // Захват — всё время, пока зритель играет звук, а не только пока звук на «Мониторе»: переключили на телефон и
    // обратно — захват не начинается заново (первые 50–100 мс после запуска PulseAudio отдаёт скопившимся, старым)
    if (const bool send = m_audio && m_sendAudio && m_connected; send != m_shared->sendSound) {
        m_shared->sendSound = send;
        qCInfo(lcStream, "звук стола зрителю — %s", send ? "с выхода «Монитор»" : "нет");
    }
    const bool want = m_audio && m_connected;
    if (want == m_capture.running())
        return;
    if (!want) {
        m_capture.stop();
        m_deskSound->clear();
        std::lock_guard lock(m_shared->m);
        m_shared->sound.clear();
        return;
    }
    // float: выход «Монитор» мешает и ставит громкость в float (naomid), его монитор отдаётся как есть — громкость не
    // съедает разрядов (в s16 при 40 % от звука оставалось ~11 бит, и колонки усиливали шум округления)
    // Каждый кусок, и тишина, — ещё и эквалайзеру окна звука (m_deskSound): ему не нужен свой захват
    m_capture.start("naomi.desk.monitor", 48000, 2, 10, [s = m_shared, h = m_deskSound](int64_t first, const void *data,
                                                                                         size_t frames) {
        h->push(static_cast<const float *>(data), frames, 2);
        // звук играет (громче −80 дБ) — стол без дела не закрывать (checkIdleExit): музыка, видео
        const auto *f = static_cast<const float *>(data);
        if (std::any_of(f, f + frames * 2, [](float v) { return v > 1e-4f || v < -1e-4f; }))
            s->lastSoundUs = nowUs();
        const auto *x = static_cast<const uint8_t *>(data);
        const size_t n = frames * 2 * 4;
        if (!s->sendSound || std::all_of(x, x + n, [](uint8_t v) { return v == 0; }))
            return;
        {
            std::lock_guard lock(s->m);
            s->sound.push_back({first, nowUs(), std::vector<uint8_t>(x, x + n)});
            while (s->sound.size() > 50)
                s->sound.pop_front();
        }
        s->wake();
    }, AudioCapture::F32, "naomi.desk", [h = m_deskSound](bool idle) {
        if (idle)
            h->clear();   // эквалайзер — тишина, а не последний кусок перед паузой
    });
}

void StreamService::attach(QQuickWindow *window, StreamScreen *localDesktop, int sceneRotation)
{
    m_window = window;
    m_input->setWindow(window);
    if (localDesktop) {
        m_localDesktop = localDesktop;
        m_input->setDesktop(localDesktop);
        connect(m_keyboard, &HardKeyboard::key, this, [this](bool down) { if (down) emit activity(); });
        // Первый запуск панели и проба Venus выполняются отдельно.
        if (qEnvironmentVariableIntValue("NAOMI_PIPA_STREAM") != 1)
            return;
    }
    // кадр потока — той же формы, что экран: ширина своя (720), высота в той же доле, чётная
    const QSize screen = window->screen()->handle()->geometry().size();
    m_gl[Phone]->source = screen;
    {
        std::lock_guard lock(m_shared->m);
        const QSize image = localDesktop ? screen.transposed() : screen;
        int &w = m_shared->width[Phone], &h = m_shared->height[Phone];
        if (localDesktop)
            w = std::clamp(envInt("NAOMI_STREAM_WIDTH", 1280), 360, image.width()) / 2 * 2;
        h = qRound(double(image.height()) * w / image.width() / 2) * 2;
        if (localDesktop && sceneRotation % 180 == 0)
            std::swap(w, h);
        m_shared->sceneRotation = m_shared->requestedRotation = sceneRotation;
    }
    connect(window, &QQuickWindow::afterRendering, this, [this] { capture(Phone); }, Qt::DirectConnection);
    m_worker = std::make_unique<Worker>(this, m_shared);
}

void StreamService::setNativeRotation(int rotation)
{
    if (!m_localDesktop || !m_worker)
        return;
    std::lock_guard lock(m_shared->m);
    m_shared->requestedRotation = rotation;
    m_shared->wake();
}

// рабочий стол — до attach() (служба начинает слушать в attach)
void StreamService::attachDesktop(StreamScreen *desktop)
{
    m_desktop = desktop;
    m_gl[Desktop]->source = desktop->pixelSize();
    {
        std::lock_guard lock(m_shared->m);
        m_shared->width[Desktop] = desktop->pixelSize().width();
        m_shared->height[Desktop] = desktop->pixelSize().height();
    }
    connect(desktop->window(), &QQuickWindow::afterRendering, this, [this] { capture(Desktop); },
            Qt::DirectConnection);
}

// Стол меняет размер (Worker::resizeDesktop, кодер уже остановлен): его буферы кадра — прочь (в его контексте GL),
// стол — новой формы, буферы выделит следующий кадр стола (capture) под новый размер — и кодер запустится снова.
void StreamService::onDesktopFps(int fps, bool named)
{
    if (m_desktop)
        m_desktop->setMaxFps(fps, named);
}

void StreamService::onDesktopSize(QSize logical, double scale)
{
    if (!m_desktop)
        return;
    Gl &g = *m_gl[Desktop];
    if (g.ready && m_desktop->makeCurrent()) {
        {   // буферы уходят — у кодера их больше нет: следующий кадр выложит новые, и кодер запустится с ними
            std::lock_guard lock(m_shared->m);
            if (m_shared->owner == Desktop) {
                m_shared->owner = -1;
                m_shared->buffersReady = false;
                m_shared->inputs.clear();
                m_shared->outputs.clear();
                m_shared->encoderStale = m_shared->running;
            }
        }
        releaseGl(g);
        m_desktop->doneCurrent();
        m_shared->wake();
    }
    m_desktop->setGeometry(logical, scale);
    g.source = m_desktop->pixelSize();
    {
        std::lock_guard lock(m_shared->m);
        m_shared->deskResize = std::max(0, m_shared->deskResize - 1);
    }
    m_desktop->requestFrame();
}

// Выключатель — плитка «трансляция» в шторке и «Настройках»; выбор помнится (shell.conf, stream/enabled)
void StreamService::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    // Включают, а стол не выведен ни на один экран (все поставили «нет» — он и выключился): вернуть последний, с
    // которого его сняли; нет такого — первый на связи; нет и его — первый, что выйдет на связь
    if (on && m_on.isEmpty()) {
        QString id = m_order.contains(m_last) ? m_last : QString();
        if (id.isEmpty() && !m_online.isEmpty())
            id = m_online.first().toMap().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            m_adopt = true;
            applyPlan(true, m_on, m_main);
            m_adopt = true;
            return;
        }
        applyPlan(true, {id}, id);
        return;
    }
    applyPlan(on, m_on, m_main);
}

// ---------- экраны (docs/SCREENS.md) ----------

QStringList StreamService::liveScreens(bool enabled, const QStringList &on) const
{
    QStringList live;
    if (!enabled)
        return live;
    for (const QVariant &v : m_online)
        if (const QString id = v.toMap().value(QStringLiteral("id")).toString(); on.contains(id))
            live << id;
    return live;
}

void StreamService::savePlan()
{
    QSettings st(QStringLiteral("naomi"), QStringLiteral("shell"));
    st.setValue(QStringLiteral("stream/enabled"), m_enabled);
    st.setValue(QStringLiteral("screens/on"), m_on);
    st.setValue(QStringLiteral("screens/main"), m_main);
}

void StreamService::pushPlan()
{
    m_pushedEnabled = m_enabled;
    m_pushedOn = m_on;
    {
        std::lock_guard lock(m_shared->m);
        m_shared->planEnabled = m_enabled;
        m_shared->planOn.clear();
        for (const QString &id : m_on)
            m_shared->planOn.push_back(id.toStdString());
        m_shared->planMain = m_main.toStdString();
        m_shared->planDirty = true;
    }
    m_shared->wake();
}

// Новый план: стол включён или нет, на какие экраны выведен, какой главный. Помнится сразу. Потоку службы — тоже
// сразу, кроме одного случая: стол уходит со всех экранов, что его показывали (выключили; перенесли на другой экран), а
// среди них есть экран с заставкой без дока — тогда сначала панель и окна уходят анимацией (экраны в те же обновления
// проявляют часы), последний кадр — одни обои, опорный; план — когда этот кадр на экране (AwayShown, finishLeave).
// Передумали посреди ухода (стол остаётся на экране, с которого уходил) — возвращается, сеанс и не кончался.
void StreamService::applyPlan(bool enabled, QStringList on, QString main)
{
    on.removeDuplicates();
    on.removeAll(QString());
    if (!on.contains(main))
        main = on.value(0);
    if (enabled == m_enabled && on == m_on && main == m_main)
        return;
    if (enabled != m_enabled)
        qCInfo(lcStream, "трансляция %s", enabled ? "включена" : "выключена");
    m_enabled = enabled;
    m_on = on;
    m_main = main;
    m_adopt = false;
    savePlan();
    qCInfo(lcStream, "экраны: стол %s, выведен на [%s], главный — %s", enabled ? "включён" : "выключен",
           qPrintable(on.join(QStringLiteral(", "))), main.isEmpty() ? "нет" : qPrintable(main));
    const QStringList was = liveScreens(m_pushedEnabled, m_pushedOn), will = liveScreens(m_enabled, m_on);
    const bool stays = std::any_of(was.begin(), was.end(), [&](const QString &id) { return will.contains(id); });
    if (m_leaving) {
        if (stays) {
            m_leaving = false;
            m_leaveTimer.stop();
            setAway(false, true);
            m_awayMark = 1 + stream::AwayBack;
            requestFrame();
            pushPlan();
        }   // иначе стол уходит дальше: план — когда уйдёт (finishLeave берёт последний)
    } else if (m_worker && m_lockViewer && m_connected && m_source == Desktop && !m_deskAway && !was.isEmpty() && !stays) {
        qCInfo(lcStream, "стол уходит с экранов: %s", qPrintable(was.join(QStringLiteral(", "))));
        m_leaving = true;
        m_finalAsked = false;
        setAway(true, true);
        m_awayMark = 1 + stream::AwayLeave;
        requestFrame();
        m_leaveTimer.start(2000);
    } else {
        pushPlan();
    }
    emit changed();
    emit screensChanged();
}

// Экраны на связи — от потока службы. Новые — в список известных (помнятся: в «Экранах» видны и те, что не на связи)
void StreamService::onScreens(const QVariantList &online, const QString &main)
{
    m_online = online;
    m_mainNow = main;
    bool fresh = false;
    for (const QVariant &v : online) {
        const QVariantMap s = v.toMap();
        const QString id = s.value(QStringLiteral("id")).toString();
        QVariantMap rec;
        for (const char *key : {"name", "kind", "w", "h", "viewer", "wake"})
            rec.insert(QLatin1String(key), s.value(QLatin1String(key)));
        if (m_known.value(id).toMap() != rec) {
            m_known.insert(id, rec);
            fresh = true;
        }
        if (!m_order.contains(id)) {
            m_order << id;
            fresh = true;
        }
    }
    if (fresh) {
        QSettings st(QStringLiteral("naomi"), QStringLiteral("shell"));
        st.setValue(QStringLiteral("screens/order"), m_order);
        st.setValue(QStringLiteral("screens/known"),
                    QJsonDocument(QJsonObject::fromVariantMap(m_known)).toJson(QJsonDocument::Compact));
    }
    // выбора ещё не было (первый запуск с экранами): первый экран на связи — выведен и главный, как было всегда
    if (m_adopt && m_on.isEmpty() && !online.isEmpty()) {
        const QString id = online.first().toMap().value(QStringLiteral("id")).toString();
        qCInfo(lcStream, "экраны: выбора ещё не было — стол на первый экран на связи (%s)", qPrintable(id));
        applyPlan(m_enabled, {id}, id);
    }
    emit screensChanged();
}

// Форма экрана словами: ближайшая из привычных (16:9, 16:10, 3:2…), иначе — как есть
static QString aspectName(int w, int h)
{
    if (w <= 0 || h <= 0)
        return QString();
    static const struct { double r; const char *name; } known[] = {{16.0 / 9, "16:9"}, {16.0 / 10, "16:10"}, {3.0 / 2, "3:2"},
                                                                    {4.0 / 3, "4:3"}, {21.0 / 9, "21:9"}, {5.0 / 4, "5:4"},
                                                                    {1.0, "1:1"}};
    const double r = double(std::max(w, h)) / std::min(w, h);
    for (const auto &k : known)
        if (std::fabs(r - k.r) < 0.035)
            return QString::fromLatin1(k.name);
    return QStringLiteral("%1:10").arg(qRound(r * 10));
}

QVariantList StreamService::screens() const
{
    QHash<QString, QVariantMap> online;
    for (const QVariant &v : m_online)
        online.insert(v.toMap().value(QStringLiteral("id")).toString(), v.toMap());
    QVariantList list;
    for (const QString &id : m_order) {
        QVariantMap s = m_known.value(id).toMap();
        const auto it = online.constFind(id);
        const bool here = it != online.constEnd(), on = m_on.contains(id);
        s.insert(QStringLiteral("id"), id);
        // подписи для «Экранов»: имя по-человечески, форма (16:9), как на связи
        const QString kind = s.value(QStringLiteral("kind")).toString(), name = s.value(QStringLiteral("name")).toString();
        const QString route = here ? it->value(QStringLiteral("route")).toString() : QString();
        s.insert(QStringLiteral("title"), id == QLatin1String("pi") ? QStringLiteral("Малинка")
                                          : id == QLatin1String("mac") ? QStringLiteral("Mac")
                                          : id == QLatin1String("tablet") ? QStringLiteral("Планшет")
                                          : id == QLatin1String("tv") ? QStringLiteral("Телевизор")
                                          : !name.isEmpty() ? name : id);
        s.insert(QStringLiteral("aspect"), aspectName(s.value(QStringLiteral("w")).toInt(), s.value(QStringLiteral("h")).toInt()));
        s.insert(QStringLiteral("via"), !here ? QString()
                                        : kind == QLatin1String("tablet") || kind == QLatin1String("tv")
                                              ? QStringLiteral("через малинку")
                                        : route == QLatin1String("cable") ? QStringLiteral("кабель")
                                        : route == QLatin1String("wifi") ? QStringLiteral("wi-fi") : QString());
        s.insert(QStringLiteral("online"), here);
        // яркость: можно ли ставить (говорит зритель, пока экран на связи), ведёт ли экран её сам, уровень
        const QVariantMap light = m_lights.value(id).toMap();
        s.insert(QStringLiteral("light"), here && it->value(QStringLiteral("light")).toBool());
        s.insert(QStringLiteral("lightAuto"), light.value(QStringLiteral("auto"), true).toBool());
        s.insert(QStringLiteral("lightLevel"), light.value(QStringLiteral("level"), 60).toInt());
        s.insert(QStringLiteral("route"), here ? it->value(QStringLiteral("route")) : QVariant(QString()));
        s.insert(QStringLiteral("on"), on);
        s.insert(QStringLiteral("main"), on && id == m_main);
        s.insert(QStringLiteral("mainNow"), here && id == m_mainNow);
        s.insert(QStringLiteral("position"), !on ? 0 : id == m_main ? 2 : 1);
        // не на связи, но зритель умеет включить его сам (телевизор) — не «нет связи», а «спит»: поставить его на
        // «стол» или «главный» — он включится (setScreenPosition)
        const bool wake = s.value(QStringLiteral("wake")).toBool();
        s.insert(QStringLiteral("wake"), wake);
        s.insert(QStringLiteral("state"), !here ? (wake ? QStringLiteral("standby") : QStringLiteral("offline"))
                                          : it->value(QStringLiteral("live")).toBool() && !m_deskAway && !m_leaving
                                                ? QStringLiteral("desk") : QStringLiteral("splash"));
        list << s;
    }
    return list;
}

void StreamService::setScreenPosition(const QString &id, int position)
{
    if (!m_order.contains(id))
        return;
    // Экран, что зритель включает сам (телевизор: "wake" в Screens), поставили на «стол» или «главный», а он не на
    // связи — попросить включить (ScreenWake); на связь он выйдет сам, и стол на нём появится по плану. Просим и когда
    // положение то же (план не меняется): телевизор выключили пультом, выбор «стол» на нём остался
    // Поставили на «нет», пока он включается, — «уже не надо». Стол при этом выключен (вышел сам без дела, выключили
    // плиткой) — включается: телевизор выбирают, чтобы увидеть на нём стол, а не заставку
    const bool waking = m_known.value(id).toMap().value(QStringLiteral("wake")).toBool()
                        && std::none_of(m_online.cbegin(), m_online.cend(), [&](const QVariant &v) {
                               return v.toMap().value(QStringLiteral("id")).toString() == id;
                           });
    if (waking) {
        {
            std::lock_guard lock(m_shared->m);
            m_shared->wakes.emplace_back(m_known.value(id).toMap().value(QStringLiteral("viewer")).toString().toStdString(),
                                         id.toStdString(), position > 0);
        }
        m_shared->wake();
    }
    QStringList on = m_on;
    QString main = m_main;
    auto next = [&] {   // главным — следующий выведенный: сначала из тех, что на связи
        const QStringList live = liveScreens(true, on);
        for (const QString &x : live)
            if (x != id)
                return x;
        for (const QString &x : on)
            if (x != id)
                return x;
        return id;
    };
    const bool none = m_on.isEmpty();
    if (position <= 0) {
        if (main == id)
            main = next();
        on.removeAll(id);
    } else {
        if (!on.contains(id))
            on << id;
        if (position >= 2)
            main = id;
        else if (main == id)
            main = next();
    }
    // Стол сняли с последнего экрана (на всех «нет») — это и есть «выключить стол» (просьба пользователя 21 ноября
    // 2026: «если на всех нет, то и рабочий стол выключаем»); какой экран был последним — помнится: выключатель вернёт
    // стол на него. И обратно: стол не был выведен никуда, экран поставили на «стол» — стол включается
    if (on.isEmpty()) {
        m_last = id;
        QSettings(QStringLiteral("naomi"), QStringLiteral("shell")).setValue(QStringLiteral("screens/last"), m_last);
        applyPlan(false, on, QString());
        return;
    }
    applyPlan(none || (waking && position > 0) ? true : m_enabled, on, main);
}

// Яркость экрана (планшета): сам по своему датчику света или уровень 1…100 %. Помнится (shell.conf, screens/light) и
// уходит зрителю, что несёт экран (ScreenLight), — сразу и при каждом его подключении
void StreamService::setScreenLight(const QString &id, bool automatic, int level)
{
    level = std::clamp(level, 1, 100);
    const QVariantMap was = m_lights.value(id).toMap();
    if (was.value(QStringLiteral("auto"), true).toBool() == automatic && was.value(QStringLiteral("level"), 60).toInt() == level
        && m_lights.contains(id))
        return;
    m_lights.insert(id, QVariantMap{{QStringLiteral("auto"), automatic}, {QStringLiteral("level"), level}});
    QSettings(QStringLiteral("naomi"), QStringLiteral("shell"))
        .setValue(QStringLiteral("screens/light"), QJsonDocument(QJsonObject::fromVariantMap(m_lights)).toJson(QJsonDocument::Compact));
    {
        std::lock_guard lock(m_shared->m);
        m_shared->lights[id.toStdString()] = {automatic, level};
        m_shared->lightsDirty = true;
    }
    m_shared->wake();
    emit screensChanged();
}

void StreamService::forgetScreen(const QString &id)
{
    for (const QVariant &v : m_online)
        if (v.toMap().value(QStringLiteral("id")).toString() == id)
            return;   // на связи — не забыть
    qCInfo(lcStream, "экран %s: забыт (убран из «Экранов»)", qPrintable(id));
    m_order.removeAll(id);
    m_known.remove(id);
    QSettings st(QStringLiteral("naomi"), QStringLiteral("shell"));
    st.setValue(QStringLiteral("screens/order"), m_order);
    st.setValue(QStringLiteral("screens/known"),
                QJsonDocument(QJsonObject::fromVariantMap(m_known)).toJson(QJsonDocument::Compact));
    QStringList on = m_on;
    on.removeAll(id);
    applyPlan(m_enabled && !on.isEmpty(), on, m_main);   // выведен был только на него — стол выключается
    emit screensChanged();
}

// Жест на экране (клавиша или щелчок у приставки и в окне Mac, двойное касание планшета), пока стол на нём не показан.
// Стол никто не показывает — включить: экран среди выбранных — как был выбран, нет — на него одного. Стол на одном
// экране — переезжает сюда (тот уходит на заставку). Стол на нескольких — этот добавляется к ним.
void StreamService::onWake(const QString &screen)
{
    if (screen.isEmpty()) {
        setEnabled(true);
        return;
    }
    const QStringList live = liveScreens(m_enabled, m_on);
    if (live.contains(screen))
        return;
    QStringList on = m_on;
    QString main = m_main;
    if (live.isEmpty()) {
        if (!on.contains(screen)) {
            on = {screen};
            main = screen;
        }
    } else if (live.size() == 1) {
        on = {screen};
        main = screen;
    } else {
        on << screen;
    }
    qCInfo(lcStream, "экран %s будит стол: %s", qPrintable(screen),
           live.isEmpty() ? "стол включается" : live.size() == 1 ? "стол переезжает на него" : "добавляется к показывающим");
    applyPlan(true, on, main);
}

// Обои стола — экранам, что рисуют заставку без дока сами: JPEG не шире 2560 и его хэш, своим потоком (файл большой)
void StreamService::setWallpaper(const QString &file)
{
    if (file == m_wallpaper)
        return;
    m_wallpaper = file;
    emit wallpaperChanged();
    QString path = file.section(QLatin1Char('?'), 0, 0);
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    std::thread([s = m_shared, path] {
        std::string hash = "none";
        std::vector<uint8_t> jpeg;
        if (QImage img(path); !path.isEmpty() && !img.isNull()) {
            if (img.width() > 2560)
                img = img.scaledToWidth(2560, Qt::SmoothTransformation);
            QByteArray out;
            QBuffer buffer(&out);
            buffer.open(QIODevice::WriteOnly);
            if (img.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", 88) && out.size() < 900 * 1024) {
                hash = QCryptographicHash::hash(out, QCryptographicHash::Sha1).toHex().left(16).toStdString();
                jpeg.assign(out.begin(), out.end());
            }
        }
        {
            std::lock_guard lock(s->m);
            s->wallHash = hash;
            s->wallJpeg = std::move(jpeg);
            s->wallDirty = true;
        }
        s->wake();
    }).detach();
}

void StreamService::setDecodersNear(bool near)
{
    if (m_shared->decodersNear.exchange(near) != near)
        m_shared->wake();
}

void StreamService::idleTest()
{
    leaveIdle(255);
}

void StreamService::leaveIdle(int dimLevel)
{
    if (!m_lockViewer || !m_connected || m_source != Desktop || !m_enabled)
        return;
    if (dimLevel <= 100)
        qCInfo(lcStream, "«луна»: зрителю IdleTest — приглушить до %d %%, стол уходит", dimLevel);
    else
        qCInfo(lcStream, "тест «без дела»: зрителю IdleTest, стол уходит");
    {
        std::lock_guard lock(m_shared->m);
        m_shared->idleTest = dimLevel;
    }
    m_shared->wake();
    setEnabled(false);
}

void StreamService::setAway(bool away, bool animated)
{
    if (animated != m_awayAnimated) {   // сначала — как: анимация включается раньше, чем стол двинется
        m_awayAnimated = animated;
        emit awayAnimatedChanged();
    }
    if (away != m_deskAway) {
        m_deskAway = away;
        emit deskAwayChanged();
        emit screensChanged();   // «стол» или «заставка» у экрана — и по этому
    }
}

// панель и окна ушли — последний кадр (одни обои), опорный; ответа зрителя нет 1,5 с — выключиться всё равно
void StreamService::awaySettled()
{
    if (!m_leaving || m_finalAsked)
        return;
    m_finalAsked = true;
    m_awayMark = 1 + stream::AwayFinal;
    requestFrame();
    m_leaveTimer.start(1500);
}

void StreamService::onAwayShown()
{
    if (m_leaving && m_finalAsked)
        finishLeave();
}

void StreamService::finishLeave()
{
    m_leaveTimer.stop();
    if (!m_leaving)
        return;
    m_leaving = false;
    qCInfo(lcStream, "стол ушёл с экранов");
    pushPlan();
    // ушёл не совсем, а на другие экраны — те покажут его сами (Reveal); не скажут — вернётся сам
    if (!liveScreens(m_enabled, m_on).isEmpty())
        m_revealTimer.start();
    emit screensChanged();
}

// Стол начинают смотреть (fresh): экран с заставкой без дока (Want lock, до выбора стола) — без панели и окон,
// сразу: его первый кадр — те же обои, что у него на экране, а стол он попросит показать сам (Reveal); экран без
// заставки — стол как есть. Пришёл к столу, который уже смотрят, — стол не трогается
void StreamService::onLockViewer(bool on, bool fresh)
{
    m_lockViewer = on;
    if (!fresh)
        return;
    if (m_leaving)   // уходили, а зритель уже другой — дальше, как просили
        finishLeave();
    setAway(on, false);
    if (on)
        m_revealTimer.start();
    else
        m_revealTimer.stop();
}

void StreamService::onReveal()
{
    if (!m_lockViewer || !m_deskAway || m_leaving)
        return;
    m_revealTimer.stop();
    qCInfo(lcStream, "экран показал обои — стол возвращается");
    setAway(false, true);
    m_awayMark = 1 + stream::AwayBack;
    requestFrame();
}

void StreamService::onRoute(int route)
{
    const QString r = route == stream::RouteCable ? QStringLiteral("cable")
                    : route == stream::RouteWifi ? QStringLiteral("wifi") : QString();
    if (r == m_route)
        return;
    m_route = r;
    holdWifi();
    emit changed();
}

void StreamService::onViewer(bool connected, const QString &name)
{
    m_connected = connected;
    if (!connected) {
        m_moving = false;
        m_wifiIdle.stop();
    }
    holdWifi();
    updateCapture();
    if (!connected) {
        if (m_leaving)   // стол уходил, а зритель пропал — выключиться сейчас
            finishLeave();
        holdGpu(false);
        m_viewer.clear();
        m_viewers.clear();
        m_wifiViewer = false;
        m_revealTimer.stop();
        onLayout(QString());
        m_fps = m_mbps = m_encodeMs = 0;
        m_input->releaseAll();
        onSource(Phone);
        // буферы стола — в его контексте, в этом же потоке; стол больше не рисует — освободить сейчас
        if (m_desktop && m_gl[Desktop]->ready && m_desktop->makeCurrent()) {
            {
                std::lock_guard lock(m_shared->m);
                if (m_shared->owner == Desktop) {
                    m_shared->owner = -1;
                    m_shared->buffersReady = false;
                    m_shared->inputs.clear();
                    m_shared->outputs.clear();
                }
            }
            releaseGl(*m_gl[Desktop]);
            m_desktop->doneCurrent();
        }
    } else if (!name.isEmpty()) {
        m_viewer = name;
    }
    emit changed();
}

void StreamService::onViewers(const QString &main, const QStringList &names, bool wifi)
{
    if (main == m_viewer && names == m_viewers && wifi == m_wifiViewer)
        return;
    m_viewer = main;
    m_viewers = names;
    m_wifiViewer = wifi;
    holdWifi();
    emit changed();
}

void StreamService::onStats(double fps, double mbps, double encodeMs)
{
    m_fps = fps;
    m_mbps = mbps;
    m_encodeMs = encodeMs;
    emit changed();
}

void StreamService::onSource(int source)
{
    m_source = source;
    if (m_desktop)
        m_desktop->setActive(source == Desktop);
    m_input->setDesktop(m_localDesktop ? m_localDesktop : source == Desktop ? m_desktop : nullptr);
    if ((source == Desktop && m_desktop) || (m_localDesktop && m_connected))
        m_cursorPoll.start();
    else
        m_cursorPoll.stop();
    requestFrame();
    emit changed();
}

// Такт зрителя: кадр стола — сейчас (рисуется здесь же, в tick(); его снимает capture — со значением такта)
void StreamService::onTick(uint64_t value)
{
    if (m_source != Desktop || !m_desktop)
        return;
    m_tickValue = value;
    m_desktop->tick(value);
    m_tickValue = 0;   // сцене нечего было рисовать — значение не к чему
    // Такты идут, пока у приставки движение (ввод за секунду, кадры за полсекунды): видеочип — не ниже 500 МГц,
    // и ещё 3 с. Сам он короткую анимацию (140–180 мс) начинает и кончает на 275 МГц (docs/PI.md, «Анимации»)
    holdGpu(true);
    m_gpuIdle.start();
    if (!m_moving) {
        m_moving = true;
        holdWifi();
    }
    m_wifiIdle.start();
}

// Wi-Fi без энергосбережения (naomid, docs/NAOMID.md) — пока он нужен: картинка идёт по нему (основной путь — не
// кабель: окно Mac, приставка без кабеля) или стол в движении (по Wi-Fi идут копии кадров запасного пути — он должен
// поспевать, чтобы кабель можно было выдернуть без шва). Со сбережением мышь и клавиши и подтверждения TCP ждут
// пробуждения радио. Стол стоит 10 с, а основной путь — кабель: радио спит — без этого телефон с неподвижным столом
// тратил +0,45 Вт (замер 19 ноября 2026, docs/AUDIT.md, этап 4). Оболочка упадёт — naomid отпустит сам
void StreamService::holdWifi()
{
    const bool awake = m_connected && (m_moving || m_wifiViewer || m_route != QLatin1String("cable"));
    if (awake == m_wifiAwake)
        return;
    m_wifiAwake = awake;
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.naomi.Phone"),
                                                    QStringLiteral("/org/naomi/Phone/Wifi"),
                                                    QStringLiteral("org.naomi.Phone.Wifi"), QStringLiteral("HoldAwake"));
    m << awake;
    QDBusConnection::systemBus().asyncCall(m);
}

void StreamService::holdGpu(bool on)
{
    if (on == m_gpuFast)
        return;
    m_gpuFast = on;
    if (!on)
        m_gpuIdle.stop();
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.naomi.Phone"),
                                                    QStringLiteral("/org/naomi/Phone/Gpu"),
                                                    QStringLiteral("org.naomi.Phone.Gpu"), QStringLiteral("HoldFast"));
    m << on;
    QDBusConnection::systemBus().asyncCall(m);
}

void StreamService::requestFrame()
{
    if (m_source == Desktop && m_desktop)
        m_desktop->requestFrame();
    else if (m_window)
        m_window->update();
}
