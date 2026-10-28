/*
    Ho-Lympe - see HoLympeSession.h.
*/

#include "HGCoop.h"
#include "HoLympeSession.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QMessageBox>
#include <QTimer>
#include <zlib.h>

#include "GameUi.h"
#include "HoLympeApi.h"
#include "HoLympeUpdate.h"

void HoLympeSession::Start(const QString& world, const QString& saveFile, bool online)
{
    new HoLympeSession(world, saveFile, online); // lives until the app quits
}

HoLympeSession::HoLympeSession(const QString& world, const QString& saveFile, bool online)
    : QObject(qApp), world(world), saveFile(saveFile), online(online)
{
    // Nuzlocke: the whole team fell -> end of the run
    auto* over = new QTimer(this);
    QObject::connect(over, &QTimer::timeout, this, [this] {
        if (!ended && GameUi::Get().IsNuzlockeOver())
            endRun();
    });
    over->start(1000);
    HoLympe_WatchUpdates();
    if (!online)
    {
        // Solo: the look is kept on this PC (online, it goes with the save's summary)
        auto* look = new QTimer(this);
        QObject::connect(look, &QTimer::timeout, this, [this] {
            QString key = "soloLook-" + this->world.mid(5); // "solo-classique" -> "classique"
            QString now = GameUi::Get().LookString();
            if (HoLympeApi::Settings().value(key).toString() != now)
                HoLympeApi::Settings().setValue(key, now);
        });
        look->start(5000);
        return;
    }

    QFileInfo info(saveFile);
    lastModified = info.exists() ? info.lastModified() : QDateTime();
    lastSize = info.exists() ? info.size() : -1;

    auto* presence = new QTimer(this);
    QObject::connect(presence, &QTimer::timeout, this, [this] { sendPresence(); });
    presence->start(15000);
    sendPresence();

    auto* saves = new QTimer(this);
    QObject::connect(saves, &QTimer::timeout, this, [this] { checkSave(); });
    saves->start(2000);

    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { finish(); });
}

void HoLympeSession::sendPresence()
{
    QJsonObject body { { "world", world }, { "location", GameUi::Get().LocationName() } };
    HoLympeApi::Get().post("/api/presence", body, [](bool ok, const QJsonObject& reply, const QString&) {
        if (ok && reply.value("kicked").toBool())
        {
            QMessageBox::warning(nullptr, "Ho-Lympe", QString::fromUtf8("L'admin t'a expulsé du serveur."));
            qApp->quit();
        }
    });
}

// The game writes its save in several steps: upload once the file stopped changing
void HoLympeSession::checkSave()
{
    QFileInfo info(saveFile);
    if (!info.exists())
        return;
    if (info.lastModified() == lastModified && info.size() == lastSize)
    {
        if (stableChecks > 0 && --stableChecks == 0)
            uploadSave(false);
        return;
    }
    lastModified = info.lastModified();
    lastSize = info.size();
    stableChecks = 2;
}

