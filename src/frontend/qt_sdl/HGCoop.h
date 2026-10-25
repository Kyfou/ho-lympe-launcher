/*
    HeartGold Coop - branding and display mode of the custom launcher.

    Game mode (default): branded window, no emulator menus, fullscreen.
      HGCOOP_WINDOWED=1  keeps a window (e.g. two players on one PC)
      HGCOOP_DEV=1       restores the regular melonDS interface
*/

#ifndef HGCOOP_H
#define HGCOOP_H

#include <stdlib.h>
#include <QCoreApplication>
#include <QDir>
#include <QString>

#define HGCOOP_TITLE "Pokémon HeartGold Coop"

inline bool HGCoop_DevMode() { return getenv("HGCOOP_DEV") != nullptr; }
inline bool HGCoop_Fullscreen() { return !HGCoop_DevMode() && getenv("HGCOOP_WINDOWED") == nullptr; }

// Where Ho-Lympe keeps its files (coop.ini, saves, the game built from the player's ROM, freeze
// reports, melonDS.toml): next to the program on Windows; a macOS app can't write in its own
// bundle: ~/Library/Application Support/Ho-Lympe
inline QString HoLympeDir()
{
#ifdef __APPLE__
    QString dir = QDir::homePath() + "/Library/Application Support/Ho-Lympe";
    QDir().mkpath(dir);
    return dir;
#else
    return QCoreApplication::applicationDirPath();
#endif
}

// The HD pictures shipped with the launcher (inside the bundle on macOS: Contents/Resources)
inline QString HoLympeAssetsDir()
{
#ifdef __APPLE__
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources/assets");
#else
    return QCoreApplication::applicationDirPath() + "/assets";
#endif
}

#endif // HGCOOP_H
