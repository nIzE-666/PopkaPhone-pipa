// naomi-term — служба вкладок «Терминала» рабочего стола НаомиФона (docs/TERMINAL.md).
//
// Держит псевдотерминалы вкладок и их оболочки, помнит текст каждой вкладки (последние 256 КБ вывода как есть, с
// управляющими последовательностями) и раз в 5 с после изменений кладёт его на диск (~/.local/share/naomi/term/N.log).
// Разбор и отрисовку текста делает стол (shell/app/term_*): связь — сокет $XDG_RUNTIME_DIR/naomi-term.sock, язык —
// term_protocol.h. Оболочка запускает службу отдельной программой пользователя (systemd-run --user), а не своей
// дочерней: перезапуск оболочки вкладки не трогает — открыли терминал снова, всё на месте, программы работают.
// Закрыли терминал (Q) или перезагрузили телефон — текст остался на диске: открыли снова — вкладки с прежним текстом,
// оболочки — новые, в тех же папках.
//
// Лёгкая: один поток, poll(); пока ничего не выводится и не нажимается — спит. Ни вкладок, ни стола 10 с — уходит.
#include "term_protocol.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace termproto;

namespace {

constexpr size_t kRing = 256 * 1024;      // текст вкладки, что помним (≈ 3000 строк по 80 знаков)
constexpr size_t kRingSlack = 64 * 1024;  // обрезать с запасом, а не на каждом чтении
constexpr int64_t kSaveAfterMs = 5000;    // текст изменился — на диск не раньше чем через 5 с (флеш не трепать)
constexpr int64_t kPlaceAfterMs = 250;    // вывод или нажатие — через столько проверить папку и программу
constexpr size_t kClientHigh = 1u << 20;  // столу не успевают отдать столько — вывод вкладок ждёт
constexpr size_t kChunk = 64 * 1024;      // вывод — кадрами не больше
constexpr int64_t kIdleExitMs = 10000;    // ни вкладок, ни стола столько — служба уходит

// Перед текстом, сохранённым на диске, оболочка вкладки — новая: всё, что прошлая программа включила (запасной экран,
// мышь, скрытый курсор, вставка скобками), выключить, и черта
const char kRestored[] = "\x1b[0m\x1b[?1049l\x1b[?25h\x1b[?1l\x1b[?2004l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"
                         "\x1b(B\r\n\x1b[90m── оболочка запущена заново ──\x1b[0m\r\n";

int64_t nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

struct Session {
    uint32_t id = 0;
    pid_t pid = -1;
    int fd = -1;
    std::string ring;       // вывод как есть, последние kRing байт
    std::string pendingIn;  // нажатия, которые псевдотерминал ещё не принял
    bool dirty = false;     // текст не сохранён
    int64_t saveAt = 0;     // когда сохранить (0 — нечего)
    int64_t placeAt = 0;    // когда проверить папку и программу (0 — незачем)
    std::string cwd, proc;
    bool reaped = false;
    int status = 0;
};

std::map<uint32_t, Session> g_sessions;
int g_listen = -1;
int g_client = -1;
std::string g_in;
std::string g_out;
size_t g_outOff = 0;
std::string g_home, g_shell, g_user, g_dir, g_sockPath;
int64_t g_idleSince = 0;

size_t outPending() { return g_out.size() - g_outOff; }

std::string logPath(uint32_t id) { return g_dir + "/" + std::to_string(id) + ".log"; }

void mkdirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0700);
}

std::string expand(const std::string &dir)
{
    if (dir.empty() || dir == "~")
        return g_home;
    if (dir.rfind("~/", 0) == 0)
        return g_home + dir.substr(1);
    return dir;
}

std::string homeify(const std::string &dir)
{
    if (dir == g_home)
        return "~";
    if (dir.size() > g_home.size() && dir.compare(0, g_home.size(), g_home) == 0 && dir[g_home.size()] == '/')
        return "~" + dir.substr(g_home.size());
    return dir;
}

