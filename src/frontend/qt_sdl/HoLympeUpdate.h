/*
    Ho-Lympe - automatic updates. The installed copy has a manifest.txt (written by the
    installer, tools/setup): at start the launcher compares it with the server's, downloads
    the files that changed into update-tmp/, then lets updater.exe put them in place.
*/

#ifndef HOLYMPEUPDATE_H
#define HOLYMPEUPDATE_H

#include <functional>
#include <QString>

// Version of this copy ("dev" when it was not installed by the installer)
QString HoLympe_LocalVersion();

// progress(percent, text) while downloading; done(restart, error): restart = the updater
// is starting, the app must quit now
void HoLympe_CheckUpdate(std::function<void(int, const QString&)> progress,
                         std::function<void(bool, const QString&)> done);

#endif // HOLYMPEUPDATE_H
