// НаомиФон — «Файлы» рабочего стола: общее на все окна (naomiFiles в QML; docs/FILES.md). Перенос files.c и
// files-places.c НаомиОС, под файловую систему телефона:
//   • избранное — ~/.config/naomi/files/starred (адреса file:// построчно, как ~/.config/naomios/starred);
//   • закреплённые («02 СВОИ») — закладки GTK ~/.config/gtk-3.0/bookmarks (их видят окна «Открыть» программ);
//   • корзина — ~/.local/share/Trash (стандарт freedesktop), счётчик — по её содержимому;
//   • буфер «копировать / вырезать» — один на все окна; вставка из «вырезано» перемещает;
//   • операции — FilesOps (очередь, ход, отмена); вид окна (плитка / список, порядок) — ~/.config/naomi/files.conf.
#pragma once

#include "files_ops.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariant>

class FilesService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QStringList starred READ starred NOTIFY starredChanged)
    Q_PROPERTY(int trashCount READ trashCount NOTIFY trashChanged)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    Q_PROPERTY(QString status READ status NOTIFY opsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY opsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY opsChanged)
    Q_PROPERTY(QString undoWhat READ undoWhat NOTIFY opsChanged)
    Q_PROPERTY(bool hasClipboard READ hasClipboard NOTIFY clipboardChanged)
    // обои стола: собрание (картинки — «Настройки» → «Обои») и какие стоят — путь в нём; "" — без картинки
    Q_PROPERTY(QStringList wallpapers READ wallpapers NOTIFY wallpaperChanged)
    Q_PROPERTY(QString wallpaper READ wallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(QUrl desktopWallpaper READ desktopWallpaper NOTIFY wallpaperChanged)

public:
    explicit FilesService(QObject *parent = nullptr);
    static FilesService *instance() { return s_instance; }

    QString home() const;
    QStringList starred() const { return m_starred; }
    int trashCount() const { return m_trashCount; }
    QVariantList bookmarks() const;
    QString status() const { return m_ops.status(); }
    bool busy() const { return m_ops.busy(); }
    bool canUndo() const { return m_ops.canUndo(); }
    QString undoWhat() const { return m_ops.undoWhat(); }
    bool hasClipboard() const { return !m_clipboard.isEmpty(); }
    QStringList wallpapers() const;
    QString wallpaper() const;
    QUrl desktopWallpaper() const;

    Q_INVOKABLE bool isStarred(const QString &path) const { return m_starred.contains(path); }
    Q_INVOKABLE void setStarred(const QStringList &paths, bool on);
    Q_INVOKABLE void pin(const QString &path);
    Q_INVOKABLE void unpin(const QString &path);

    Q_INVOKABLE void drop(const QStringList &paths, const QString &dest) { m_ops.run(FilesOps::Drop, paths, dest); }
    Q_INVOKABLE void trash(const QStringList &paths) { m_ops.run(FilesOps::Trash, paths); }
    Q_INVOKABLE void deleteForever(const QStringList &paths) { m_ops.run(FilesOps::Delete, paths); }
    Q_INVOKABLE void restore(const QStringList &paths) { m_ops.run(FilesOps::Restore, paths); }
    Q_INVOKABLE void emptyTrash() { m_ops.run(FilesOps::EmptyTrash, {}); }
    Q_INVOKABLE void undo() { m_ops.undo(); }
    Q_INVOKABLE void cancel() { m_ops.cancel(); }

    Q_INVOKABLE void clipboardPut(const QStringList &paths, bool cut);
    Q_INVOKABLE void paste(const QString &dest);
    // переименовать: новый путь — или пусто (ошибка — в lastError)
    Q_INVOKABLE QString rename(const QString &path, const QString &newName);
    // «Новая папка» (занято — «Новая папка (2)»): путь — или пусто
    Q_INVOKABLE QString newFolder(const QString &dir);
    Q_INVOKABLE QString lastError() const { return m_error; }
    // фоном рабочего стола (docs/DESKTOP.md): картинка — сначала в собрание, если не оттуда; "" — без картинки
    Q_INVOKABLE void setWallpaper(const QString &path);

    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);

    // для пути наверху: «ДОМ», части до дома (или до корня — «СИСТЕМА»)
    Q_INVOKABLE QVariantList crumbs(const QString &location) const;
    Q_INVOKABLE QString parentOf(const QString &path) const;
    Q_INVOKABLE bool isDir(const QString &path) const;

signals:
    void starredChanged();
    void trashChanged();
    void bookmarksChanged();
    void opsChanged();
    void clipboardChanged();
    void wallpaperChanged();

private:
    void loadStarred();
    void saveStarred();
    QStringList loadBookmarkLines() const;
    void saveBookmarkLines(const QStringList &lines);
    void refreshTrash();
    bool isPinned(const QString &path) const;

    static FilesService *s_instance;
    QHash<QString, QVariant> m_written;   // вид окна: записанное (setSetting) — файл пишет свой поток
    FilesOps m_ops;
    QStringList m_starred;
    QStringList m_clipboard;
    bool m_cut = false;
    int m_trashCount = 0;
    int m_wallpaperVersion = 0;
    QString m_error;
    QFileSystemWatcher m_trashWatch;
};
