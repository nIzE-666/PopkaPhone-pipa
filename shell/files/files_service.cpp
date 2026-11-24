#include "files_service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QMutex>
#include <QSettings>
#include <QThreadPool>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

FilesService *FilesService::s_instance = nullptr;

namespace {

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

QString starredFile() { return configDir() + "/naomi/files/starred"; }
QString bookmarksFile() { return configDir() + "/gtk-3.0/bookmarks"; }
QString settingsFile() { return configDir() + "/naomi/files.conf"; }
QString deskDir() { return configDir() + "/naomi/desktop"; }
QString wallDir() { return deskDir() + "/wallpapers"; }        // собрание обоев («Настройки» → «Обои»)
QString wallConf() { return deskDir() + "/wallpaper.conf"; }   // current — имя в собрании; «-» — без картинки

// Копия одним шагом (QSaveFile: пишем рядом, потом переименование): старое не пропадает, если прочитать не вышло.
// Было «удалить, потом QFile::copy» — копия не легла, и стол остался без обоев.
bool copyWhole(const QString &from, const QString &to)
{
    QFile in(from);
    QSaveFile out(to);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
        qWarning("обои: не открылось: %s / %s", qPrintable(in.errorString()), qPrintable(out.errorString()));
        return false;
    }
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty() || out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            return false;
        }
    }
    if (!out.commit()) {
        qWarning("обои: не записались: %s", qPrintable(out.errorString()));
        return false;
    }
    return true;
}

} // namespace

FilesService::FilesService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    loadStarred();
    connect(&m_ops, &FilesOps::changed, this, [this] {
        emit opsChanged();
        refreshTrash();
    });
    QDir().mkpath(FilesOps::trashDir() + "/files");
    QDir().mkpath(FilesOps::trashDir() + "/info");
    m_trashWatch.addPath(FilesOps::trashDir() + "/files");
    connect(&m_trashWatch, &QFileSystemWatcher::directoryChanged, this, &FilesService::refreshTrash);
    refreshTrash();
    // собрания обоев ещё нет, а обои стоят (до «Настроек» их клали tools/naomi и «Сделать фоном») — они и начнут его
    const QString current = deskDir() + "/wallpaper.jpg";
    if (QDir(wallDir()).isEmpty() && QFile::exists(current) && QDir().mkpath(wallDir())
        && copyWhole(current, wallDir() + "/Свои.jpg"))
        QSettings(wallConf(), QSettings::IniFormat).setValue("current", "Свои.jpg");
}

QString FilesService::home() const
{
    return QDir::homePath();
}

void FilesService::refreshTrash()
{
    const int n = int(QDir(FilesOps::trashDir() + "/files")
                          .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).size());
    if (n != m_trashCount) {
        m_trashCount = n;
        emit trashChanged();
    }
}

// ---------- избранное ----------

void FilesService::loadStarred()
{
    m_starred.clear();
    QFile f(starredFile());
    if (f.open(QIODevice::ReadOnly))
        for (const QByteArray &line : f.readAll().split('\n'))
            if (!line.trimmed().isEmpty())
                m_starred.append(QUrl(QString::fromUtf8(line.trimmed())).toLocalFile());
}

