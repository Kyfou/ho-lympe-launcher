/*
    Ho-Lympe - see HoLympeUpdate.h.
*/

#include "HoLympeUpdate.h"

#include <memory>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

#include "HoLympeApi.h"
#include "HGCoop.h"

namespace
{

struct Entry
{
    QString sha, flags, path;
    qint64 size = 0;
};

struct Manifest
{
    QString version;
    QList<Entry> files;
};

Manifest Parse(const QByteArray& text)
{
    Manifest m;
    for (const QByteArray& raw : text.split('\n'))
    {
        QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith("version "))
            m.version = line.mid(8).trimmed();
        else if (line.startsWith("file "))
        {
            QStringList parts = line.mid(5).split(' ');
            if (parts.size() >= 4)
                m.files.append({ parts[0], parts[2], parts.mid(3).join(' '), parts[1].toLongLong() });
        }
    }
    return m;
}

// Windows: the files are next to the program, the server's manifest lists them all.
// macOS: they are in Ho-Lympe's folder (Application Support); the server lists the Mac's own files
// in mac/manifest.txt: the game patch, and the app itself (mac/Ho-Lympe-mac.zip)
QString AppDir()
{
#ifdef __APPLE__
    return HoLympeDir();
#else
    return QCoreApplication::applicationDirPath();
#endif
}

#ifdef __APPLE__
const char* const kManifestUrl = "/update/mac/manifest.txt";
const char* const kAppZip = "mac/Ho-Lympe-mac.zip";

// The version of this app, written in the bundle when it was built (Contents/Resources)
QString BundleVersion()
{
    QFile f(QCoreApplication::applicationDirPath() + "/../Resources/holympe-version.txt");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}
#else
const char* const kManifestUrl = "/api/update/manifest.txt";
#endif

QByteArray ReadAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// One download after the other, then the updater
struct Job
{
    Manifest remote;
    QByteArray remoteText;
    QList<Entry> todo;
    qint64 total = 0, done = 0;
    int index = 0;
    std::function<void(int, const QString&)> progress;
    std::function<void(bool, const QString&)> finished;
};

void Next(std::shared_ptr<Job> job);

#ifdef __APPLE__
// macOS: the game's files go in place now; a new app replaces this one through a little script,
// once Ho-Lympe has quit, then opens again
void StartUpdater(std::shared_ptr<Job> job)
{
    QString dir = AppDir(), tmp = dir + "/update-tmp";
    for (const Entry& e : job->todo)
    {
        if (e.path == kAppZip)
            continue;
        QString target = dir + "/" + e.path;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(target);
        if (!QFile::rename(tmp + "/" + e.path, target))
            return job->finished(false, QString::fromUtf8("Impossible d'installer la mise à jour sur ce Mac."));
    }
    QFile manifest(dir + "/manifest.txt");
    if (manifest.open(QIODevice::WriteOnly))
        manifest.write(job->remoteText);
    manifest.close();

    QString zip = tmp + "/" + kAppZip;
    if (!QFile::exists(zip))
    {
        QDir(tmp).removeRecursively();
        return job->finished(false, QString()); // only the game changed: play on
    }
    QString bundle = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    QString script = tmp + "/update.sh";
    QFile sh(script);
    if (!sh.open(QIODevice::WriteOnly))
        return job->finished(false, QString::fromUtf8("Impossible d'installer la mise à jour sur ce Mac."));
    QString q = "'";
    auto quote = [&](const QString& s) { return q + QString(s).replace(q, "'\\''") + q; };
    sh.write(QString("#!/bin/sh\n"
                     "while kill -0 %1 2>/dev/null; do sleep 0.3; done\n"
                     "rm -rf %2 && mkdir -p %2 && ditto -x -k %3 %2 || exit 1\n"
                     "rm -rf %4 && mv %2/Ho-Lympe.app %4\n"
                     "xattr -dr com.apple.quarantine %4 2>/dev/null\n"
                     "open %4\n")
                 .arg(QCoreApplication::applicationPid())
                 .arg(quote(tmp + "/app"), quote(zip), quote(bundle))
                 .toUtf8());
    sh.close();
    if (!QProcess::startDetached("/bin/sh", { script }))
        return job->finished(false, QString::fromUtf8("Impossible de lancer la mise à jour."));
    job->finished(true, QString());
}
#else
void StartUpdater(std::shared_ptr<Job> job)
{
    QString tmp = AppDir() + "/update-tmp";
    QDir().mkpath(tmp);
    QFile manifest(tmp + "/manifest.txt");
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(job->remoteText) != job->remoteText.size())
        return job->finished(false, QString::fromUtf8("Impossible d'écrire la mise à jour sur ce PC."));
    manifest.close();
    // The updater replaces itself too: a copy of it runs from the temp folder
    QString source = QFile::exists(tmp + "/updater.exe") ? tmp + "/updater.exe" : AppDir() + "/updater.exe";
    QString runner = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/holympe-updater.exe";
    QFile::remove(runner);
    if (!QFile::copy(source, runner))
        return job->finished(false, QString::fromUtf8("Le programme de mise à jour est introuvable. Réinstalle Ho-Lympe."));
    QStringList args { "--apply", QDir::toNativeSeparators(tmp), "--wait", QString::number(QCoreApplication::applicationPid()),
                       "--run", QDir::toNativeSeparators(AppDir() + "/Ho-Lympe.exe") };
    if (!QProcess::startDetached(runner, args))
        return job->finished(false, QString::fromUtf8("Impossible de lancer la mise à jour."));
    job->finished(true, QString());
}
#endif

