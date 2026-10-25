/*
    HeartGold Coop - launch screen: play alone, host a game or join a friend.

    Shown before the game starts (game mode, when HGCOOP_MODE is not already set,
    e.g. by Coop-Test-Local.bat). Sets HGCOOP_MODE / HGCOOP_ADDR for CoopLink.
*/

#ifndef COOPLOBBY_H
#define COOPLOBBY_H

// Returns false when the player closed the window (quit)
bool CoopLobby_Run();

#endif // COOPLOBBY_H