void FilesService::saveStarred()
{
    QDir().mkpath(QFileInfo(starredFile()).absolutePath());
    QSaveFile f(starredFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    for (const QString &p : m_starred)
        f.write(QUrl::fromLocalFile(p).toEncoded() + "\n");
    f.commit();
}

void FilesService::setStarred(const QStringList &paths, bool on)
{
    bool changed = false;
    for (const QString &p : paths) {
        if (on && !m_starred.contains(p)) {
            m_starred.append(p);
            changed = true;
        } else if (!on && m_starred.removeAll(p) > 0) {
            changed = true;
        }
    }
    if (changed) {
        saveStarred();
        emit starredChanged();
    }
}

// ---------- закреплённые: закладки GTK («file:///путь имя») ----------

QStringList FilesService::loadBookmarkLines() const
{
    QStringList lines;
    QFile f(bookmarksFile());
    if (f.open(QIODevice::ReadOnly))
        for (const QByteArray &line : f.readAll().split('\n'))
            if (line.startsWith("file://"))
                lines.append(QString::fromUtf8(line));
    return lines;
}

void FilesService::saveBookmarkLines(const QStringList &lines)
{
    QDir().mkpath(QFileInfo(bookmarksFile()).absolutePath());
    QSaveFile f(bookmarksFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    for (const QString &l : lines)
        f.write(l.toUtf8() + "\n");
    f.commit();
    emit bookmarksChanged();
}

QVariantList FilesService::bookmarks() const
{
    QVariantList out;
    for (const QString &line : loadBookmarkLines()) {
        const int space = line.indexOf(' ');
        const QString path = QUrl(space > 0 ? line.left(space) : line).toLocalFile();
        const QString label = space > 0 ? line.mid(space + 1) : QFileInfo(path).fileName();
        out.append(QVariantMap{{"path", path}, {"label", label}});
    }
    return out;
}

bool FilesService::isPinned(const QString &path) const
{
    for (const QVariant &b : bookmarks())
        if (b.toMap().value("path").toString() == path)
            return true;
    return false;
}

void FilesService::pin(const QString &path)
{
    if (isPinned(path) || !QFileInfo(path).isDir())
        return;
    QStringList lines = loadBookmarkLines();
    lines.append(QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()));
    saveBookmarkLines(lines);
}

void FilesService::unpin(const QString &path)
{
    QStringList lines = loadBookmarkLines();
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const QString &l) {
                    const int space = l.indexOf(' ');
                    return QUrl(space > 0 ? l.left(space) : l).toLocalFile() == path;
                }), lines.end());
    saveBookmarkLines(lines);
}

// ---------- буфер, имена ----------

void FilesService::clipboardPut(const QStringList &paths, bool cut)
{
    m_clipboard = paths;
    m_cut = cut;
    emit clipboardChanged();
}

void FilesService::paste(const QString &dest)
{
    if (m_clipboard.isEmpty() || dest.isEmpty())
        return;
    if (m_cut) {
        m_ops.run(FilesOps::Move, m_clipboard, dest);
        m_clipboard.clear();   // вырезанное вставляется один раз
        emit clipboardChanged();
    } else {
        m_ops.run(FilesOps::Copy, m_clipboard, dest);
    }
}

QString FilesService::rename(const QString &path, const QString &newName)
{
    m_error.clear();
    const QString name = newName.trimmed();
    const QFileInfo fi(path);
    if (name.isEmpty() || name == fi.fileName())
        return path;
    if (name.contains('/') || name == QLatin1String(".") || name == QLatin1String("..")) {
        m_error = "такое имя нельзя";
        return {};
    }
    const QString target = fi.absolutePath() + "/" + name;
    if (QFileInfo::exists(target)) {
        m_error = "имя занято";
        return {};
    }
    if (!QFile::rename(path, target)) {
        m_error = "не вышло";
        return {};
    }
    m_ops.rememberRename(path, target);
    if (isStarred(path)) {
        m_starred.replace(m_starred.indexOf(path), target);
        saveStarred();
        emit starredChanged();
    }
    return target;
}

QString FilesService::newFolder(const QString &dir)
{
    const QString path = FilesOps::uniqueChild(dir, QStringLiteral("Новая папка"));
    if (!QDir().mkdir(path)) {
        m_error = "не вышло";
        return {};
    }
    m_ops.rememberCreated(path, QStringLiteral("НОВУЮ ПАПКУ"));
    return path;
}

// ---------- обои стола (docs/DESKTOP.md, «Настройки») ----------
// Стол рисует ~/.config/naomi/desktop/wallpaper.jpg; выбранная картинка — копия из собрания (wallpapers/), какая —
// в wallpaper.conf. Картинка не из собрания («Сделать фоном» в «Файлах») сначала ложится туда: её видно в «Обоях».

QStringList FilesService::wallpapers() const
{
    QStringList out;
    const QDir d(wallDir());
    for (const QString &name : d.entryList(QDir::Files | QDir::Readable, QDir::Name))
        out.append(d.filePath(name));
    return out;
}

