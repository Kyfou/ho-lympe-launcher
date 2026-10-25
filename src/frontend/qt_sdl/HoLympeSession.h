/*
    Ho-Lympe - while playing on a server: sends the save to the server each time the game
    writes it, tells the server we are online (and where), and does a last upload on exit.
    Also builds the game from the player's own ROM (the .hgpatch shipped with the launcher).
*/

#ifndef HOLYMPESESSION_H
#define HOLYMPESESSION_H

#include <QDateTime>
#include <QObject>
#include <QString>

class QTimer;

class HoLympeSession : public QObject
{
public:
    // Starts the session: online on a server ("classique" / "nuzlocke"), or a solo game
    // ("solo-classique" / "solo-nuzlocke": no upload, only the end of a Nuzlocke run)
    static void Start(const QString& world, const QString& saveFile, bool online);

private:
    HoLympeSession(const QString& world, const QString& saveFile, bool online);
    void endRun();
    void sendPresence();
    void checkSave();
    void uploadSave(bool wait);
    void finish();

    QString world;
    QString saveFile;
    QDateTime lastModified;
    qint64 lastSize = -1;
    int stableChecks = 0;
    bool uploading = false;
    bool online = true;
    bool ended = false; // the Nuzlocke run is over: nothing more to upload
};

// While playing: tells the player (in game) when a new version is out, to restart when they want
void HoLympe_WatchUpdates();

// The game file built from the player's ROM: <exe>/game/HeartGold.nds
// (in the developer setup, the ROM built from the decomp)
QString HoLympe_GamePath();
bool HoLympe_GameReady();
// Builds the game from the player's ROM; returns an error message, empty when it worked
QString HoLympe_BuildGame(const QString& romPath);

#endif // HOLYMPESESSION_H
