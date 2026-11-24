// НаомиФон — учёт выведенных кадров по present-fence.
// Каждый present даёт fence, который срабатывает, когда кадр реально ушёл на панель; отдельный поток
// ждёт его и читает метку срабатывания (SYNC_IOC_FILE_INFO, CLOCK_MONOTONIC).
// NAOMI_FRAMESTATS — раз в секунду сводка в журнал; NAOMI_FRAMESTATS=drops — ещё и каждый пропуск с
// соседними интервалами: видно, был он посреди анимации (рывок) или на её краю.
#pragma once

#include "naomi_display.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class PresentTimer
{
public:
    PresentTimer();
    ~PresentTimer();

    void add(int fenceFd);              // забирает fd себе
    void setPeriodNs(int64_t ns) { m_periodNs = ns; }
    NaomiFrameSnapshot snapshot() const;
    double recentMedianMs(int intervals) const;  // медиана последних интервалов вывода; 0 — мало кадров

private:
    void run();
    void logSecond(int64_t now);
    void logDrop(int64_t end, bool idleAfter);

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<int> m_queue;
    bool m_stop = false;

    std::deque<int64_t> m_stamps;       // метки за последние 2 с
    uint64_t m_frames = 0;
    uint64_t m_drops = 0;
    int64_t m_last = 0;
    std::atomic<int64_t> m_periodNs{16666666};
    bool m_log = false;
    bool m_logDrops = false;
    int64_t m_lastLog = 0;
    uint64_t m_loggedDrops = 0;   // пропусков на момент прошлой строки сводки (logSecond)
    int64_t m_dropAt = 0;               // метка кадра после пропуска, ждём кадры за ним для журнала
    int m_afterDrop = 0;
};
