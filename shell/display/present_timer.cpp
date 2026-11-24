#include "present_timer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <vector>

#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

int64_t monotonicNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Когда fence сработал (нс, CLOCK_MONOTONIC); 0 — узнать нельзя.
int64_t fenceSignalTime(int fd)
{
    sync_file_info info{};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0 || info.num_fences == 0)
        return 0;
    std::vector<sync_fence_info> fences(info.num_fences);
    info.sync_fence_info = reinterpret_cast<uint64_t>(fences.data());
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0)
        return 0;
    uint64_t latest = 0;
    for (const auto &f : fences)
        latest = std::max<uint64_t>(latest, f.timestamp_ns);
    return int64_t(latest);
}

// Кадры с метками не раньше `from`: число и интервалы между соседними выводами.
void windowStats(const std::deque<int64_t> &stamps, int64_t from, NaomiFrameSnapshot &s)
{
    std::vector<double> iv;
    int64_t prev = 0;
    for (int64_t t : stamps) {
        if (t >= from) {
            ++s.fps;
            if (prev)
                iv.push_back((t - prev) / 1e6);
        }
        prev = t;
    }
    if (iv.empty())
        return;
    std::sort(iv.begin(), iv.end());
    s.medianMs = iv[iv.size() / 2];
    s.p95Ms = iv[std::min(iv.size() - 1, size_t(iv.size() * 0.95))];
    s.maxMs = iv.back();
}

} // namespace

PresentTimer::PresentTimer()
    : m_log(getenv("NAOMI_FRAMESTATS") != nullptr)
    , m_logDrops(m_log && std::string_view(getenv("NAOMI_FRAMESTATS")) == "drops")
{
    m_thread = std::thread([this] { run(); });
}

PresentTimer::~PresentTimer()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
    for (int fd : m_queue)
        close(fd);
}

void PresentTimer::add(int fenceFd)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Учёт кадров не должен накапливать fd, если HAL перестал сигналить.
        if (m_queue.size() >= 16) {
            close(m_queue.front());
            m_queue.pop_front();
        }
        m_queue.push_back(fenceFd);
    }
    m_cv.notify_one();
}

void PresentTimer::run()
{
    for (;;) {
        int fd;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop)
                return;
            fd = m_queue.front();
            m_queue.pop_front();
        }
        pollfd p{fd, POLLIN, 0};
        int ready;
        do
            ready = poll(&p, 1, 1000);
        while (ready < 0 && errno == EINTR);
        const bool signaled = ready > 0 && (p.revents & POLLIN) && !(p.revents & POLLERR);
        const int64_t stamp = signaled ? fenceSignalTime(fd) : 0;
        close(fd);
        if (!signaled) {
            if (m_log && ready == 0)
                fprintf(stderr, "naomi-frames: present fence не сработал за 1 с\n");
            continue;
        }
        const int64_t ts = stamp > 0 ? stamp : monotonicNs();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (ts <= m_last)
            continue;
        // Пропуск — пауза от 1,5 до 6 периодов: кадр не успел к vsync посреди анимации. Длиннее — это
        // покой: оболочка не рисует, когда ничего не меняется (так и задумано, ради батареи).
        const int64_t gap = ts - m_last;
        const bool idle = gap > m_periodNs * 6;
        if (m_logDrops && m_dropAt && idle)
            logDrop(m_last, true);
        const bool drop = m_last && gap > m_periodNs * 3 / 2 && !idle;
        if (drop)
            ++m_drops;
        m_last = ts;
        ++m_frames;
        m_stamps.push_back(ts);
        if (m_logDrops) {
            if (m_dropAt && ++m_afterDrop > 8)
                logDrop(ts, false);
            if (drop && !m_dropAt) {
                m_dropAt = ts;
                m_afterDrop = 0;
            }
        }
        while (!m_stamps.empty() && m_stamps.front() < ts - 2000000000)
            m_stamps.pop_front();
        if (m_log && ts - m_lastLog >= 1000000000) {
            m_lastLog = ts;
            logSecond(ts);
        }
    }
}

NaomiFrameSnapshot PresentTimer::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    NaomiFrameSnapshot s{};
    s.refreshHz = 1e9 / double(m_periodNs);
    s.frames = m_frames;
    s.drops = m_drops;
    windowStats(m_stamps, monotonicNs() - 1000000000, s);
    return s;
}

double PresentTimer::recentMedianMs(int intervals) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stamps.size() < size_t(intervals) + 1)
        return 0;
    std::vector<double> iv;
    for (size_t i = m_stamps.size() - intervals; i < m_stamps.size(); ++i)
        iv.push_back((m_stamps[i] - m_stamps[i - 1]) / 1e6);
    std::sort(iv.begin(), iv.end());
    return iv[iv.size() / 2];
}

void PresentTimer::logDrop(int64_t end, bool idleAfter)  // под m_mutex
{
    // интервалы: до 9 кадров перед пропуском, сам пропуск [в скобках], кадры после него до `end`
    const auto at = std::find(m_stamps.begin(), m_stamps.end(), m_dropAt);
    m_dropAt = 0;
    if (at == m_stamps.end())
        return;
    const size_t drop = size_t(at - m_stamps.begin());
    std::string line;
    char buf[32];
    for (size_t i = drop > 9 ? drop - 9 : 1; i < m_stamps.size() && m_stamps[i] <= end; ++i) {
        const double ms = (m_stamps[i] - m_stamps[i - 1]) / 1e6;
        const bool idleBefore = m_stamps[i] - m_stamps[i - 1] > m_periodNs * 6;
        snprintf(buf, sizeof buf, i == drop ? " [%.1f]" : idleBefore ? " покой→" : " %.1f", ms);
        line += buf;
    }
    fprintf(stderr, "naomi-drop:%s%s\n", line.c_str(), idleAfter ? " →покой" : "");
}

void PresentTimer::logSecond(int64_t now)  // под m_mutex
{
    NaomiFrameSnapshot s{};
    windowStats(m_stamps, now - 1000000000, s);
    // Покой (часы на столе — 1 к/с, мигающий курсор терминала — 2 к/с) без новых пропусков — строку не пишем
    if (s.fps <= 2 && m_drops == m_loggedDrops)
        return;
    m_loggedDrops = m_drops;
    fprintf(stderr, "naomi-frames: %.0f к/с, медиана %.2f мс, p95 %.2f мс, максимум %.2f мс, "
                    "всего %llu, пропусков %llu\n",
            s.fps, s.medianMs, s.p95Ms, s.maxMs, (unsigned long long)m_frames,
            (unsigned long long)m_drops);
}