void trimRing(std::string &r, size_t limit)
{
    if (r.size() <= limit)
        return;
    size_t cut = r.size() - limit;
    const size_t nl = r.find('\n', cut);   // с начала строки: середина строки — обрывок последовательности
    if (nl != std::string::npos && nl - cut < 4096)
        cut = nl + 1;
    r.erase(0, cut);
}

bool readFile(const std::string &path, std::string &out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        out.append(buf, size_t(n));
    close(fd);
    trimRing(out, kRing);
    return true;
}

void save(Session &s)
{
    s.saveAt = 0;
    if (!s.dirty)
        return;
    s.dirty = false;
    mkdirs(g_dir);
    const std::string path = logPath(s.id), tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        logf("вкладка %u: не записать %s: %s", s.id, tmp.c_str(), strerror(errno));
        return;
    }
    size_t off = 0;
    bool ok = true;
    while (off < s.ring.size()) {
        const ssize_t w = write(fd, s.ring.data() + off, s.ring.size() - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            ok = false;
            break;
        }
        off += size_t(w);
    }
    close(fd);
    if (ok)
        rename(tmp.c_str(), path.c_str());
    else
        unlink(tmp.c_str());
}

// ---------- стол ----------
void dropClient()
{
    if (g_client >= 0)
        close(g_client);
    g_client = -1;
    g_in.clear();
    g_out.clear();
    g_outOff = 0;
}