void HoLympeSession::uploadSave(bool wait)
{
    QFile file(saveFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QByteArray data = file.readAll();
    if (data.isEmpty())
        return;
    // The Nuzlocke state and cemetery live next to the save
    QString dir = QFileInfo(saveFile).absolutePath();
    QFile nuz(dir + "/nuzlocke.dat"), cemetery(dir + "/cemetery.json");
    QByteArray nuzData = nuz.open(QIODevice::ReadOnly) ? nuz.readAll() : QByteArray();
    QJsonArray graves = cemetery.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(cemetery.readAll()).array() : QJsonArray();
    QJsonObject body { { "save", QString::fromLatin1(data.toBase64()) }, { "summary", GameUi::Get().SaveSummary() },
                       { "nuzlocke", QString::fromLatin1(nuzData.toBase64()) }, { "cemetery", graves } };
    QString path = "/api/save/" + world;
    if (wait)
    {
        HoLympeApi::Get().postAndWait(path, body, 8000);
        return;
    }
    uploading = true;
    HoLympeApi::Get().post(path, body, [this](bool ok, const QJsonObject&, const QString&) {
        uploading = false;
        if (!ok)
            stableChecks = 5; // retry a bit later
    });
}

// The run is over: the server (or this PC) archives it, Ho-Lympe restarts on the launcher
void HoLympeSession::endRun()
{
    ended = true;
    QTimer::singleShot(6000, this, [this] {
        QString dir = QFileInfo(saveFile).absolutePath();
        if (online)
        {
            HoLympeApi::Get().postAndWait("/api/nuzlocke/lost", QJsonObject(), 8000);
            HoLympeApi::Get().postAndWait("/api/presence", QJsonObject { { "world", "" } }, 3000);
        }
        else
        {
            // Solo: the cemetery is kept as a souvenir, the run starts over with a new seed
            QString stamp = QString::number(QDateTime::currentSecsSinceEpoch());
            QFile::rename(dir + "/cemetery.json", dir + "/cemetery-" + stamp + ".json");
            HoLympeApi::Settings().remove("soloSeed");
        }
        QFile::remove(saveFile);
        QFile::remove(dir + "/nuzlocke.dat");
        QFile::remove(dir + "/cemetery.json");
        // The new Ho-Lympe opens on the launcher: this game's settings (HGCOOP_MODE, the old seed...)
        // must not reach it, or it starts the game again at once with the lost run's seed
        for (const QString& key : QProcessEnvironment::systemEnvironment().keys())
        {
            if (key.startsWith("HGCOOP_"))
                qunsetenv(key.toUtf8().constData());
        }
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
        qApp->exit(0);
    });
}

void HoLympeSession::finish()
{
    if (ended || !online)
        return;
    // The emulator writes the save file when it stops: send the latest one
    QFileInfo info(saveFile);
    if (info.exists() && (info.lastModified() != lastModified || info.size() != lastSize || stableChecks > 0))
        uploadSave(true);
    HoLympeApi::Get().postAndWait("/api/presence", QJsonObject { { "world", "" } }, 3000);
}

// ---------------------------------------------------------------------------
// A new version published while playing: the game keeps running (and saving), the player
// just learns that the next start will update
// ---------------------------------------------------------------------------

void HoLympe_WatchUpdates()
{
    // HOLYMPE_FAKE_VERSION: tests (a dev copy has no version of its own)
    static QString local = qEnvironmentVariableIsSet("HOLYMPE_FAKE_VERSION") ? qEnvironmentVariable("HOLYMPE_FAKE_VERSION") : HoLympe_LocalVersion();
    static QString told;
    if (local == "dev")
        return;
    auto check = [] {
        HoLympeApi::Get().getRaw("/api/update/manifest.txt", [](bool ok, const QByteArray& data, const QString&) {
            if (!ok)
                return;
            QString first = QString::fromUtf8(data).section('\n', 0, 0).trimmed();
            if (!first.startsWith("version "))
                return;
            QString remote = first.mid(8).trimmed();
            if (remote.isEmpty() || remote == local || remote == told)
                return;
            told = remote;
            GameUi::Get().NuzlockeMessage(QString::fromUtf8("Mise à jour %1 disponible : relance Ho-Lympe quand tu veux").arg(remote),
                                          QColor(240, 186, 40));
        });
    };
    auto* timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, qApp, check);
    timer->start(5 * 60 * 1000);
    QTimer::singleShot(qEnvironmentVariableIsSet("HOLYMPE_FAKE_VERSION") ? 12000 : 60 * 1000, qApp, check);
}

// ---------------------------------------------------------------------------
// The game, built from the player's ROM
// ---------------------------------------------------------------------------

static QString GameDir()
{
    return HoLympeDir() + "/game";
}

QString HoLympe_GamePath()
{
    QString shipped = GameDir() + "/HeartGold.nds";
    if (QFile::exists(GameDir() + "/HoLympe.hgpatch") || QFile::exists(shipped))
        return shipped;
    // Developer setup: the game built from the decomp
    QString dev = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../build/coop-p1.nds");
    return QFile::exists(dev) ? dev : shipped;
}

static quint32 ReadU32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return u[0] | (u[1] << 8) | (u[2] << 16) | ((quint32)u[3] << 24);
}