void Next(std::shared_ptr<Job> job)
{
    if (job->index >= job->todo.size())
        return StartUpdater(job);
    const Entry& e = job->todo[job->index];
    int percent = job->total ? (int)(job->done * 100 / job->total) : 100;
    job->progress(percent, QString::fromUtf8("Mise à jour %1 — %2 %").arg(job->remote.version).arg(percent));
    QString urlPath = "/update/" + QString::fromLatin1(QUrl::toPercentEncoding(e.path, "/"));
    HoLympeApi::Get().getRaw(urlPath, [job, e](bool ok, const QByteArray& data, const QString& error) {
        if (!ok)
            return job->finished(false, error);
        if (QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != e.sha)
            return job->finished(false, QString::fromUtf8("Un fichier de la mise à jour est arrivé abîmé, réessaie."));
        QString target = AppDir() + "/update-tmp/" + e.path;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile f(target);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            return job->finished(false, QString::fromUtf8("Impossible d'écrire la mise à jour sur ce PC."));
        job->done += e.size;
        job->index++;
        Next(job);
    });
}

}

QString HoLympe_LocalVersion()
{
    static QString version = [] {
        Manifest m = Parse(ReadAll(AppDir() + "/manifest.txt"));
#ifdef __APPLE__
        // The first time, the app's own version (the game's files come with the first update)
        return m.version.isEmpty() ? (BundleVersion().isEmpty() ? QString("0") : BundleVersion() + "-new") : m.version;
#else
        return m.version.isEmpty() ? QString("dev") : m.version;
#endif
    }();
    return version;
}

void HoLympe_CheckUpdate(std::function<void(int, const QString&)> progress,
                         std::function<void(bool, const QString&)> done)
{
    if (HoLympe_LocalVersion() == "dev")
        return done(false, QString()); // developer copy: updated by hand
    progress(0, QString::fromUtf8("Recherche de mises à jour…"));
    HoLympeApi::Get().getRaw(kManifestUrl, [progress, done](bool ok, const QByteArray& text, const QString&) {
        Manifest remote = Parse(text);
        if (!ok || remote.version.isEmpty() || remote.version == HoLympe_LocalVersion())
            return done(false, QString()); // up to date, or the server can't be reached: play anyway
        Manifest local = Parse(ReadAll(AppDir() + "/manifest.txt"));
        QMap<QString, QString> known;
        for (const Entry& e : local.files)
            known[e.path] = e.sha;

        auto job = std::make_shared<Job>();
        job->remote = remote;
        job->remoteText = text;
        job->progress = progress;
        job->finished = done;
        for (const Entry& e : remote.files)
        {
#ifdef __APPLE__
            if (e.path == kAppZip && BundleVersion() == remote.version)
                continue; // this app is already that version
#endif
            bool exists = QFile::exists(AppDir() + "/" + e.path);
            if (e.flags == "once" && exists)
                continue; // the player's own settings
            if (exists && known.value(e.path) == e.sha)
                continue;
            job->todo.append(e);
            job->total += e.size;
        }
        QDir(AppDir() + "/update-tmp").removeRecursively();
        Next(job);
    });
}
