#include "code_pin.h"

#include <QByteArray>
#include <QtGlobal>

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <sys/syscall.h>
#include <thread>
#include <tuple>
#include <unistd.h>

#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif

namespace {

// Один проход по /proc/self/maps: закрепить свои (private) отображения файлов, которых ещё не закрепляли. Повторно
// те же не трогаются: mlock2 обходит все страницы отображения, держа карту памяти процесса, — а её в это время ждёт
// поток интерфейса; проходить заново по сотням мегабайт уже закреплённого незачем.
// Ответ — сколько МБ отображений закреплено этим проходом; -1 — ядро отказало (нет LimitMEMLOCK)
long pinOnce(std::set<std::tuple<uint64_t, uint64_t, uint64_t>> &done)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return -1;
    char line[1024];
    long bytes = 0;
    bool permDenied = false;
    bool noMem = false;
    while (fgets(line, sizeof line, maps)) {
        uint64_t from = 0, to = 0, inode = 0;
        char perms[8] = "";
        int path = 0;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %*s %*s %" SCNu64 " %n", &from, &to, perms, &inode, &path) < 4)
            continue;
        // файл на диске: есть inode и путь от корня; общие отображения (буферы окон, память драйверов) — не наши
        if (!inode || perms[3] != 'p' || line[path] != '/' || !strncmp(line + path, "/dev/", 5))
            continue;
        if (!done.insert({from, to, inode}).second)
            continue;
        if (syscall(SYS_mlock2, from, to - from, MLOCK_ONFAULT) == 0)
            bytes += long(to - from);
        else if (errno == EPERM)
            permDenied = true;
        else if (errno == ENOMEM)
            noMem = true;
    }
    fclose(maps);
    if (permDenied && !bytes)
        return -1;
    if (noMem && !bytes && done.size() <= 1)
        return -1;
    return bytes >> 20;
}

} // namespace

void pinCodeStart()
{
    if (qgetenv("NAOMI_PIN_CODE") == "0")
        return;
    std::thread([] {
        std::set<std::tuple<uint64_t, uint64_t, uint64_t>> done;
        for (int pass = 0;; ++pass) {
            const long mb = pinOnce(done);
            if (mb < 0) {
                qWarning("код оболочки не закреплён: ядро отказало (службе нужен LimitMEMLOCK=infinity)");
                return;
            }
            if (pass == 0)
                qInfo("код оболочки закреплён в памяти: отображений файлов %ld МБ (по мере обращения)", mb);
            std::this_thread::sleep_for(std::chrono::seconds(pass == 0 ? 5 : pass == 1 ? 30 : 300));
        }
    }).detach();
}