// The patch says which game it makes (size + CRC): the game is ready when it matches
bool HoLympe_GameReady()
{
    QString game = HoLympe_GamePath();
    QFile patchFile(GameDir() + "/HoLympe.hgpatch");
    if (!patchFile.open(QIODevice::ReadOnly))
        return QFile::exists(game); // no patch: developer setup
    QByteArray header = patchFile.read(24);
    if (header.size() < 24 || !QFile::exists(game))
        return false;
    QFile stamp(GameDir() + "/HeartGold.ok");
    return stamp.open(QIODevice::ReadOnly) && stamp.readAll() == header.toHex();
}

QString HoLympe_BuildGame(const QString& romPath)
{
    QFile romFile(romPath), patchFile(GameDir() + "/HoLympe.hgpatch");
    if (!romFile.open(QIODevice::ReadOnly))
        return QString::fromUtf8("Impossible de lire ce fichier.");
    if (!patchFile.open(QIODevice::ReadOnly))
        return QString::fromUtf8("Il manque le fichier du jeu (game/HoLympe.hgpatch). Réinstalle Ho-Lympe.");
    QByteArray base = romFile.readAll(), patch = patchFile.readAll();
    if (patch.size() < 24 || !patch.startsWith("HGCP"))
        return QString::fromUtf8("Le fichier du jeu est abîmé. Réinstalle Ho-Lympe.");

    quint32 baseSize = ReadU32(patch.constData() + 8), baseCrc = ReadU32(patch.constData() + 12);
    quint32 targetSize = ReadU32(patch.constData() + 16), targetCrc = ReadU32(patch.constData() + 20);
    if ((quint32)base.size() != baseSize
        || crc32(0, reinterpret_cast<const Bytef*>(base.constData()), base.size()) != baseCrc)
        return QString::fromUtf8("Ce n'est pas la bonne ROM : il faut « Pokémon - Version Or HeartGold (France) ».");

    // Operations: copy a block of the ROM, or data from the patch
    QByteArray ops(patch.size() * 4, Qt::Uninitialized);
    for (;;)
    {
        uLongf size = ops.size();
        int err = uncompress(reinterpret_cast<Bytef*>(ops.data()), &size,
                             reinterpret_cast<const Bytef*>(patch.constData() + 24), patch.size() - 24);
        if (err == Z_OK)
        {
            ops.resize(size);
            break;
        }
        if (err != Z_BUF_ERROR)
            return QString::fromUtf8("Le fichier du jeu est abîmé. Réinstalle Ho-Lympe.");
        ops.resize(ops.size() * 2);
    }
    QByteArray out;
    out.reserve(targetSize);
    for (qsizetype i = 0; i + 5 <= ops.size();)
    {
        char kind = ops[i];
        quint32 len = ReadU32(ops.constData() + i + 1);
        i += 5;
        if (kind == 0)
        {
            quint32 src = ReadU32(ops.constData() + i);
            i += 4;
            if ((qint64)src + len > base.size())
                break;
            out.append(base.constData() + src, len);
        }
        else
        {
            if (i + len > ops.size())
                break;
            out.append(ops.constData() + i, len);
            i += len;
        }
    }
    if ((quint32)out.size() != targetSize
        || crc32(0, reinterpret_cast<const Bytef*>(out.constData()), out.size()) != targetCrc)
        return QString::fromUtf8("La création du jeu n'a pas marché.");

    QDir().mkpath(GameDir());
    // The player's ROM is kept: each update rebuilds the game from it
    if (QFileInfo(romPath).absoluteFilePath() != QFileInfo(GameDir() + "/base.nds").absoluteFilePath())
    {
        QFile::remove(GameDir() + "/base.nds");
        QFile::copy(romPath, GameDir() + "/base.nds");
    }
    QFile game(GameDir() + "/HeartGold.nds");
    if (!game.open(QIODevice::WriteOnly) || game.write(out) != out.size())
        return QString::fromUtf8("Impossible d'écrire le jeu dans le dossier de Ho-Lympe.");
    game.close();
    QFile stamp(GameDir() + "/HeartGold.ok");
    if (stamp.open(QIODevice::WriteOnly))
        stamp.write(patch.left(24).toHex());
    return QString();
}