QString FilesService::wallpaper() const
{
    const QString name = QSettings(wallConf(), QSettings::IniFormat).value("current").toString();
    const QString file = deskDir() + "/wallpaper.jpg";
    const QFileInfo info(file);
    if (name == "-" || !info.isFile() || !info.isReadable())
        return {};
    if (name.isEmpty())   // не выбирали в «Настройках» — та, что стоит
        return file;
    return wallDir() + "/" + name;
}

QUrl FilesService::desktopWallpaper() const
{
    if (wallpaper().isEmpty())
        return {};
    QUrl url = QUrl::fromLocalFile(deskDir() + "/wallpaper.jpg");
    url.setQuery(QString::number(m_wallpaperVersion));
    return url;
}

void FilesService::setWallpaper(const QString &path)
{
    m_error.clear();
    QDir().mkpath(wallDir());
    QString name = "-";
    if (!path.isEmpty()) {
        QString pick = path;
        const QFileInfo src(path);
        if (src.absolutePath() != QFileInfo(wallDir()).absoluteFilePath()) {
            // в собрание: то же имя и размер — уже там; имя занято другой — «имя (2)»
            pick = wallDir() + "/" + src.fileName();
            for (int n = 2; QFile::exists(pick) && QFileInfo(pick).size() != src.size(); ++n)
                pick = wallDir() + "/" + src.completeBaseName() + QStringLiteral(" (%1).").arg(n) + src.suffix();
            if (!QFile::exists(pick) && !copyWhole(path, pick)) {
                m_error = "не вышло";
                return;
            }
        }
        if (!copyWhole(pick, deskDir() + "/wallpaper.jpg")) {
            m_error = "не вышло";
            return;
        }
        name = QFileInfo(pick).fileName();
    }
    QSettings(wallConf(), QSettings::IniFormat).setValue("current", name);
    ++m_wallpaperVersion;
    emit wallpaperChanged();
}

// ---------- вид окна, путь ----------

QVariant FilesService::setting(const QString &key, const QVariant &fallback) const
{
    if (const auto it = m_written.constFind(key); it != m_written.constEnd())
        return *it;   // записанное — сразу, файл пишет свой поток
    return QSettings(settingsFile(), QSettings::IniFormat).value(key, fallback);
}

// Запись — своим потоком, по порядку: QSettings, уходя из области, пишет файл и ждёт диск (fdatasync) — в потоке
// интерфейса стола это 10–20 мс посреди смены вида «Файлов» (прогон стола 19 ноября 2026, выборки стека)
void FilesService::setSetting(const QString &key, const QVariant &value)
{
    m_written.insert(key, value);
    QThreadPool::globalInstance()->start([file = settingsFile(), key, value] {
        static QMutex order;
        QMutexLocker lock(&order);
        QSettings(file, QSettings::IniFormat).setValue(key, value);
    });
}

QVariantList FilesService::crumbs(const QString &location) const
{
    if (location == QLatin1String("starred:"))
        return {QVariantMap{{"label", "ИЗБРАННОЕ"}, {"path", location}}};
    if (location == QLatin1String("trash:"))
        return {QVariantMap{{"label", "КОРЗИНА"}, {"path", location}}};
    QVariantList out;
    const QString home = QDir::homePath();
    QString p = QDir::cleanPath(location);
    for (;;) {
        const bool isHome = p == home;
        const QString label = isHome ? QStringLiteral("ДОМ") : p == QLatin1String("/") ? QStringLiteral("СИСТЕМА")
                                                                                        : QFileInfo(p).fileName();
        out.prepend(QVariantMap{{"label", label}, {"path", p}});
        if (isHome || p == QLatin1String("/"))
            break;
        p = QFileInfo(p).absolutePath();
    }
    return out;
}

QString FilesService::parentOf(const QString &path) const
{
    if (path == QLatin1String("/") || path.isEmpty() || path.endsWith(':'))
        return {};
    return QFileInfo(QDir::cleanPath(path)).absolutePath();
}

bool FilesService::isDir(const QString &path) const
{
    return QFileInfo(path).isDir();
}