void flushClient()
{
    while (g_client >= 0 && outPending()) {
        const ssize_t w = send(g_client, g_out.data() + g_outOff, outPending(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) {
            g_outOff += size_t(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            logf("стол отключился");
            dropClient();
            return;
        }
    }
    if (g_outOff == g_out.size()) {
        g_out.clear();
        g_outOff = 0;
    } else if (g_outOff > (1u << 20)) {
        g_out.erase(0, g_outOff);
        g_outOff = 0;
    }
}

void sendFrame(char type, uint32_t id, const char *data, size_t n)
{
    if (g_client < 0)
        return;
    char h[kHeader];
    h[0] = type;
    putU32(h + 1, id);
    putU32(h + 5, uint32_t(n));
    g_out.append(h, kHeader);
    g_out.append(data, n);
}

void sendFrame(char type, uint32_t id, const std::string &s) { sendFrame(type, id, s.data(), s.size()); }

void sendData(uint32_t id, const char *data, size_t n, char type = Data)
{
    for (size_t off = 0; off < n; off += kChunk)
        sendFrame(type, id, data + off, std::min(kChunk, n - off));
}

void sendPlace(const Session &s) { sendFrame(Place, s.id, s.cwd + "\n" + s.proc); }

// ---------- вкладки ----------
bool spawn(Session &s, int cols, int rows)
{
    winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        logf("вкладка %u: forkpty: %s", s.id, strerror(errno));
        return false;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig : {SIGCHLD, SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGQUIT})
            signal(sig, SIG_DFL);
        std::string dir = expand(s.cwd);
        if (chdir(dir.c_str()) != 0) {
            dir = g_home;
            if (chdir(dir.c_str()) != 0)
                dir = "/";
        }
        // окружение программ оболочки телефона (служба могла запуститься и от неё) — не им
        for (const char *v : {"QT_QPA_PLATFORM", "QT_QPA_EGLFS_INTEGRATION", "QT_QPA_EGLFS_HIDECURSOR", "QT_PLUGIN_PATH",
                              "EGL_PLATFORM", "NAOMI_FRAMESTATS", "QT_SCALE_FACTOR", "INVOCATION_ID",
                              "JOURNAL_STREAM", "SYSTEMD_EXEC_PID", "MANAGERPID"})
            unsetenv(v);
        // Пробрасываем сокет Wayland рабочего стола, если он поднят: из терминала можно запускать GUI-приложения
        // Linux (mpv, firefox, code, gimp) и wl-copy/wl-paste прямо окнами на столе PopkaPhone.
        const char *rtDir = getenv("XDG_RUNTIME_DIR");
        const std::string wlSock = std::string(rtDir && *rtDir ? rtDir : "/run/user/32011") + "/wayland-naomi";
        if (access(wlSock.c_str(), F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-naomi", 1);
            setenv("QT_QPA_PLATFORM", "wayland", 1);
            setenv("EGL_PLATFORM", "wayland", 1);
            setenv("GDK_BACKEND", "wayland,x11", 1);
            setenv("SDL_VIDEODRIVER", "wayland,x11", 1);
            setenv("MOZ_ENABLE_WAYLAND", "1", 1);
            setenv("ELECTRON_OZONE_PLATFORM_HINT", "wayland", 1);
        } else {
            unsetenv("WAYLAND_DISPLAY");
        }
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "naomi-term", 1);
        const char *lang = getenv("LANG");
        if (!lang || !*lang)
            setenv("LANG", "C.UTF-8", 1);
        if (!getenv("PATH"))
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("HOME", g_home.c_str(), 1);
        setenv("SHELL", g_shell.c_str(), 1);
        setenv("USER", g_user.c_str(), 1);
        setenv("LOGNAME", g_user.c_str(), 1);
        setenv("PWD", dir.c_str(), 1);
        const char *slash = strrchr(g_shell.c_str(), '/');
        execl(g_shell.c_str(), slash ? slash + 1 : g_shell.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    s.pid = pid;
    s.fd = master;
    return true;
}

void hangup(Session &s)
{
    if (s.pid > 0 && !s.reaped) {
        kill(-s.pid, SIGHUP);
        kill(s.pid, SIGHUP);
        kill(-s.pid, SIGCONT);
    }
    if (s.fd >= 0)
        close(s.fd);
    s.fd = -1;
}

void checkPlace(Session &s)
{
    s.placeAt = 0;
    if (s.fd < 0 || s.pid <= 0)
        return;
    std::string cwd = s.cwd;
    char buf[4096];
    const ssize_t n = readlink(("/proc/" + std::to_string(s.pid) + "/cwd").c_str(), buf, sizeof buf - 1);
    if (n > 0)
        cwd = homeify(std::string(buf, size_t(n)));
    std::string proc;
    const pid_t fg = tcgetpgrp(s.fd);
    if (fg > 0 && fg != s.pid) {
        const int fd = open(("/proc/" + std::to_string(fg) + "/comm").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            const ssize_t m = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (m > 0)
                proc.assign(buf, size_t(m));
            while (!proc.empty() && (proc.back() == '\n' || proc.back() == ' '))
                proc.pop_back();
        }
        if (proc.empty())
            proc = "…";   // вожак группы уже ушёл (конвейер) — что-то работает, а что — не сказать
    }
    if (cwd != s.cwd || proc != s.proc) {
        s.cwd = cwd;
        s.proc = proc;
        sendPlace(s);
    }
}

void touched(Session &s, int64_t now)
{
    if (!s.placeAt)
        s.placeAt = now + kPlaceAfterMs;
}

// оболочка вкладки ушла (exit, Ctrl+D): дочитать, сказать столу, текст с диска прочь — вкладка закрывается
void finish(Session &s)
{
    if (s.fd >= 0) {
        char buf[65536];
        ssize_t n;
        while ((n = read(s.fd, buf, sizeof buf)) > 0)
            sendData(s.id, buf, size_t(n));
        close(s.fd);
        s.fd = -1;
    }
    const int code = WIFEXITED(s.status) ? WEXITSTATUS(s.status) : 128 + (WIFSIGNALED(s.status) ? WTERMSIG(s.status) : 0);
    sendFrame(Exit, s.id, std::to_string(code));
    unlink(logPath(s.id).c_str());
}

void readSession(Session &s, int64_t now, std::vector<uint32_t> &gone)
{
    char buf[65536];
    for (int i = 0; i < 4; ++i) {   // не больше 256 КБ за раз — другим вкладкам и столу тоже очередь
        const ssize_t n = read(s.fd, buf, sizeof buf);
        if (n > 0) {
            s.ring.append(buf, size_t(n));
            if (s.ring.size() > kRing + kRingSlack)
                trimRing(s.ring, kRing);
            s.dirty = true;
            if (!s.saveAt)
                s.saveAt = now + kSaveAfterMs;
            touched(s, now);
            sendData(s.id, buf, size_t(n));
            if (outPending() > kClientHigh)
                return;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        // EIO: закрылась последняя сторона оболочки — она ушла
        close(s.fd);
        s.fd = -1;
        if (!s.reaped) {
            int st = 0;
            if (waitpid(s.pid, &st, WNOHANG) == s.pid) {
                s.reaped = true;
                s.status = st;
            }
        }
        if (s.reaped)
            gone.push_back(s.id);
        return;
    }
}

void writeInput(Session &s)
{
    while (s.fd >= 0 && !s.pendingIn.empty()) {
        const ssize_t w = write(s.fd, s.pendingIn.data(), s.pendingIn.size());
        if (w > 0)
            s.pendingIn.erase(0, size_t(w));
        else if (w < 0 && errno == EINTR)
            continue;
        else
            break;   // EAGAIN — допишем, когда примет
    }
}

void removeLogsExcept(const std::set<uint32_t> &keep)
{
    DIR *d = opendir(g_dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        char *end = nullptr;
        const unsigned long id = strtoul(e->d_name, &end, 10);
        if (end && end != e->d_name && strcmp(end, ".log") == 0 && !keep.count(uint32_t(id)))
            unlink((g_dir + "/" + e->d_name).c_str());
    }
    closedir(d);
}

void quitAll()
{
    for (auto &[id, s] : g_sessions) {
        if (!s.reaped && s.fd >= 0) {
            s.dirty = true;   // сохранить и то, что уже на диске: файл мог остаться от прошлого раза другим
            save(s);
        }
        hangup(s);
    }
    g_sessions.clear();
}

// ---------- кадры от стола ----------
void handle(char type, uint32_t id, std::string &p, int64_t now, std::vector<uint32_t> &gone)
{
    switch (type) {
    case Hello: {
        std::set<uint32_t> keep;
        size_t pos = p.find(' ');
        while (pos != std::string::npos) {
            const size_t next = p.find(' ', pos + 1);
            const std::string word = p.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
            if (!word.empty())
                keep.insert(uint32_t(strtoul(word.c_str(), nullptr, 10)));
            pos = next;
        }
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            if (keep.count(it->first)) {
                ++it;
                continue;
            }
            logf("вкладка %u: у стола её нет — закрыта", it->first);
            hangup(it->second);
            it = g_sessions.erase(it);
        }
        removeLogsExcept(keep);
        sendFrame(Hello, 0, std::to_string(kVersion));
        logf("стол на связи: вкладок у него %zu, живых %zu", keep.size(), g_sessions.size());
        break;
    }
    case Open: {
        int cols = 80, rows = 24, restore = 0, used = 0;
        if (sscanf(p.c_str(), "%d %d %d %n", &cols, &rows, &restore, &used) < 3)
            break;
        cols = std::max(2, std::min(cols, 1000));
        rows = std::max(1, std::min(rows, 500));
        auto it = g_sessions.find(id);
        if (it != g_sessions.end()) {   // живая — её текст и размер
            Session &s = it->second;
            winsize ws {};
            ws.ws_col = static_cast<unsigned short>(cols);
            ws.ws_row = static_cast<unsigned short>(rows);
            if (s.fd >= 0)
                ioctl(s.fd, TIOCSWINSZ, &ws);
            sendData(id, s.ring.data(), s.ring.size(), Replay);
            sendPlace(s);
            touched(s, now);
            break;
        }
        Session s;
        s.id = id;
        s.cwd = p.substr(size_t(used));
        if (s.cwd.empty())
            s.cwd = "~";
        if (restore && readFile(logPath(id), s.ring))
            s.ring += kRestored;
        if (!spawn(s, cols, rows)) {
            sendFrame(Exit, id, "127");
            break;
        }
        s.dirty = !s.ring.empty();
        s.saveAt = s.dirty ? now + kSaveAfterMs : 0;
        sendData(id, s.ring.data(), s.ring.size(), Replay);
        sendPlace(s);
        touched(s, now);
        g_sessions.emplace(id, std::move(s));
        break;
    }
    case Input: {
        auto it = g_sessions.find(id);
        if (it == g_sessions.end())
            break;
        it->second.pendingIn += p;
        writeInput(it->second);
        touched(it->second, now);
        break;
    }
    case Resize: {
        auto it = g_sessions.find(id);
        int cols = 0, rows = 0;
        if (it == g_sessions.end() || it->second.fd < 0 || sscanf(p.c_str(), "%d %d", &cols, &rows) != 2)
            break;
        winsize ws {};
        ws.ws_col = static_cast<unsigned short>(std::max(2, std::min(cols, 1000)));
        ws.ws_row = static_cast<unsigned short>(std::max(1, std::min(rows, 500)));
        ioctl(it->second.fd, TIOCSWINSZ, &ws);
        break;
    }
    case Close: {
        auto it = g_sessions.find(id);
        if (it != g_sessions.end()) {
            hangup(it->second);
            g_sessions.erase(it);
        }
        unlink(logPath(id).c_str());
        break;
    }
    case Quit:
        logf("терминал закрыт: текст вкладок сохранён, оболочки закрыты");
        quitAll();
        flushClient();
        unlink(g_sockPath.c_str());
        exit(0);
    default:
        break;
    }
    (void)gone;
}

void readClient(int64_t now, std::vector<uint32_t> &gone)
{
    char buf[65536];
    bool closed = false;   // стол закрыл связь — но сначала разобрать, что он успел прислать (Q и закрыл)
    for (;;) {
        const ssize_t n = recv(g_client, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            g_in.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        closed = true;
        break;
    }
    size_t off = 0;
    while (g_client >= 0 && g_in.size() - off >= kHeader) {
        const char *h = g_in.data() + off;
        const uint32_t len = getU32(h + 5);
        if (len > kMaxPayload) {
            logf("стол прислал кадр в %u байт — связь закрыта", len);
            dropClient();
            return;
        }
        if (g_in.size() - off < kHeader + len)
            break;
        std::string payload(h + kHeader, len);
        const char type = h[0];
        const uint32_t id = getU32(h + 1);
        off += kHeader + len;
        handle(type, id, payload, now, gone);
    }
    if (g_client >= 0)
        g_in.erase(0, off);
    if (closed && g_client >= 0) {
        logf("стол отключился");
        dropClient();
    }
}

int listenSocket(const std::string &path)
{
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return -1;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    // уже работает другая служба — она и главная
    const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0 && connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
        close(probe);
        logf("служба уже работает");
        exit(0);
    }
    if (probe >= 0)
        close(probe);
    unlink(path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    const mode_t old = umask(0077);
    const bool ok = fd >= 0 && bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 && listen(fd, 4) == 0;
    umask(old);
    if (!ok) {
        logf("сокет %s: %s", path.c_str(), strerror(errno));
        return -1;
    }
    return fd;
}

}   // namespace

int main()
{
    const passwd *pw = getpwuid(getuid());
    g_home = pw && pw->pw_dir ? pw->pw_dir : (getenv("HOME") ? getenv("HOME") : "/");
    g_shell = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/bash";
    g_user = pw && pw->pw_name ? pw->pw_name : "user";
    const char *dir = getenv("NAOMI_TERM_DIR");   // своя папка — для проверок, мимо настоящих вкладок
    g_dir = dir && *dir ? dir : g_home + "/.local/share/naomi/term";
    const char *rt = getenv("XDG_RUNTIME_DIR");
    g_sockPath = std::string(rt && *rt ? rt : "/tmp") + "/naomi-term.sock";
    const std::string &sockPath = g_sockPath;

    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP})
        sigaddset(&mask, sig);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    g_listen = listenSocket(sockPath);
    if (g_listen < 0 || sfd < 0)
        return 1;
    logf("служба терминала: %s, текст вкладок — %s", sockPath.c_str(), g_dir.c_str());

    std::vector<pollfd> fds;
    std::vector<uint32_t> ids;   // чья строка в fds (с третьей)
    for (;;) {
        int64_t now = nowMs();
        // когда проснуться: сохранить текст, проверить папку и программу, уйти без дела
        int64_t wake = -1;
        for (auto &[id, s] : g_sessions) {
            for (int64_t at : {s.saveAt, s.placeAt})
                if (at && (wake < 0 || at < wake))
                    wake = at;
        }
        if (g_sessions.empty() && g_client < 0) {
            if (!g_idleSince)
                g_idleSince = now;
            if (now - g_idleSince >= kIdleExitMs) {
                logf("ни вкладок, ни стола — служба уходит");
                unlink(sockPath.c_str());
                return 0;
            }
            const int64_t at = g_idleSince + kIdleExitMs;
            if (wake < 0 || at < wake)
                wake = at;
        } else {
            g_idleSince = 0;
        }

        fds.clear();
        ids.clear();
        fds.push_back({sfd, POLLIN, 0});
        fds.push_back({g_listen, POLLIN, 0});
        if (g_client >= 0)
            fds.push_back({g_client, short(POLLIN | (outPending() ? POLLOUT : 0)), 0});
        const size_t first = fds.size();
        const bool reading = outPending() <= kClientHigh;   // стол не успевает — вывод вкладок ждёт в их буферах
        for (auto &[id, s] : g_sessions) {
            if (s.fd < 0)
                continue;
            const short ev = short((reading ? POLLIN : 0) | (s.pendingIn.empty() ? 0 : POLLOUT));
            if (!ev)
                continue;
            fds.push_back({s.fd, ev, 0});
            ids.push_back(id);
        }
        const int timeout = wake < 0 ? -1 : int(std::max<int64_t>(0, wake - now));
        if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            logf("poll: %s", strerror(errno));
            return 1;
        }
        now = nowMs();
        std::vector<uint32_t> gone;

        if (fds[0].revents & POLLIN) {
            signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    int st = 0;
                    pid_t pid;
                    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
                        for (auto &[id, s] : g_sessions)
                            if (s.pid == pid) {
                                s.reaped = true;
                                s.status = st;
                                gone.push_back(id);
                            }
                    }
                } else {   // остановка службы, выключение телефона — текст на диск
                    logf("сигнал %u — текст вкладок на диск, ухожу", si.ssi_signo);
                    for (auto &[id, s] : g_sessions)
                        save(s);
                    unlink(sockPath.c_str());
                    return 0;
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            const int c = accept4(g_listen, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c >= 0) {
                if (g_client >= 0)
                    logf("новый стол — прежний отключён");
                dropClient();
                g_client = c;
            }
        }
        if (g_client >= 0 && fds.size() > 2 && fds[2].fd == g_client) {
            if (fds[2].revents & POLLOUT)
                flushClient();
            if (g_client >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR)))
                readClient(now, gone);
        }
        for (size_t i = first; i < fds.size(); ++i) {
            auto it = g_sessions.find(ids[i - first]);
            if (it == g_sessions.end() || it->second.fd != fds[i].fd)
                continue;   // вкладку закрыли кадром стола выше
            if (fds[i].revents & POLLOUT)
                writeInput(it->second);
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                readSession(it->second, now, gone);
        }
        for (uint32_t id : gone) {
            auto it = g_sessions.find(id);
            if (it == g_sessions.end() || !it->second.reaped)
                continue;
            finish(it->second);
            g_sessions.erase(it);
        }
        for (auto &[id, s] : g_sessions) {
            if (s.saveAt && s.saveAt <= now)
                save(s);
            if (s.placeAt && s.placeAt <= now)
                checkPlace(s);
        }
        flushClient();
    }
}
