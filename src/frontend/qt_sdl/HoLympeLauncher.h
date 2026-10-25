/*
    Ho-Lympe - the launcher window: account, the two servers, solo games, friends, news,
    settings and the admin panel. Shown before the game starts.
*/

#ifndef HOLYMPELAUNCHER_H
#define HOLYMPELAUNCHER_H

#include <QString>

struct HoLympeChoice
{
    QString romPath;  // the game to start
    QString saveFile; // where it saves
};

// Shows the launcher. Returns false when the player closed it (quit the app).
// On success, the environment is set up for the game (mode, server, name, save folder)
// and an online session is started when playing on a server.
bool HoLympe_RunLauncher(HoLympeChoice& choice);

#endif // HOLYMPELAUNCHER_H
