/*
    HeartGold Coop - modern HD menus drawn by the launcher over the game.
    See GameUi.h.
*/

#include "HGCoop.h"
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QFile>
#include <QDateTime>
#include <QCoreApplication>
#include <QJsonArray>
#include <QLinearGradient>
#include <cstring>
#include <unordered_map>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <cmath>

#include "GameUi.h"
#include "CoopLink.h"
#include "NDS.h"

using namespace melonDS;

// CoopUiMenu fields (pokeheartgold include/coop.h)
enum
{
    kUiType = 0,
    kUiCount = 1,
    kUiCursor = 2,
    kUiHostPresent = 3,
    kUiResult = 4,
    kUiRequestId = 5,
    kUiMoney = 8,
    kUiEntries = 12,
    kUiMaxEntries = 12,
};

enum
{
    kUiResultWaiting = -1,
    kUiResultCancel = -2,
};

// StartMenuAction values (pokeheartgold src/start_menu.c)
enum
{
    kActionPokedex = 0,
    kActionPokemon = 1,
    kActionBag = 2,
    kActionTrainerCard = 3,
    kActionSave = 4,
    kActionOptions = 5,
    kActionExit = 6,
    kActionRetire = 8,
    kActionPokegear = 11,
    kActionPokegearAlt = 12,
    kActionPlayers = 200, // Ho-Lympe: online players (launcher only, the game never sees it)
    kActionCemetery = 201, // Ho-Lympe: the Nuzlocke cemetery (launcher only)
};

// DS buttons, as 12 bits: A B SELECT START RIGHT LEFT UP DOWN R L X Y
enum
{
    kKeyA = 1 << 0,
    kKeyB = 1 << 1,
    kKeyRight = 1 << 4,
    kKeyLeft = 1 << 5,
    kKeyUp = 1 << 6,
    kKeyDown = 1 << 7,
    kKeyX = 1 << 10,
    kKeyR = 1 << 8,
    kKeyL = 1 << 9,
    kKeyDirections = kKeyRight | kKeyLeft | kKeyUp | kKeyDown,
};

// The start menu's grid: 2 columns, 3 when there are many entries (Pokedex, Players, Cemetery...)
static int MenuColumns(int count)
{
    return count > 6 ? 3 : 2;
}

// CoopUiScreen (pokeheartgold include/coop.h)
static constexpr int kScreenParty = 1;
static constexpr int kScreenBag = 2;
static constexpr int kScreenSummary = 3;
static constexpr int kScreenBattle = 4;
static constexpr int kChatShowFrames = 60 * 12; // a chat line stays 12 seconds on screen
static constexpr int kChatMaxLength = 120;
static constexpr int kScreenBattleBag = 5;
static constexpr int kScreenBattleParty = 6;
static constexpr int kScreenOptions = 7;
static constexpr int kScreenTrainerCard = 8;
static constexpr int kScreenPokedex = 9;
static constexpr int kScreenPhone = 10;
static constexpr int kScreenGearMap = 11;
static constexpr int kScreenRadio = 12;
static constexpr int kScreenMainMenu = 13;
static constexpr int kScreenStarter = 14;
static constexpr int kScreenNaming = 15;
static constexpr int kScreenLook = 16;
static constexpr int kScreenShop = 17;
enum
{
    kScrType = 0,
    kScrCount = 1,
    kScrCursor = 2,
    kScrMenuOpen = 3,
    kScrMenuCount = 4,
    kScrMenuCursor = 5,
    kScrMessageKind = 6,
    kScrContext = 7,
    kScrFrame = 8,
    kScrMessage = 12,    // u16[96]
    kScrMenuItems = 204, // u16[8][16]
    kScrMons = 460,      // CoopUiPartyMon[6], 40 bytes each
    kScrOffset = 352,    // CoopUiScreen in CoopMailbox
    kTextLen = 96,
    kMenuItemLen = 16,
    kMonSize = 40,
};

// What the HD menus are doing, for a diagnostic report (emulator thread)
QString GameUi::DebugState()
{
    QMutexLocker locker(&mutex);
    QString entries;
    for (u8 e : state.entries)
        entries += QString::number(e) + " ";
    return QString("menu type %1 cursor %2 open %3 closing %4 entries [ %5]\n"
                   "screen type %6 lastFrame %7 stale %8  frame %9\n"
                   "chatOpen %10 namingActive %11 playersOpen %12 cemeteryOpen %13 giftOpen %14 playersAction %15\n"
                   "tradeRequest %16 tradeUi %17 closeMenuForTrade %18 pendingClick %19 prevPressed %20\n")
        .arg(state.type).arg(state.cursor).arg(state.open).arg(state.closing).arg(entries)
        .arg(screen.type).arg(screen.lastFrame).arg(screen.staleFrames).arg(frameCount)
        .arg(chatOpen).arg(namingActive).arg(playersOpen).arg(cemeteryOpen).arg(giftOpen).arg(playersAction)
        .arg(tradeRequest.pending).arg(tradeUi.open).arg(closeMenuForTrade).arg(pendingClick).arg(prevPressed, 0, 16);
}

GameUi& GameUi::Get()
{
    static GameUi instance;
    return instance;
}

std::vector<int> GameUi::visibleEntries() const
{
    // "Exit" is not shown: B closes the menu, as in recent Pokémon games
    std::vector<int> visible;
    for (size_t i = 0; i < state.entries.size(); i++)
    {
        if (state.entries[i] != kActionExit)
            visible.push_back((int)i);
    }
    return visible;
}

void GameUi::moveCursor(int dx, int dy)
{
    std::vector<int> visible = visibleEntries();
    if (visible.empty())
        return;

    int pos = 0;
    for (size_t i = 0; i < visible.size(); i++)
    {
        if (visible[i] == state.cursor)
            pos = (int)i;
    }
    int count = (int)visible.size();
    const int columns = MenuColumns(count);
    int rows = (count + columns - 1) / columns;
    int col = pos % columns, row = pos / columns;

    if (dx != 0)
    {
        col = (col + dx + columns) % columns;
    }
    if (dy != 0)
    {
        row = (row + dy + rows) % rows;
    }
    int newPos = row * columns + col;
    if (newPos >= count)
        newPos = dy != 0 ? col : count - 1; // wrapped onto an empty cell
    state.cursor = visible[newPos];
}

void GameUi::Update(NDS* nds, u8* mailbox)
{
    if (!mailbox)
        return;
    u8* ui = mailbox + CoopLink::kOffUiMenu;
    ui[kUiHostPresent] = 1;
    {
        // A click on an HD screen: the game reads it (and clears it)
        QMutexLocker clickLocker(&mutex);
        if (pendingClick != 0 && pendingClick < 100 && ui[7] == 0) // 100+: handled here (starter choice)
        {
            ui[7] = (u8)pendingClick;
            pendingClick = 0;
        }
    }

    u32 raw = nds->KeyInput;
    u32 pressed = (~raw & 0x3FF) | (((~raw >> 16) & 3) << 10);
    u32 newPressed = pressed & ~prevPressed;
    prevPressed = pressed;

    QMutexLocker locker(&mutex);

    updateScreen(mailbox);
    updateCover(mailbox);
    updateCoopStatus(mailbox);
    // Chat: old lines fade out; while typing, the game gets no buttons
    if (++chatFadeCheck >= 30)
    {
        chatFadeCheck = 0;
        if (!chatLines.isEmpty() && frameCount - chatLines.last().frame < kChatShowFrames + 40)
            dirty = true;
    }
    if (chatOpen)
    {
        nds->SetKeyMask(0xFFF);
        return;
    }
    // A trade starts: the X menu closes (the game runs the trade on the map)
    if (closeMenuForTrade)
    {
        closeMenuForTrade = false;
        playersOpen = false;
        giftOpen = false;
        cemeteryOpen = false;
        playersAction = -1;
        if (state.type != 0 && (s8)ui[kUiResult] == kUiResultWaiting)
            ui[kUiResult] = (u8)(s8)kUiResultCancel;
    }
    // A trade asked by a friend (expires after a minute and a half), then the trade screen
    if (tradeRequest.pending && !tradeUi.open)
    {
        nds->SetKeyMask(0xFFF);
        if (frameCount - tradeRequest.frame > 60 * 90 || (newPressed & kKeyB))
        {
            tradeActions.append({ TradeAction::Cancel, tradeRequest.slot, -1, GiftMon() });
            tradeRequest.pending = false;
            dirty = true;
        }
        else if (newPressed & kKeyA)
        {
            tradeOpenFor(tradeRequest.slot, false);
            tradeUi.theirs = tradeRequest.theirs;
            tradeUi.haveTheirs = true;
            tradeRequest.pending = false;
            dirty = true;
        }
        return;
    }
    if (tradeUi.open)
    {
        nds->SetKeyMask(0xFFF);
        tradeInput(newPressed);
        return;
    }
    // Offers expire after a minute and a half
    while (!giftOffers.isEmpty() && frameCount - giftOffers.first().frame > 60 * 90)
    {
        giftAnswers.append({ giftOffers.first().key, false });
        giftOffers.removeFirst();
        dirty = true;
    }
    if (!giftOffers.isEmpty())
    {
        nds->SetKeyMask(0xFFF);
        if (newPressed & (kKeyA | kKeyB))
        {
            giftAnswers.append({ giftOffers.first().key, (newPressed & kKeyA) != 0 });
            giftOffers.removeFirst();
            if (giftOffers.isEmpty() || true)
                dirty = true;
            if (giftOffers.size() > 0)
                giftOffers.first().frame = frameCount;
        }
        return;
    }
    updateNameplates(mailbox);
    updateInfo(mailbox);
    updateStarter(nds, mailbox, newPressed);
    updateNaming(nds, mailbox, newPressed);
    updateLook(nds, mailbox, newPressed);
    writeLook(mailbox);

    // HD Pokédex list: it is shown as a vertical list while the game uses a grid of
    // 5 columns, so Up/Down move by one Pokémon (game Left/Right) and Left/Right by
    // a row of 5 (game Up/Down)
    if (screen.type == kScreenPokedex && screen.raw[1] == 1)
    {
        const u32 kRight = 1 << 4, kLeft = 1 << 5, kUp = 1 << 6, kDown = 1 << 7;
        u32 keys = pressed & ~(kRight | kLeft | kUp | kDown);
        if (pressed & kUp) keys |= kLeft;
        if (pressed & kDown) keys |= kRight;
        if (pressed & kLeft) keys |= kUp;
        if (pressed & kRight) keys |= kDown;
        nds->SetKeyMask(~keys & 0xFFF);
    }

    // Menu opened (or reopened) by the game
    if (ui[kUiType] != 0 && (state.type == 0 || state.closing || ui[kUiRequestId] != requestId))
    {
        requestId = ui[kUiRequestId];
        state.type = ui[kUiType];
        state.entries.assign(ui + kUiEntries, ui + kUiEntries + std::min<int>(ui[kUiCount], kUiMaxEntries));
        if (coopMode)
            state.entries.push_back(kActionPlayers);
        if (nuzActive)
            state.entries.push_back(kActionCemetery);
        state.cursor = ui[kUiCursor] < state.entries.size() ? ui[kUiCursor] : 0;
        playersOpen = false;
        cemeteryOpen = false;
        state.money = ui[kUiMoney] | (ui[kUiMoney + 1] << 8) | (ui[kUiMoney + 2] << 16) | ((u32)ui[kUiMoney + 3] << 24);
        state.closing = false;
        if (state.entries[state.cursor] == kActionExit)
            moveCursor(0, 0);
        dirty = true;
    }
    else if (ui[kUiType] == 0 && state.type != 0 && !state.closing)
    {
        state.closing = true;
    }

    // Open/close animation
    if (state.type != 0)
    {
        float target = state.closing ? 0.0f : 1.0f;
        if (state.open != target)
        {
            state.open += state.closing ? -0.25f : 0.2f;
            state.open = std::max(0.0f, std::min(1.0f, state.open));
            dirty = true;
        }
        if (state.closing && state.open <= 0.0f)
        {
            state.type = 0;
            state.closing = false;
            dirty = true;
        }
    }

    if (state.type == 0 || ui[kUiType] == 0)
    {
        playersOpen = false;
        cemeteryOpen = false;
        giftOpen = false;
        playersAction = -1;
        return;
    }

    // The menu has the controls: the game doesn't see the buttons
    nds->SetKeyMask(0xFFF);
    // An answer the game never takes (its menu is gone without saying so): after 2 s the menu
    // closes and the buttons go back to the game, rather than a game stuck behind it
    if ((s8)ui[kUiResult] != kUiResultWaiting && !state.closing)
    {
        if (++unansweredFrames > 120)
        {
            unansweredFrames = 0;
            ui[kUiType] = 0;
            ui[kUiResult] = (u8)(s8)kUiResultWaiting;
            state.closing = true;
            dirty = true;
        }
        return;
    }
    unansweredFrames = 0;
    if (state.closing || (s8)ui[kUiResult] != kUiResultWaiting)
        return;

    // The cemetery, opened from the menu
    if (cemeteryOpen)
    {
        int count = (int)cemetery.size();
        if (newPressed & (kKeyB | kKeyX | kKeyA))
        {
            cemeteryOpen = false;
            dirty = true;
        }
        else if ((newPressed & (kKeyUp | kKeyDown)) && count > 0)
        {
            cemeteryCursor = (cemeteryCursor + ((newPressed & kKeyDown) ? 1 : count - 1)) % count;
            dirty = true;
        }
        return;
    }

    // The gift screen, opened from the players screen
    if (giftOpen)
    {
        if (giftInput(newPressed))
            dirty = true;
        return;
    }

    // The online players screen, opened from the menu
    if (playersOpen)
    {
        if (playersAction >= 0)
        {
            // Write or give a gift to the friend chosen
            if (newPressed & kKeyRight)
                playersAction = (playersAction + 1) % 3;
            else if (newPressed & kKeyLeft)
                playersAction = (playersAction + 2) % 3;
            else if ((newPressed & kKeyA) && playersCursor > 0 && playersCursor <= (int)coopPlayers.size())
            {
                int slot = coopPlayers[playersCursor - 1].slot;
                if (playersAction == 0)
                {
                    playersOpen = false;
                    chatOpen = true;
                    chatInput.clear();
                    chatTarget = slot;
                }
                else if (playersAction == 1)
                    giftOpenFor(slot);
                else
                    tradeOpenFor(slot, true);
                playersAction = -1;
            }
            else if (newPressed & (kKeyB | kKeyX))
                playersAction = -1;
            dirty = true;
            return;
        }
        if ((newPressed & kKeyA) && playersCursor > 0 && playersCursor <= (int)coopPlayers.size())
        {
            playersAction = 0;
            dirty = true;
        }
        else if (newPressed & (kKeyB | kKeyX | kKeyA))
        {
            playersOpen = false;
            dirty = true;
        }
        else if (newPressed & (kKeyUp | kKeyDown))
        {
            int count = (int)coopPlayers.size() + 1;
            playersCursor = (playersCursor + ((newPressed & kKeyDown) ? 1 : count - 1)) % count;
            dirty = true;
        }
        return;
    }

    // Directions, with key repeat
    int dirKey = 0;
    if (newPressed & kKeyDirections)
    {
        dirKey = newPressed & kKeyDirections;
        repeatKey = dirKey;
        repeatTimer = 18;
    }
    else if (repeatKey && (pressed & repeatKey))
    {
        if (--repeatTimer <= 0)
        {
            dirKey = repeatKey;
            repeatTimer = 5;
        }
    }
    else
    {
        repeatKey = 0;
    }

    int oldCursor = state.cursor;
    if (dirKey & kKeyUp) moveCursor(0, -1);
    else if (dirKey & kKeyDown) moveCursor(0, 1);
    else if (dirKey & kKeyLeft) moveCursor(-1, 0);
    else if (dirKey & kKeyRight) moveCursor(1, 0);
    if (state.cursor != oldCursor)
    {
        if (state.entries[state.cursor] < kActionPlayers) // the game doesn't know the launcher's entries
            ui[kUiCursor] = (u8)state.cursor; // the game plays the cursor sound
        dirty = true;
    }

    if ((newPressed & kKeyA) && state.entries[state.cursor] == kActionCemetery)
    {
        // The graves, the last ones first (cemetery.json, written by CoopLink next to the save)
        cemetery.clear();
        QFile file(QString::fromUtf8(getenv("HGCOOP_SAVE_DIR") ? getenv("HGCOOP_SAVE_DIR") : "") + "/cemetery.json");
        if (file.open(QIODevice::ReadOnly))
        {
            QJsonArray graves = QJsonDocument::fromJson(file.readAll()).array();
            for (int i = graves.size() - 1; i >= 0; i--)
                cemetery.append(graves[i].toObject());
        }
        cemeteryOpen = true;
        cemeteryCursor = 0;
        dirty = true;
    }
    else if ((newPressed & kKeyA) && state.entries[state.cursor] == kActionPlayers)
    {
        playersOpen = true;
        playersCursor = 0;
        dirty = true;
    }
    else if (newPressed & kKeyA)
    {
        ui[kUiResult] = (u8)state.cursor;
        // An app opens (Pokédex, party, bag, card, options, Pokégear): hide its fade-in
        int action = state.entries[state.cursor];
        if (action != kActionSave && action != kActionExit && action != kActionRetire)
            coverArmed = 180;
    }
    else if (newPressed & (kKeyB | kKeyX))
        ui[kUiResult] = (u8)(s8)kUiResultCancel;
}

// Screens shown outside of battles (their game apps fade in and out with the old graphics)
static bool IsFieldAppScreen(int type)
{
    return type != 0 && type != kScreenBattle && type != kScreenBattleBag && type != kScreenBattleParty;
}

void GameUi::updateCover(u8* mailbox)
{
    // The map runs again: no more cover
    u32 fieldFrame = mailbox[CoopLink::kOffFieldFrame] | (mailbox[CoopLink::kOffFieldFrame + 1] << 8)
        | (mailbox[CoopLink::kOffFieldFrame + 2] << 16) | ((u32)mailbox[CoopLink::kOffFieldFrame + 3] << 24);
    // The map logic runs every other frame: it stopped after a few frames without a tick
    fieldIdleFrames = (started && fieldFrame != lastFieldFrame) ? 0 : fieldIdleFrames + 1;
    lastFieldFrame = fieldFrame;
    bool fieldRunning = fieldIdleFrames < 4;

    // Boot: hide the game's own main menu until the HD one is there
    if (!started)
    {
        started = true;
        coverFrames = 420;
        coverAlpha = 1.0f;
    }

    // An HD screen goes away (back to the map, or another app comes): cover the gap
    if (IsFieldAppScreen(lastScreenType) && screen.type == 0)
        coverFrames = 120;
    lastScreenType = screen.type;

    // An app was chosen in the menu: the map fades out first, then stops
    if (coverArmed > 0)
    {
        coverArmed--;
        if (!fieldRunning)
        {
            coverArmed = 0;
            coverFrames = 120;
        }
    }

    if (fieldRunning || screen.type != 0)
        coverFrames = 0;
    else if (coverFrames > 0)
        coverFrames--;

    float target = (coverFrames > 0 && state.type == 0) ? 1.0f : 0.0f;
    if (coverAlpha != target)
    {
        coverAlpha = target > coverAlpha ? std::min(1.0f, coverAlpha + 0.25f) : std::max(0.0f, coverAlpha - 0.15f);
        dirty = true;
    }
}

QColor GameUi::PlayerColor(int slot)
{
    static const QColor colors[6] = { QColor(243, 195, 90), QColor(84, 156, 236), QColor(95, 211, 138),
                                      QColor(236, 112, 160), QColor(168, 128, 236), QColor(240, 140, 60) };
    return slot >= 0 && slot < 6 ? colors[slot] : QColor(200, 200, 200);
}

void GameUi::SetLocalName(const QString& name)
{
    QMutexLocker locker(&mutex);
    localName = name;
    dirty = true;
}

// The roster changed (CoopLink, emulator thread): notifications for who came and went
void GameUi::SetCoopPlayers(const std::vector<CoopPlayer>& players, int mySlot)
{
    QMutexLocker locker(&mutex);
    QStringList before, after;
    for (const CoopPlayer& p : coopPlayers)
        before << p.name;
    for (const CoopPlayer& p : players)
        after << p.name;
    if (mySlot >= 0 && !rosterSeen)
    {
        // Just arrived on the server: who is already there
        rosterSeen = true;
        if (!after.isEmpty())
            pushToast(after.size() == 1 ? QString::fromUtf8("%1 est en ligne").arg(after[0])
                                        : QString::fromUtf8("En ligne : %1").arg(after.join(", ")), PlayerColor(players[0].slot));
    }
    else if (mySlot >= 0)
    {
        for (const CoopPlayer& p : players)
            if (!before.contains(p.name))
            {
                pushToast(QString::fromUtf8("%1 vient de se connecter").arg(p.name), PlayerColor(p.slot));
                arrivedAt[p.name] = frameCount;
            }
        for (const CoopPlayer& p : coopPlayers)
            if (!after.contains(p.name))
                pushToast(QString::fromUtf8("%1 s'est déconnecté").arg(p.name), QColor(200, 70, 70));
    }
    if (mySlot < 0)
        rosterSeen = false;
    coopPlayers = players;
    coopMySlot = mySlot;
    dirty = true;
}

void GameUi::pushToast(const QString& text, const QColor& color)
{
    toasts.append({ text, color });
    while (toasts.size() > 4)
        toasts.removeFirst();
}

void GameUi::updateCoopStatus(u8* mailbox)
{
    frameCount++;
    if (!started)
        return;
    // Toasts, one after the other (4 seconds each)
    if (toastFrames > 0)
    {
        toastFrames--;
        if (toastFrames == 0 || toastFrames % 4 == 0)
            dirty = true;
    }
    else if (!toasts.isEmpty())
    {
        toastText = toasts.first().first;
        toastColor = toasts.first().second;
        toasts.removeFirst();
        toastFrames = 240;
        dirty = true;
    }
    // Nuzlocke: the current wild battle's catch rule (badge on the battle screen)
    nuzActive = (mailbox[CoopLink::kOffNuzState - 8 + 4] & 1) != 0; // CoopNuzlocke.rules
    int catchState = nuzActive ? mailbox[CoopLink::kOffNuzState + CoopLink::kNuzBattleCatch] : 0;
    if (catchState != nuzBattleCatch)
    {
        nuzBattleCatch = catchState;
        dirty = true;
    }
    coopMode = getenv("HGCOOP_MODE") != nullptr;
    if (!coopMode)
        return;
    if (networkStatus != lastNetworkStatus)
    {
        lastNetworkStatus = networkStatus;
        dirty = true;
    }
    // A friend's character appears on our map: "X est ici !"
    for (int i = 0; i < kMaxRemotePlayers; i++)
    {
        bool visible = mailbox[CoopLink::kOffRemotes + i * CoopLink::kRemoteSize + CoopLink::kRemGhostVisible] != 0;
        if (visible && !ghostSeen[i])
        {
            for (const CoopPlayer& p : coopPlayers)
                if (p.remote == i && frameCount - arrivedAt.value(p.name, -1000) > 300)
                    pushToast(QString::fromUtf8("%1 est ici !").arg(p.name), PlayerColor(p.slot));
        }
        ghostSeen[i] = visible;
    }
    if (playersOpen && frameCount % 16 == 0)
        dirty = true; // the team icons move
}

// Coop status: a pill while connecting or alone on the server, toasts when players come or go
static void DrawButtonHint(QPainter& p, const QPointF& pos, qreal size, const QString& button, const QString& text, const QColor& color,
                           const QColor& textColor = QColor(60, 66, 80));

// ---------------------------------------------------------------------------
// The player's look: Ethan or Lyra, and the colors of five zones of their sprites
// ---------------------------------------------------------------------------

namespace {
enum LookKind { kLookClothes, kLookHair, kLookSkin };
struct LookZone { const char* name; LookKind kind; std::vector<u16> colors; };
struct LookPreset { const char* name; int r, g, b; };

// The colors (RGB555) of each part of the map sprites. Each part has its own colors in the
// sprites (tools/coop/split_hero_colors.py, look_zones.json): changing one never changes another.
// The first five keep the meaning they had in saved looks.
const std::vector<LookZone>& LookZones(int gender)
{
    static const std::vector<LookZone> ethan {
        { "Casquette", kLookClothes, { 0x11D7, 0x12DF } },
        { "Cheveux", kLookHair, { 0x1CA5, 0x2107, 0x3189 } },
        { "Veste", kLookClothes, { 0x1CF8, 0x25BE } },
        { "Sac", kLookClothes, { 0x5653, 0x5A32, 0x66D7, 0x7FBE } },
        { "Peau", kLookSkin, { 0x3E9B, 0x5F5F } },
        { "Short", kLookClothes, { 0x20A5, 0x2507, 0x3589 } },
        { "Chaussures", kLookClothes, { 0x5632, 0x5E53, 0x6ED7, 0x7BBD } },
        { "T-shirt", kLookClothes, { 0x18A5, 0x1D07, 0x2D89 } },
    };
    static const std::vector<LookZone> lyra {
        { "Chapeau", kLookClothes, { 0x4987, 0x6A95, 0x7339, 0x7BBD } },
        { "Ruban", kLookClothes, { 0x18F9, 0x2DBF } },
        { "Cheveux", kLookHair, { 0x148B, 0x250F } },
        { "Sac", kLookClothes, { 0x1E79, 0x2F7F } },
        { "Peau", kLookSkin, { 0x3E9B, 0x5F5F } },
        { "Haut", kLookClothes, { 0x20F9, 0x35BF } },
        { "Salopette", kLookClothes, { 0x2907, 0x4187 } },
        { "Chaussures", kLookClothes, { 0x1CF9, 0x31BF } },
    };
    return gender == 1 ? lyra : ethan;
}

// Preset 0 is always the original color
const std::vector<LookPreset>& LookPresets(LookKind kind)
{
    static const std::vector<LookPreset> clothes {
        { "Origine", 0, 0, 0 }, { "Rouge", 230, 60, 60 }, { "Orange", 245, 140, 40 }, { "Jaune", 245, 210, 50 },
        { "Vert", 80, 190, 80 }, { "Turquoise", 60, 190, 180 }, { "Bleu", 60, 120, 230 }, { "Marine", 55, 65, 150 },
        { "Violet", 150, 80, 210 }, { "Rose", 240, 120, 180 }, { "Blanc", 240, 240, 245 }, { "Gris", 150, 150, 160 },
        { "Noir", 65, 65, 75 },
    };
    static const std::vector<LookPreset> hair {
        { "Origine", 0, 0, 0 }, { "Noir", 50, 50, 60 }, { "Brun", 110, 70, 40 }, { "Châtain", 150, 100, 60 },
        { "Roux", 200, 90, 40 }, { "Blond", 240, 210, 110 }, { "Gris", 170, 170, 180 }, { "Bleu", 70, 110, 200 },
        { "Vert", 70, 150, 90 }, { "Rose", 230, 120, 170 }, { "Violet", 130, 80, 180 },
    };
    static const std::vector<LookPreset> skin {
        { "Origine", 0, 0, 0 }, { "Claire", 250, 215, 190 }, { "Moyenne", 225, 170, 125 }, { "Mate", 190, 130, 90 },
        { "Foncée", 140, 90, 60 }, { "Très foncée", 95, 60, 40 },
    };
    return kind == kLookHair ? hair : kind == kLookSkin ? skin : clothes;
}

QColor From555(u16 c)
{
    return QColor((c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31);
}

u16 To555(const QColor& c)
{
    return (u16)(((c.red() * 31 + 127) / 255) | (((c.green() * 31 + 127) / 255) << 5) | (((c.blue() * 31 + 127) / 255) << 10));
}
} // namespace

// The new colors of a look: each zone keeps its shading (lightness relative to its lightest
// color) with the preset's hue and saturation
std::vector<std::pair<u16, u16>> GameUi::lookPairs(const Look& l) const
{
    std::vector<std::pair<u16, u16>> pairs;
    int gender = l.gender == 1 ? 1 : 0;
    const auto& zones = LookZones(gender);
    for (int z = 0; z < kLookZones && z < (int)zones.size(); z++)
    {
        const auto& presets = LookPresets(zones[z].kind);
        int choice = std::clamp(l.zone[z], 0, (int)presets.size() - 1);
        if (choice == 0)
            continue;
        QColor target(presets[choice].r, presets[choice].g, presets[choice].b);
        float lightest = 0.01f;
        for (u16 original : zones[z].colors)
            lightest = std::max(lightest, (float)From555(original).lightnessF());
        for (u16 original : zones[z].colors)
        {
            float ratio = From555(original).lightnessF() / lightest;
            QColor c = QColor::fromHslF(target.hslHueF() < 0 ? 0 : target.hslHueF(), target.hslSaturationF(),
                                        std::clamp(target.lightnessF() * ratio, 0.0f, 1.0f));
            pairs.push_back({ original, To555(c) });
        }
    }
    return pairs;
}

void GameUi::SetLook(const QString& text)
{
    QMutexLocker locker(&mutex);
    Look l;
    QStringList parts = text.split(':');
    if (parts.size() == 2)
    {
        l.gender = parts[0].toInt() == 1 ? 1 : 0;
        QStringList zones = parts[1].split('.');
        for (int z = 0; z < kLookZones && z < zones.size(); z++)
            l.zone[z] = std::max(0, zones[z].toInt());
    }
    look = l;
    lookDirty = true;
}

std::vector<std::pair<u16, u16>> GameUi::LookPairsFor(const QString& text)
{
    Look l;
    QStringList parts = text.split(':');
    if (parts.size() != 2)
        return {};
    l.gender = parts[0].toInt() == 1 ? 1 : 0;
    QStringList zones = parts[1].split('.');
    for (int z = 0; z < kLookZones && z < zones.size(); z++)
        l.zone[z] = std::max(0, zones[z].toInt());
    return lookPairs(l);
}

QString GameUi::LookString()
{
    QStringList zones;
    for (int z = 0; z < kLookZones; z++)
        zones << QString::number(look.zone[z]);
    return QString("%1:%2").arg(look.gender).arg(zones.join('.'));
}

// The game swaps these colors when it loads the player's sprites (CoopLook)
void GameUi::writeLook(u8* mailbox)
{
    if (!lookDirty && frameCount % 60 != 0)
        return; // written again now and then (the game may have cleared its memory)
    lookDirty = false;
    u8* l = mailbox + CoopLink::kOffLook;
    auto pairs = lookPairs(look);
    l[0] = (u8)look.gender;
    WriteLookPairs(l, pairs);
}

// count, from[32], to[32] of a CoopLook / CoopGhostLook (byte 0 is left to the caller)
void GameUi::WriteLookPairs(u8* l, const std::vector<std::pair<u16, u16>>& pairs)
{
    const int n = CoopLink::kLookColors;
    l[1] = (u8)std::min<size_t>(pairs.size(), n);
    for (int i = 0; i < n; i++)
    {
        u16 from = i < (int)pairs.size() ? pairs[i].first : 0xFFFF, to = i < (int)pairs.size() ? pairs[i].second : 0;
        l[4 + i * 2] = from & 0xFF;
        l[5 + i * 2] = from >> 8;
        l[4 + n * 2 + i * 2] = to & 0xFF;
        l[5 + n * 2 + i * 2] = to >> 8;
    }
}

static constexpr int kLkMode = 1, kLkDone = 2, kLkGender = 3;

void GameUi::updateLook(NDS* nds, u8* mailbox, u32 newPressed)
{
    if (screen.type != kScreenLook)
    {
        lookActive = false;
        return;
    }
    u8* scr = mailbox + kScrOffset;
    if (!lookActive)
    {
        lookActive = true;
        lookEdit = look;
        if (scr[kLkMode] != 0)
            lookEdit.gender = scr[kLkGender] == 1 ? 1 : 0; // the save's gender
        lookRow = 0;
        lookFrame = 0;
    }
    nds->SetKeyMask(0xFFF);
    lookFrame++;
    if (lookFrame % 4 == 0)
        dirty = true; // the preview turns
    if (scr[kLkDone] || !newPressed)
        return;
    dirty = true;
    const int zoneCount = (int)LookZones(lookEdit.gender).size();
    const int rows = 1 + zoneCount + 1; // Ethan / Lyra, the zones, OK
    if (newPressed & kKeyUp)
        lookRow = (lookRow + rows - 1) % rows;
    if (newPressed & kKeyDown)
        lookRow = (lookRow + 1) % rows;
    int step = (newPressed & kKeyRight) ? 1 : (newPressed & kKeyLeft) ? -1 : 0;
    if (step && lookRow == 0)
    {
        lookEdit.gender = 1 - lookEdit.gender;
        for (int z = 0; z < kLookZones; z++)
            lookEdit.zone[z] = 0;
    }
    else if (step && lookRow <= zoneCount)
    {
        int count = (int)LookPresets(LookZones(lookEdit.gender)[lookRow - 1].kind).size();
        lookEdit.zone[lookRow - 1] = (lookEdit.zone[lookRow - 1] + step + count) % count;
    }
    if ((newPressed & kKeyA) && lookRow == rows - 1)
    {
        look = lookEdit;
        lookDirty = true;
        writeLook(mailbox);
        scr[kLkDone] = 1;
    }
    else if ((newPressed & kKeyB) && scr[kLkMode] != 0)
        scr[kLkDone] = 2; // mirror: left as it was
}

void GameUi::drawLook(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(60, 40, 110));
    bg.setColorAt(1, QColor(20, 16, 50));
    p.fillRect(QRectF(0, 0, width, height), bg);
    QFont font("Segoe UI");
    font.setBold(true);
    font.setPixelSize((int)(48 * u));
    p.setFont(font);
    p.setPen(Qt::white);
    bool newGame = screen.raw[kLkMode] == 0;
    p.drawText(QRectF(0, 30 * u, width, 70 * u), Qt::AlignCenter, newGame ? QString::fromUtf8("Ton apparence") : QString::fromUtf8("Changer de style"));

    // The preview: the sprite in its new colors, turning
    int gender = lookEdit.gender == 1 ? 1 : 0;
    if (lookSheet[gender].isNull())
        lookSheet[gender] = QImage(HoLympeAssetsDir() + (gender ? "/look/heroine.png" : "/look/hero.png"));
    QRectF stage(width / 2.0 - 760 * u, 140 * u, 640 * u, 760 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 30));
    p.drawRoundedRect(stage, 40 * u, 40 * u);
    if (!lookSheet[gender].isNull() && lookSheet[gender].format() == QImage::Format_Indexed8)
    {
        QImage sheet = lookSheet[gender];
        auto pairs = lookPairs(lookEdit);
        for (int i = 1; i < sheet.colorCount(); i++)
        {
            u16 c = To555(QColor::fromRgb(lookSheet[gender].color(i)));
            for (const auto& pr : pairs)
                if (pr.first == c)
                    c = pr.second;
            sheet.setColor(i, From555(c).rgb());
        }
        sheet.setColor(0, qRgba(0, 0, 0, 0));
        static const int kTurn[4] = { 21, 1, 0, 1 }; // front, left, back, right (left mirrored)
        int dir = (lookFrame / 50) % 4;
        QImage frame = sheet.copy(0, kTurn[dir] * 32, 32, 32).convertToFormat(QImage::Format_ARGB32);
        if (dir == 3)
            frame = frame.mirrored(true, false);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // a shadow under the feet
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawEllipse(QPointF(stage.center().x(), stage.top() + 620 * u), 150 * u, 40 * u);
        p.drawImage(QRectF(stage.center().x() - 256 * u, stage.top() + 120 * u, 512 * u, 512 * u), frame);
    }

    // The choices
    const auto& zones = LookZones(gender);
    QRectF panel(width / 2.0 - 60 * u, 140 * u, 820 * u, 760 * u);
    p.setBrush(QColor(250, 250, 255));
    p.drawRoundedRect(panel, 40 * u, 40 * u);
    const int zoneCount = (int)zones.size();
    const qreal rowH = (panel.height() - 60 * u) / (zoneCount + 2) - 8 * u;
    for (int r = 0; r < 1 + zoneCount + 1; r++)
    {
        QRectF row(panel.left() + 30 * u, panel.top() + 30 * u + r * (rowH + 8 * u), panel.width() - 60 * u, rowH);
        bool selected = r == lookRow;
        bool ok = r == zoneCount + 1;
        p.setPen(selected ? QPen(QColor(255, 196, 60), 5 * u) : Qt::NoPen);
        p.setBrush(ok ? (selected ? QColor(76, 176, 120) : QColor(120, 200, 150)) : selected ? QColor(255, 248, 230) : QColor(238, 238, 248));
        p.drawRoundedRect(row, 26 * u, 26 * u);
        font.setPixelSize((int)(30 * u));
        font.setBold(true);
        p.setFont(font);
        if (ok)
        {
            p.setPen(Qt::white);
            p.drawText(row, Qt::AlignCenter, newGame ? QString::fromUtf8("C'est parti !") : QString::fromUtf8("Valider"));
            continue;
        }
        QString label, value;
        QColor swatch;
        bool hasSwatch = false;
        if (r == 0)
        {
            label = QString::fromUtf8("Personnage");
            value = gender ? QString::fromUtf8("Lyra") : QString::fromUtf8("Ethan");
        }
        else
        {
            const LookZone& zone = zones[r - 1];
            const auto& presets = LookPresets(zone.kind);
            int choice = std::clamp(lookEdit.zone[r - 1], 0, (int)presets.size() - 1);
            label = QString::fromUtf8(zone.name);
            value = QString::fromUtf8(presets[choice].name);
            swatch = choice == 0 ? From555(zone.colors.back()) : QColor(presets[choice].r, presets[choice].g, presets[choice].b);
            hasSwatch = true;
        }
        p.setPen(QColor(50, 50, 70));
        p.drawText(QRectF(row.left() + 30 * u, row.top(), row.width() * 0.5, row.height()), Qt::AlignVCenter | Qt::AlignLeft, label);
        font.setBold(false);
        p.setFont(font);
        QRectF valueRect(row.right() - 340 * u, row.top(), 310 * u, row.height());
        p.drawText(valueRect, Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("◀  %1  ▶").arg(value));
        if (hasSwatch)
        {
            p.setPen(QPen(QColor(0, 0, 0, 60), 2 * u));
            p.setBrush(swatch);
            p.drawEllipse(QPointF(row.right() - 380 * u, row.center().y()), 22 * u, 22 * u);
        }
    }
    font.setPixelSize((int)(26 * u));
    font.setBold(false);
    p.setFont(font);
    p.setPen(QColor(220, 215, 240));
    p.drawText(QRectF(0, height - 120 * u, width, 50 * u), Qt::AlignCenter,
               newGame ? QString::fromUtf8("Haut / Bas : choisir   ·   Gauche / Droite : changer   ·   A sur « C'est parti ! »")
                       : QString::fromUtf8("Haut / Bas : choisir   ·   Gauche / Droite : changer   ·   A sur « Valider »   ·   B : annuler"));
    p.restore();
}

// ---------------------------------------------------------------------------
// Naming screen: the name is typed with the PC keyboard in an HD box
// ---------------------------------------------------------------------------

static constexpr int kNmKind = 1, kNmMaxLen = 2, kNmDone = 3, kNmSpecies = 4, kNmGender = 6,
                     kNmPrompt = 12, kNmCurrent = 92, kNmResult = 132;

bool GameUi::namingKey(QKeyEvent* event)
{
    int maxLen = std::clamp<int>(screen.raw[kNmMaxLen], 1, 19);
    switch (event->key())
    {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        namingSubmit = true;
        break;
    case Qt::Key_Escape:
        namingInput.clear(); // keep the usual name
        namingSubmit = true;
        break;
    case Qt::Key_Backspace:
        namingInput.chop(1);
        break;
    default:
        for (QChar c : event->text())
        {
            u16 code[2];
            if (c.isPrint() && namingInput.size() < maxLen && EncodeGameText(QString(c), code, 1))
                namingInput.append(c);
        }
        break;
    }
    dirty = true;
    return true; // every key is for the box while it is open
}

void GameUi::updateNaming(NDS* nds, u8* mailbox, u32 newPressed)
{
    if (screen.type != kScreenNaming)
    {
        namingActive = false;
        return;
    }
    u8* scr = mailbox + kScrOffset;
    if (!namingActive)
    {
        namingActive = true;
        namingInput = namingPending;
        namingPending.clear();
        namingSubmit = false;
        dirty = true;
    }
    nds->SetKeyMask(0xFFF); // the box has the controls
    // With a controller: A confirms what was typed, B deletes a letter
    if (newPressed & kKeyA)
        namingSubmit = true;
    else if (newPressed & kKeyB)
    {
        namingInput.chop(1);
        dirty = true;
    }
    if ((screen.animFrame % 30) == 0)
        dirty = true; // the caret blinks
    if (!namingSubmit || scr[kNmDone])
        return;
    u16 result[20];
    int len = 0;
    QString text = namingInput.trimmed();
    for (QChar c : text)
        if (len < 19 && EncodeGameText(QString(c), result + len, 1))
            len++;
    result[len] = 0xFFFF;
    memcpy(scr + kNmResult, result, (len + 1) * 2);
    scr[kNmDone] = 1;
    namingSubmit = false;
}

void GameUi::drawNaming(QPainter& p, int width, int height)
{
    const u8* scr = screen.raw;
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(40, 70, 130));
    bg.setColorAt(1, QColor(14, 22, 52));
    p.fillRect(QRectF(0, 0, width, height), bg);

    const int kind = scr[kNmKind];
    const int maxLen = std::clamp<int>(scr[kNmMaxLen], 1, 19);
    const int species = scr[kNmSpecies] | (scr[kNmSpecies + 1] << 8);
    QRectF card((width - 1100 * u) / 2, (height - 640 * u) / 2, 1100 * u, 640 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(card.translated(0, 12 * u), 44 * u, 44 * u);
    p.setBrush(QColor(248, 250, 255));
    p.drawRoundedRect(card, 44 * u, 44 * u);

    // The Pokemon being named
    qreal top = card.top() + 40 * u;
    if (kind == 1 && species > 0)
    {
        const QImage& sprite = pokemonSprite(species);
        if (!sprite.isNull())
        {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            // The sprite sheet has its two frames side by side
            QImage frame = sprite.width() >= 2 * sprite.height() ? sprite.copy(0, 0, sprite.width() / 2, sprite.height()) : sprite;
            p.drawImage(QRectF(card.center().x() - 110 * u, top - 10 * u, 220 * u, 220 * u), frame);
            p.restore();
        }
        top += 210 * u;
    }
    QFont font("Segoe UI");
    font.setPixelSize((int)(38 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(32, 40, 56));
    QString prompt = DecodeGameText((const u16*)(scr + kNmPrompt), 40);
    p.drawText(QRectF(card.left() + 40 * u, top, card.width() - 80 * u, 60 * u), Qt::AlignCenter, prompt);

    // The text box
    QRectF box(card.left() + 120 * u, top + 90 * u, card.width() - 240 * u, 100 * u);
    p.setPen(QPen(QColor(240, 186, 40), 5 * u));
    p.setBrush(Qt::white);
    p.drawRoundedRect(box, 30 * u, 30 * u);
    font.setPixelSize((int)(52 * u));
    p.setFont(font);
    bool caret = (screen.animFrame / 30) % 2 == 0;
    if (namingInput.isEmpty())
    {
        QString current = DecodeGameText((const u16*)(scr + kNmCurrent), 20);
        QString placeholder = current.isEmpty() ? QString::fromUtf8("Écris un nom…") : current;
        p.setPen(QColor(170, 178, 192));
        p.drawText(box, Qt::AlignCenter, placeholder);
        if (caret)
        {
            // The caret waits just before the grey text
            p.setPen(QPen(QColor(40, 48, 66), 4 * u));
            qreal x = box.center().x() - QFontMetricsF(font).horizontalAdvance(placeholder) / 2 - 12 * u;
            p.drawLine(QPointF(x, box.top() + 24 * u), QPointF(x, box.bottom() - 24 * u));
        }
    }
    else
        {
        p.setPen(QColor(32, 40, 56));
        p.drawText(box, Qt::AlignCenter, namingInput + (caret ? "|" : " "));
    }
    font.setPixelSize((int)(26 * u));
    font.setBold(false);
    p.setFont(font);
    p.setPen(QColor(120, 130, 150));
    p.drawText(QRectF(box.left(), box.bottom() + 10 * u, box.width(), 36 * u), Qt::AlignRight | Qt::AlignVCenter,
               QString("%1 / %2").arg(namingInput.size()).arg(maxLen));

    // Help
    font.setPixelSize((int)(28 * u));
    p.setFont(font);
    p.setPen(QColor(80, 90, 110));
    p.drawText(QRectF(card.left(), card.bottom() - 90 * u, card.width(), 40 * u), Qt::AlignCenter,
               QString::fromUtf8("Tape le nom au clavier   ·   Entrée : valider   ·   Échap : garder le nom d'origine"));
    p.restore();
}

// ---------------------------------------------------------------------------
// Trades: each player sees both Pokemon, both confirm, the games play the trade
// ---------------------------------------------------------------------------

void GameUi::tradeOpenFor(int slot, bool initiator)
{
    tradeUi = TradeUi();
    tradeUi.open = true;
    tradeUi.slot = slot;
    tradeUi.initiator = initiator;
    tradeUi.picking = true;
    for (const CoopPlayer& p : coopPlayers)
        if (p.slot == slot)
            tradeUi.partner = p.name;
    giftHaveCatalog = false;
    giftCatalogAsked = true; // our team, with the details
}

bool GameUi::TakeTradeAction(TradeAction& action)
{
    QMutexLocker locker(&mutex);
    if (tradeActions.isEmpty())
        return false;
    action = tradeActions.takeFirst();
    return true;
}

void GameUi::TradeRequested(int slot, const QString& from, const GiftMon& theirs)
{
    QMutexLocker locker(&mutex);
    tradeRequest = { true, slot, from, theirs, frameCount };
    dirty = true;
}

void GameUi::TradeOffered(const GiftMon& theirs)
{
    QMutexLocker locker(&mutex);
    if (!tradeUi.open)
        return;
    tradeUi.theirs = theirs;
    tradeUi.haveTheirs = true;
    dirty = true;
}

void GameUi::TradeTheyConfirmed()
{
    QMutexLocker locker(&mutex);
    tradeUi.theirConfirm = true;
    dirty = true;
}

void GameUi::TradeCancelled(const QString& why)
{
    QMutexLocker locker(&mutex);
    tradeUi = TradeUi();
    tradeRequest.pending = false;
    pushToast(why, QColor(239, 106, 90));
    dirty = true;
}

void GameUi::TradeExecuting()
{
    QMutexLocker locker(&mutex);
    tradeUi = TradeUi();
    closeMenuForTrade = true; // the game plays the trade on the map
    dirty = true;
}

// The trade screen's controls (mutex held)
void GameUi::tradeInput(u32 newPressed)
{
    if (!newPressed)
        return;
    dirty = true;
    if (newPressed & (kKeyB | kKeyX))
    {
        tradeActions.append({ TradeAction::Cancel, tradeUi.slot, -1, GiftMon() });
        tradeUi = TradeUi();
        return;
    }
    if (tradeUi.picking)
    {
        int count = giftMons.size();
        if (!giftHaveCatalog || count == 0)
            return;
        if (newPressed & kKeyUp)
            tradeUi.cursor = (tradeUi.cursor + count - 1) % count;
        if (newPressed & kKeyDown)
            tradeUi.cursor = (tradeUi.cursor + 1) % count;
        if (newPressed & kKeyA)
        {
            tradeUi.mine = giftMons[tradeUi.cursor];
            tradeUi.haveMine = true;
            tradeUi.picking = false;
            TradeAction a { tradeUi.initiator ? TradeAction::Request : TradeAction::Offer, tradeUi.slot, tradeUi.cursor, tradeUi.mine };
            tradeActions.append(a);
        }
        return;
    }
    if ((newPressed & kKeyA) && tradeUi.haveMine && tradeUi.haveTheirs && !tradeUi.myConfirm)
    {
        tradeUi.myConfirm = true;
        tradeActions.append({ TradeAction::Confirm, tradeUi.slot, -1, GiftMon() });
    }
}

void GameUi::drawTradeCard(QPainter& p, const QRectF& card, qreal u, const QString& title, const GiftMon* mon, const QString& waiting,
                           bool confirmed, bool picking)
{
    p.save();
    p.setPen(confirmed ? QPen(QColor(76, 176, 120), 5 * u) : Qt::NoPen);
    p.setBrush(QColor(250, 252, 255));
    p.drawRoundedRect(card, 36 * u, 36 * u);
    QFont font("Segoe UI");
    font.setPixelSize((int)(26 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(120, 128, 150));
    p.drawText(QRectF(card.left() + 36 * u, card.top() + 20 * u, card.width() - 72 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft, title);
    if (confirmed)
    {
        p.setPen(QColor(60, 160, 100));
        p.drawText(QRectF(card.left() + 36 * u, card.top() + 20 * u, card.width() - 72 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight,
                   QString::fromUtf8("✓ Confirmé"));
    }

    if (picking)
    {
        // Our team: which one goes?
        if (!giftHaveCatalog)
        {
            p.setPen(QColor(140, 150, 170));
            p.drawText(card, Qt::AlignCenter, QString::fromUtf8("Ouverture de l'équipe…"));
            p.restore();
            return;
        }
        qreal rowH = 92 * u;
        for (int i = 0; i < giftMons.size(); i++)
        {
            const GiftMon& m = giftMons[i];
            bool selected = i == tradeUi.cursor;
            QRectF row(card.left() + 24 * u, card.top() + 76 * u + i * (rowH + 8 * u), card.width() - 48 * u, rowH);
            p.setPen(selected ? QPen(QColor(150, 110, 220), 4 * u) : Qt::NoPen);
            p.setBrush(selected ? QColor(245, 240, 255) : QColor(236, 240, 248));
            p.drawRoundedRect(row, 22 * u, 22 * u);
            const QImage& icon = pokemonIcon(m.species, m.egg);
            if (!icon.isNull())
            {
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(QRectF(row.left() + 10 * u, row.top() + 6 * u, 80 * u, 80 * u), icon.copy(0, 0, 32, 32));
                p.restore();
            }
            font.setPixelSize((int)(28 * u));
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(row.left() + 110 * u, row.top(), row.width() * 0.6, rowH), Qt::AlignVCenter | Qt::AlignLeft, m.egg ? QString::fromUtf8("Œuf") : m.name);
            p.setPen(QColor(110, 120, 140));
            if (!m.egg)
                p.drawText(QRectF(row.left(), row.top(), row.width() - 24 * u, rowH), Qt::AlignVCenter | Qt::AlignRight, QString("N. %1").arg(m.level));
        }
        p.restore();
        return;
    }
    if (!mon)
    {
        font.setBold(false);
        font.setPixelSize((int)(30 * u));
        p.setFont(font);
        p.setPen(QColor(140, 150, 170));
        p.drawText(card, Qt::AlignCenter, waiting);
        p.restore();
        return;
    }

    // The Pokemon
    QRectF spriteBox(card.center().x() - 100 * u, card.top() + 60 * u, 200 * u, 200 * u);
    const QImage& sprite = mon->egg ? pokemonIcon(0, true) : pokemonSprite(mon->species);
    if (!sprite.isNull())
    {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        QImage frame = mon->egg ? sprite.copy(0, 0, 32, 32)
                       : sprite.width() >= 2 * sprite.height() ? sprite.copy(0, 0, sprite.width() / 2, sprite.height()) : sprite;
        p.drawImage(spriteBox, frame);
        p.restore();
    }
    font.setPixelSize((int)(38 * u));
    p.setFont(font);
    p.setPen(QColor(32, 40, 56));
    QString head = mon->egg ? QString::fromUtf8("Œuf") : mon->name;
    if (!mon->egg)
        head += mon->gender == 0 ? QString::fromUtf8("  ♂") : mon->gender == 1 ? QString::fromUtf8("  ♀") : QString();
    p.drawText(QRectF(card.left(), spriteBox.bottom() + 4 * u, card.width(), 50 * u), Qt::AlignCenter, head + (mon->shiny ? QString::fromUtf8("  ✦") : QString()));
    if (mon->egg)
    {
        p.restore();
        return;
    }
    font.setPixelSize((int)(26 * u));
    p.setFont(font);
    p.setPen(QColor(200, 140, 30));
    p.drawText(QRectF(card.left(), spriteBox.bottom() + 54 * u, card.width(), 36 * u), Qt::AlignCenter, QString("N. %1").arg(mon->level));

    qreal y = spriteBox.bottom() + 100 * u;
    font.setBold(false);
    font.setPixelSize((int)(24 * u));
    p.setFont(font);
    p.setPen(QColor(80, 90, 110));
    p.drawText(QRectF(card.left() + 36 * u, y, card.width() - 72 * u, 32 * u), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("Talent : %1").arg(mon->ability));
    p.drawText(QRectF(card.left() + 36 * u, y + 34 * u, card.width() - 72 * u, 32 * u), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("Objet : %1").arg(mon->itemName.isEmpty() ? QString::fromUtf8("aucun") : mon->itemName));
    // Stats
    static const char* kStatNames[6] = { "PV", "Att", "Déf", "Vit", "A.Spé", "D.Spé" };
    y += 80 * u;
    for (int s = 0; s < 6; s++)
    {
        qreal col = s % 2, row = s / 2;
        QRectF cell(card.left() + 36 * u + col * (card.width() - 72 * u) / 2, y + row * 34 * u, (card.width() - 72 * u) / 2 - 16 * u, 30 * u);
        p.setPen(QColor(110, 120, 140));
        p.drawText(cell, Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(kStatNames[s]));
        p.setPen(QColor(32, 40, 56));
        p.drawText(cell, Qt::AlignVCenter | Qt::AlignRight, QString::number(mon->stats[s]));
    }
    // Moves
    y += 112 * u;
    for (int m = 0; m < 4; m++)
    {
        qreal col = m % 2, row = m / 2;
        QRectF pill(card.left() + 30 * u + col * (card.width() - 60 * u) / 2, y + row * 50 * u, (card.width() - 60 * u) / 2 - 12 * u, 42 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(m < mon->moves.size() ? QColor(232, 236, 246) : QColor(244, 246, 250));
        p.drawRoundedRect(pill, 21 * u, 21 * u);
        p.setPen(QColor(50, 58, 76));
        if (m < mon->moves.size())
            p.drawText(pill, Qt::AlignCenter, mon->moves[m]);
    }
    p.restore();
}

void GameUi::drawTrade(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(70, 50, 130));
    bg.setColorAt(1, QColor(20, 18, 50));
    p.fillRect(QRectF(0, 0, width, height), bg);

    QFont font("Segoe UI");
    font.setPixelSize((int)(46 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 30 * u, width, 70 * u), Qt::AlignCenter, QString::fromUtf8("Échange avec %1").arg(tradeUi.partner));

    const qreal cardW = std::min<qreal>(760 * u, (width - 220 * u) / 2), cardH = std::min<qreal>(height - 300 * u, 760 * u);
    QRectF left(width / 2.0 - cardW - 50 * u, 120 * u, cardW, cardH);
    QRectF right(width / 2.0 + 50 * u, 120 * u, cardW, cardH);
    drawTradeCard(p, left, u, tradeUi.picking ? QString::fromUtf8("Choisis ton Pokémon") : QString::fromUtf8("Tu donnes"),
                  tradeUi.haveMine ? &tradeUi.mine : nullptr, QString(), tradeUi.myConfirm, tradeUi.picking);
    drawTradeCard(p, right, u, QString::fromUtf8("%1 donne").arg(tradeUi.partner), tradeUi.haveTheirs ? &tradeUi.theirs : nullptr,
                  QString::fromUtf8("%1 choisit…").arg(tradeUi.partner), tradeUi.theirConfirm, false);
    // The arrows between the cards
    font.setPixelSize((int)(60 * u));
    p.setFont(font);
    p.setPen(QColor(255, 210, 120));
    p.drawText(QRectF(width / 2.0 - 50 * u, left.center().y() - 50 * u, 100 * u, 100 * u), Qt::AlignCenter, QString::fromUtf8("⇄"));

    // What to do now
    QString status;
    if (tradeUi.picking)
        status = QString::fromUtf8("Haut / Bas puis A : choisir le Pokémon à échanger   ·   B : annuler");
    else if (!tradeUi.haveTheirs)
        status = QString::fromUtf8("En attente de %1…   ·   B : annuler").arg(tradeUi.partner);
    else if (!tradeUi.myConfirm)
        status = QString::fromUtf8("A : confirmer l'échange   ·   B : annuler");
    else if (!tradeUi.theirConfirm)
        status = QString::fromUtf8("En attente de la confirmation de %1…   ·   B : annuler").arg(tradeUi.partner);
    else
        status = QString::fromUtf8("Échange en cours…");
    font.setPixelSize((int)(30 * u));
    font.setBold(false);
    p.setFont(font);
    p.setPen(QColor(230, 226, 245));
    p.drawText(QRectF(0, height - 140 * u, width, 60 * u), Qt::AlignCenter, status);
    p.restore();
}

void GameUi::drawTradeRequest(QPainter& p, int width, int height)
{
    const GiftMon& m = tradeRequest.theirs;
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    p.fillRect(QRectF(0, 0, width, height), QColor(10, 14, 28, 120));
    QRectF card((width - 940 * u) / 2, height * 0.5 - 210 * u, 940 * u, 420 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(card.translated(0, 10 * u), 40 * u, 40 * u);
    p.setBrush(QColor(246, 242, 255));
    p.drawRoundedRect(card, 40 * u, 40 * u);
    p.setBrush(PlayerColor(tradeRequest.slot));
    p.drawRoundedRect(QRectF(card.left(), card.top(), card.width(), 18 * u), 9 * u, 9 * u);
    QFont font("Segoe UI");
    font.setPixelSize((int)(36 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(40, 30, 70));
    p.drawText(QRectF(card.left(), card.top() + 40 * u, card.width(), 56 * u), Qt::AlignCenter,
               QString::fromUtf8("%1 te propose un échange !").arg(tradeRequest.from));
    QRectF iconBox(card.center().x() - 70 * u, card.top() + 110 * u, 140 * u, 140 * u);
    p.setBrush(Qt::white);
    p.drawEllipse(iconBox);
    const QImage& icon = pokemonIcon(m.species, m.egg);
    if (!icon.isNull())
    {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(iconBox.adjusted(6 * u, 0, -6 * u, -12 * u), icon.copy(0, ((frameCount / 16) % 2) * 32, 32, 32));
        p.restore();
    }
    font.setPixelSize((int)(32 * u));
    p.setFont(font);
    p.drawText(QRectF(card.left(), card.top() + 262 * u, card.width(), 46 * u), Qt::AlignCenter,
               m.egg ? QString::fromUtf8("Un Œuf") : QString::fromUtf8("%1  (N. %2)").arg(m.name).arg(m.level));
    DrawButtonHint(p, QPointF(card.center().x() - 330 * u, card.bottom() - 70 * u), 46 * u, "A", QString::fromUtf8("Choisir mon Pokémon"), QColor(150, 110, 220));
    DrawButtonHint(p, QPointF(card.center().x() + 120 * u, card.bottom() - 70 * u), 46 * u, "B", "Refuser", QColor(220, 80, 80));
    p.restore();
}

// ---------------------------------------------------------------------------
// Gifts: items, money or a Pokemon for a friend, who has to accept
// ---------------------------------------------------------------------------

static QString GiftQuantityLabel(const QString& name, int quantity)
{
    return quantity > 1 ? QString::fromUtf8("%1 ×%2").arg(name).arg(quantity) : name;
}

static QString MoneyLabel(melonDS::u32 amount)
{
    QString digits = QString::number(amount);
    for (int i = digits.size() - 3; i > 0; i -= 3)
        digits.insert(i, QChar(0x202F));
    return digits + QString::fromUtf8(" ₽");
}

void GameUi::giftOpenFor(int slot)
{
    giftOpen = true;
    giftTarget = slot;
    giftTab = 0;
    giftCursor = giftScroll = 0;
    giftQuantity = 1;
    giftMoneyAmount = 100;
    giftConfirm = false;
    giftHaveCatalog = false;
    giftCatalogAsked = true; // CoopLink asks the game for the bag and the party
}

bool GameUi::GiftCatalogWanted()
{
    QMutexLocker locker(&mutex);
    bool wanted = giftCatalogAsked;
    giftCatalogAsked = false;
    return wanted;
}

void GameUi::SetGiftCatalog(u32 money, const QList<GiftItem>& items, const QList<GiftMon>& mons)
{
    QMutexLocker locker(&mutex);
    giftMoney = money;
    giftItems = items;
    giftMons = mons;
    giftHaveCatalog = true;
    giftCursor = std::min(giftCursor, std::max(0, (int)(giftTab == 0 ? items.size() : mons.size()) - 1));
    giftMoneyAmount = std::min<u32>(std::max<u32>(giftMoneyAmount, money > 0 ? 1 : 0), money);
    dirty = true;
}

bool GameUi::TakeGiftChoice(GiftChoice& choice)
{
    QMutexLocker locker(&mutex);
    if (giftChoices.isEmpty())
        return false;
    choice = giftChoices.takeFirst();
    return true;
}

void GameUi::GiftOffered(int key, int slot, const QString& from, const QString& label, int kind, int itemId, int species, int quantity)
{
    QMutexLocker locker(&mutex);
    giftOffers.append({ key, slot, kind, itemId, species, quantity, from, label, frameCount });
    dirty = true;
}

bool GameUi::TakeGiftAnswer(int& key, bool& accept)
{
    QMutexLocker locker(&mutex);
    if (giftAnswers.isEmpty())
        return false;
    key = giftAnswers.first().first;
    accept = giftAnswers.first().second;
    giftAnswers.removeFirst();
    return true;
}

// The gift screen's controls (mutex held). Returns true when something changed.
bool GameUi::giftInput(u32 newPressed)
{
    if (!newPressed)
        return false;
    QString friendName;
    for (const CoopPlayer& p : coopPlayers)
        if (p.slot == giftTarget)
            friendName = p.name;
    if (friendName.isEmpty())
    {
        giftOpen = false; // the friend left
        pushToast(QString::fromUtf8("Ton ami s'est déconnecté"), QColor(239, 106, 90));
        return true;
    }
    if (giftConfirm)
    {
        if (newPressed & kKeyA)
        {
            GiftChoice c;
            c.target = giftTarget;
            if (giftTab == 0 && giftCursor < giftItems.size())
            {
                const GiftItem& item = giftItems[giftCursor];
                c.kind = 1;
                c.id = item.id;
                c.quantity = giftQuantity;
                c.label = GiftQuantityLabel(item.name, giftQuantity);
            }
            else if (giftTab == 1)
            {
                c.kind = 2;
                c.quantity = giftMoneyAmount;
                c.label = MoneyLabel(giftMoneyAmount);
            }
            else if (giftTab == 2 && giftCursor < giftMons.size())
            {
                const GiftMon& mon = giftMons[giftCursor];
                c.kind = 3;
                c.id = giftCursor;
                c.quantity = 1;
                c.species = mon.egg ? 0 : mon.species;
                c.level = mon.level;
                c.label = mon.egg ? QString::fromUtf8("un Œuf") : QString::fromUtf8("%1 (N. %2)").arg(mon.name).arg(mon.level);
            }
            if (c.kind)
                giftChoices.append(c);
            giftOpen = false;
            giftConfirm = false;
        }
        else if (newPressed & (kKeyB | kKeyX))
            giftConfirm = false;
        return true;
    }
    if (newPressed & (kKeyB | kKeyX))
    {
        giftOpen = false;
        return true;
    }
    if (newPressed & (kKeyL | kKeyR))
    {
        giftTab = (giftTab + ((newPressed & kKeyR) ? 1 : 2)) % 3;
        giftCursor = giftScroll = 0;
        giftQuantity = 1;
        return true;
    }
    if (!giftHaveCatalog)
        return false;
    if (giftTab == 1)
    {
        // Money: up/down by 100, left/right by 1000
        s64 amount = giftMoneyAmount;
        if (newPressed & kKeyUp) amount += 100;
        if (newPressed & kKeyDown) amount -= 100;
        if (newPressed & kKeyRight) amount += 1000;
        if (newPressed & kKeyLeft) amount -= 1000;
        giftMoneyAmount = (u32)std::clamp<s64>(amount, giftMoney > 0 ? 1 : 0, giftMoney);
        if ((newPressed & kKeyA) && giftMoneyAmount > 0)
            giftConfirm = true;
        return true;
    }
    int count = giftTab == 0 ? giftItems.size() : giftMons.size();
    if (count == 0)
        return false;
    if (newPressed & kKeyUp)
        giftCursor = (giftCursor + count - 1) % count, giftQuantity = 1;
    if (newPressed & kKeyDown)
        giftCursor = (giftCursor + 1) % count, giftQuantity = 1;
    if (giftTab == 0)
    {
        int have = giftItems[giftCursor].quantity;
        if (newPressed & kKeyRight) giftQuantity = std::min(have, giftQuantity + 1);
        if (newPressed & kKeyLeft) giftQuantity = std::max(1, giftQuantity - 1);
    }
    if (newPressed & kKeyA)
        giftConfirm = true;
    return true;
}

void GameUi::drawPlayersAction(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = height / 1080.0;
    QRectF box((width - 900 * u) / 2, height / 2.0 - 110 * u, 900 * u, 220 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(box.translated(0, 8 * u), 36 * u, 36 * u);
    p.setBrush(QColor(250, 252, 255));
    p.drawRoundedRect(box, 36 * u, 36 * u);
    QString who = playersCursor > 0 && playersCursor <= (int)coopPlayers.size() ? coopPlayers[playersCursor - 1].name : QString();
    QFont font("Segoe UI");
    font.setPixelSize((int)(32 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(40, 48, 66));
    p.drawText(QRectF(box.left(), box.top() + 22 * u, box.width(), 50 * u), Qt::AlignCenter, who);
    const QString labels[3] = { QString::fromUtf8("Écrire"), QString::fromUtf8("Offrir un cadeau"), QString::fromUtf8("Échanger") };
    const QColor colors[3] = { QColor(70, 150, 220), QColor(240, 186, 40), QColor(150, 110, 220) };
    for (int i = 0; i < 3; i++)
    {
        QRectF b(box.left() + 30 * u + i * 285 * u, box.top() + 100 * u, 270 * u, 84 * u);
        bool selected = playersAction == i;
        p.setPen(selected ? QPen(QColor(255, 196, 60), 5 * u) : Qt::NoPen);
        p.setBrush(selected ? colors[i] : colors[i].lighter(150));
        p.drawRoundedRect(b, 42 * u, 42 * u);
        p.setPen(Qt::white);
        font.setPixelSize((int)(28 * u));
        p.setFont(font);
        p.drawText(b, Qt::AlignCenter, labels[i]);
    }
    p.restore();
}

void GameUi::drawGift(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = height / 1080.0;
    p.fillRect(QRectF(0, 0, width, height), QColor(10, 14, 28, 160));
    const qreal panelW = 1200 * u, panelH = 860 * u;
    QRectF panel((width - panelW) / 2, (height - panelH) / 2, panelW, panelH);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(panel.translated(0, 10 * u), 40 * u, 40 * u);
    QLinearGradient bg(panel.topLeft(), panel.bottomLeft());
    bg.setColorAt(0, QColor(252, 248, 236));
    bg.setColorAt(1, QColor(238, 232, 214));
    p.setBrush(bg);
    p.drawRoundedRect(panel, 40 * u, 40 * u);

    QString friendName;
    for (const CoopPlayer& f : coopPlayers)
        if (f.slot == giftTarget)
            friendName = f.name;
    QFont title("Segoe UI");
    title.setPixelSize((int)(40 * u));
    title.setBold(true);
    p.setFont(title);
    p.setPen(QColor(60, 44, 20));
    p.drawText(QRectF(panel.left() + 48 * u, panel.top() + 26 * u, panelW - 96 * u, 60 * u), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("Un cadeau pour %1").arg(friendName));

    // Tabs (L / R)
    const QString tabs[3] = { QString::fromUtf8("Objets"), QString::fromUtf8("Argent"), QString::fromUtf8("Pokémon") };
    QFont tabFont("Segoe UI");
    tabFont.setPixelSize((int)(26 * u));
    tabFont.setBold(true);
    for (int i = 0; i < 3; i++)
    {
        QRectF t(panel.left() + 48 * u + i * 230 * u, panel.top() + 100 * u, 210 * u, 58 * u);
        bool on = giftTab == i;
        p.setPen(Qt::NoPen);
        p.setBrush(on ? QColor(240, 170, 40) : QColor(255, 255, 255, 170));
        p.drawRoundedRect(t, 29 * u, 29 * u);
        p.setFont(tabFont);
        p.setPen(on ? Qt::white : QColor(120, 100, 70));
        p.drawText(t, Qt::AlignCenter, tabs[i]);
    }
    QFont small("Segoe UI");
    small.setPixelSize((int)(22 * u));
    p.setFont(small);
    p.setPen(QColor(140, 120, 90));
    p.drawText(QRectF(panel.left() + 760 * u, panel.top() + 100 * u, panelW - 808 * u, 58 * u), Qt::AlignVCenter | Qt::AlignRight,
               QString::fromUtf8("L / R : changer d'onglet"));

    QRectF area(panel.left() + 48 * u, panel.top() + 190 * u, panelW - 96 * u, panelH - 300 * u);
    QFont name("Segoe UI");
    name.setPixelSize((int)(30 * u));
    name.setBold(true);
    QFont normal("Segoe UI");
    normal.setPixelSize((int)(26 * u));
    if (!giftHaveCatalog)
    {
        p.setFont(normal);
        p.setPen(QColor(140, 120, 90));
        p.drawText(area, Qt::AlignCenter, QString::fromUtf8("Ouverture du sac…"));
    }
    else if (giftTab == 1)
    {
        p.setFont(normal);
        p.setPen(QColor(120, 100, 70));
        p.drawText(QRectF(area.left(), area.top() + 40 * u, area.width(), 40 * u), Qt::AlignCenter,
                   QString::fromUtf8("Tu as %1").arg(MoneyLabel(giftMoney)));
        QFont big("Segoe UI");
        big.setPixelSize((int)(96 * u));
        big.setBold(true);
        p.setFont(big);
        p.setPen(QColor(200, 140, 20));
        p.drawText(QRectF(area.left(), area.top() + 120 * u, area.width(), 140 * u), Qt::AlignCenter, MoneyLabel(giftMoneyAmount));
        p.setFont(normal);
        p.setPen(QColor(140, 120, 90));
        p.drawText(QRectF(area.left(), area.top() + 290 * u, area.width(), 40 * u), Qt::AlignCenter,
                   QString::fromUtf8("Haut / Bas : ±100      Gauche / Droite : ±1 000"));
    }
    else
    {
        const int rows = 6;
        int count = giftTab == 0 ? giftItems.size() : giftMons.size();
        if (count == 0)
        {
            p.setFont(normal);
            p.setPen(QColor(140, 120, 90));
            p.drawText(area, Qt::AlignCenter, giftTab == 0 ? QString::fromUtf8("Rien à offrir dans ton sac.") : QString::fromUtf8("Personne dans ton équipe."));
        }
        if (giftCursor < giftScroll)
            giftScroll = giftCursor;
        if (giftCursor >= giftScroll + rows)
            giftScroll = giftCursor - rows + 1;
        const qreal rowH = area.height() / rows;
        for (int r = 0; r < rows && giftScroll + r < count; r++)
        {
            int i = giftScroll + r;
            bool selected = i == giftCursor;
            QRectF row(area.left(), area.top() + r * rowH, area.width(), rowH - 10 * u);
            p.setPen(selected ? QPen(QColor(240, 170, 40), 4 * u) : Qt::NoPen);
            p.setBrush(selected ? QColor(255, 250, 235) : QColor(255, 255, 255, 150));
            p.drawRoundedRect(row, 22 * u, 22 * u);
            QRectF iconBox(row.left() + 16 * u, row.center().y() - 40 * u, 80 * u, 80 * u);
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            QString left, right;
            if (giftTab == 0)
            {
                const GiftItem& item = giftItems[i];
                const QImage& icon = itemIcon(item.id);
                if (!icon.isNull())
                    p.drawImage(iconBox.adjusted(8 * u, 8 * u, -8 * u, -8 * u), icon);
                left = item.name;
                right = selected ? QString::fromUtf8("◀  ×%1  ▶     (tu en as %2)").arg(giftQuantity).arg(item.quantity)
                                 : QString::fromUtf8("×%1").arg(item.quantity);
            }
            else
            {
                const GiftMon& mon = giftMons[i];
                const QImage& icon = pokemonIcon(mon.species, mon.egg);
                if (!icon.isNull())
                    p.drawImage(iconBox, icon.copy(0, 0, 32, 32));
                left = mon.egg ? QString::fromUtf8("Œuf") : mon.name;
                right = mon.egg ? QString() : QString::fromUtf8("N. %1").arg(mon.level);
            }
            p.restore();
            p.setFont(name);
            p.setPen(QColor(50, 40, 25));
            p.drawText(QRectF(iconBox.right() + 20 * u, row.top(), row.width() * 0.5, row.height()), Qt::AlignVCenter | Qt::AlignLeft, left);
            p.setFont(normal);
            p.setPen(QColor(130, 110, 80));
            p.drawText(QRectF(row.left(), row.top(), row.width() - 28 * u, row.height()), Qt::AlignVCenter | Qt::AlignRight, right);
        }
    }

    // Footer / confirmation
    QRectF footer(panel.left() + 48 * u, panel.bottom() - 96 * u, panelW - 96 * u, 64 * u);
    if (giftConfirm)
    {
        QString what;
        if (giftTab == 0 && giftCursor < giftItems.size())
            what = GiftQuantityLabel(giftItems[giftCursor].name, giftQuantity);
        else if (giftTab == 1)
            what = MoneyLabel(giftMoneyAmount);
        else if (giftCursor < giftMons.size())
            what = giftMons[giftCursor].egg ? QString::fromUtf8("cet Œuf") : giftMons[giftCursor].name;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 44, 20));
        p.drawRoundedRect(footer, 32 * u, 32 * u);
        p.setFont(name);
        p.setPen(Qt::white);
        p.drawText(footer.adjusted(30 * u, 0, -30 * u, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromUtf8("Offrir %1 à %2 ?").arg(what, friendName));
        p.setFont(normal);
        p.setPen(QColor(255, 210, 120));
        p.drawText(footer.adjusted(30 * u, 0, -30 * u, 0), Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("A : oui    B : non"));
    }
    else
    {
        DrawButtonHint(p, QPointF(footer.left(), footer.top() + 8 * u), 46 * u, "A", QString::fromUtf8("Offrir"), QColor(76, 176, 120));
        DrawButtonHint(p, QPointF(footer.left() + 240 * u, footer.top() + 8 * u), 46 * u, "B", "Retour", QColor(220, 80, 80));
        p.setFont(small);
        p.setPen(QColor(140, 120, 90));
        p.drawText(footer, Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("Ton ami devra accepter le cadeau"));
    }
    p.restore();
}

void GameUi::drawGiftOffer(QPainter& p, int width, int height)
{
    const GiftOffer& offer = giftOffers.first();
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    p.fillRect(QRectF(0, 0, width, height), QColor(10, 14, 28, 120));
    QRectF card((width - 900 * u) / 2, height * 0.5 - 200 * u, 900 * u, 400 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(card.translated(0, 10 * u), 40 * u, 40 * u);
    QLinearGradient bg(card.topLeft(), card.bottomLeft());
    bg.setColorAt(0, QColor(255, 250, 232));
    bg.setColorAt(1, QColor(244, 232, 200));
    p.setBrush(bg);
    p.drawRoundedRect(card, 40 * u, 40 * u);
    // The gift's ribbon, in the friend's color
    p.setBrush(PlayerColor(offer.slot));
    p.drawRoundedRect(QRectF(card.left(), card.top(), card.width(), 18 * u), 9 * u, 9 * u);

    QFont title("Segoe UI");
    title.setPixelSize((int)(38 * u));
    title.setBold(true);
    p.setFont(title);
    p.setPen(QColor(60, 44, 20));
    p.drawText(QRectF(card.left(), card.top() + 40 * u, card.width(), 56 * u), Qt::AlignCenter,
               QString::fromUtf8("%1 t'offre un cadeau !").arg(offer.from));

    QRectF iconBox(card.center().x() - 70 * u, card.top() + 115 * u, 140 * u, 140 * u);
    p.setBrush(QColor(255, 255, 255, 200));
    p.drawEllipse(iconBox);
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (offer.kind == 1)
    {
        const QImage& icon = itemIcon(offer.itemId);
        if (!icon.isNull())
            p.drawImage(iconBox.adjusted(30 * u, 30 * u, -30 * u, -30 * u), icon);
    }
    else if (offer.kind == 3)
    {
        const QImage& icon = pokemonIcon(offer.species, offer.species == 0);
        if (!icon.isNull())
            p.drawImage(iconBox.adjusted(6 * u, 0, -6 * u, -12 * u), icon.copy(0, ((frameCount / 16) % 2) * 32, 32, 32));
    }
    else
    {
        QFont coin("Segoe UI");
        coin.setPixelSize((int)(70 * u));
        coin.setBold(true);
        p.setFont(coin);
        p.setPen(QColor(220, 160, 30));
        p.drawText(iconBox, Qt::AlignCenter, QString::fromUtf8("₽"));
    }
    p.restore();

    QFont label("Segoe UI");
    label.setPixelSize((int)(34 * u));
    label.setBold(true);
    p.setFont(label);
    p.setPen(QColor(50, 40, 25));
    p.drawText(QRectF(card.left(), card.top() + 270 * u, card.width(), 50 * u), Qt::AlignCenter, offer.label);
    DrawButtonHint(p, QPointF(card.center().x() - 230 * u, card.bottom() - 68 * u), 46 * u, "A", "Accepter", QColor(76, 176, 120));
    DrawButtonHint(p, QPointF(card.center().x() + 60 * u, card.bottom() - 68 * u), 46 * u, "B", "Refuser", QColor(220, 80, 80));
    p.restore();
}

// ---------------------------------------------------------------------------
// Chat (online): a general channel for the server and private messages
// ---------------------------------------------------------------------------

bool GameUi::ChatKey(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F12)
    {
        reportAsked = true;
        return true;
    }
    QMutexLocker locker(&mutex);
    if (namingActive && screen.type == kScreenNaming)
        return namingKey(event);
    if (!coopMode || !started)
        return false;
    if (!chatOpen)
    {
        if (event->key() == Qt::Key_T && !event->isAutoRepeat() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
        {
            chatOpen = true;
            chatInput.clear();
            dirty = true;
            return true;
        }
        return false;
    }
    switch (event->key())
    {
    case Qt::Key_Escape:
        chatOpen = false;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        chatSend();
        chatOpen = false;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    {
        // Everybody, then each friend online
        QList<int> targets { -1 };
        for (const CoopPlayer& p : coopPlayers)
            targets.append(p.slot);
        int i = std::max(0, (int)targets.indexOf(chatTarget));
        int step = event->key() == Qt::Key_Backtab ? targets.size() - 1 : 1;
        chatTarget = targets[(i + step) % targets.size()];
        break;
    }
    case Qt::Key_Backspace:
        chatInput.chop(1);
        break;
    default:
    {
        QString text = event->text();
        for (QChar c : text)
            if (c.isPrint() && chatInput.size() < kChatMaxLength)
                chatInput.append(c);
        break;
    }
    }
    dirty = true;
    return true; // while the chat is open, every key is for it
}

void GameUi::chatSend()
{
    QString text = chatInput.simplified();
    chatInput.clear();
    if (text.isEmpty())
        return;
    // The friend may have left: then it goes to everybody
    bool found = chatTarget < 0;
    for (const CoopPlayer& p : coopPlayers)
        found |= p.slot == chatTarget;
    if (!found)
        chatTarget = -1;
    chatOut.append({ chatTarget, text });
    chatAdd({ localName, text, chatTargetName(), coopMySlot, chatTarget >= 0, frameCount });
}

QString GameUi::chatTargetName() const
{
    for (const CoopPlayer& p : coopPlayers)
        if (p.slot == chatTarget)
            return p.name;
    return QString();
}

void GameUi::chatAdd(const ChatLine& line)
{
    chatLines.append(line);
    while (chatLines.size() > 50)
        chatLines.removeFirst();
    dirty = true;
}

void GameUi::ChatReceived(int slot, const QString& from, const QString& text, bool isPrivate)
{
    QMutexLocker locker(&mutex);
    ChatLine line { from, text, QString(), slot, isPrivate, frameCount };
    if (isPrivate)
        line.to = localName;
    chatAdd(line);
}

bool GameUi::TakeChatOut(int& target, QString& text)
{
    QMutexLocker locker(&mutex);
    if (chatOut.isEmpty())
        return false;
    target = chatOut.first().first;
    text = chatOut.first().second;
    chatOut.removeFirst();
    return true;
}

void GameUi::ChatSay(const QString& text, const QString& to)
{
    QMutexLocker locker(&mutex);
    chatTarget = -1;
    for (const CoopPlayer& p : coopPlayers)
        if (p.name == to)
            chatTarget = p.slot;
    chatInput = text;
    chatSend();
}

void GameUi::NamingType(const QString& text)
{
    QMutexLocker locker(&mutex);
    namingPending = text; // typed as soon as the box is open
    if (namingActive)
    {
        namingInput = namingPending;
        namingPending.clear();
    }
    dirty = true;
}

void GameUi::ChatShowInput(const QString& text)
{
    QMutexLocker locker(&mutex);
    chatOpen = true;
    chatInput = text;
    dirty = true;
}

bool GameUi::chatVisible() const
{
    return chatOpen || (!chatLines.isEmpty() && frameCount - chatLines.last().frame < kChatShowFrames + 40);
}

void GameUi::drawChat(QPainter& p, int width, int height)
{
    if (!chatVisible())
        return;
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const qreal panelW = std::min<qreal>(760 * u, width - 60 * u);
    const qreal left = 30 * u;
    qreal bottom = height - 30 * u;

    QFont font("Segoe UI");
    font.setPixelSize((int)(25 * u));
    QFont bold = font;
    bold.setBold(true);
    QFontMetricsF fm(font), fmBold(bold);

    // The input box, when typing
    if (chatOpen)
    {
        QRectF box(left, bottom - 64 * u, panelW, 64 * u);
        p.setPen(QPen(QColor(255, 196, 60), 3 * u));
        p.setBrush(QColor(16, 20, 34, 235));
        p.drawRoundedRect(box, 20 * u, 20 * u);
        // Who it goes to
        QString who = chatTarget < 0 ? QString::fromUtf8("Tous") : chatTargetName();
        QColor whoColor = chatTarget < 0 ? QColor(90, 160, 230) : PlayerColor(chatTarget);
        qreal pillW = fmBold.horizontalAdvance(who) + 40 * u;
        QRectF pill(box.left() + 12 * u, box.top() + 12 * u, pillW, box.height() - 24 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(whoColor);
        p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        p.setFont(bold);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, who);
        // The text, with a blinking caret; the end stays visible
        QRectF textRect(pill.right() + 16 * u, box.top(), box.right() - pill.right() - 32 * u, box.height());
        QString shown = chatInput;
        while (!shown.isEmpty() && fm.horizontalAdvance(shown + "|") > textRect.width())
            shown.remove(0, 1);
        p.setFont(font);
        p.setPen(QColor(240, 244, 250));
        bool caret = (frameCount / 30) % 2 == 0;
        if (chatInput.isEmpty())
        {
            p.setPen(QColor(140, 150, 170));
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Écris ton message…"));
        }
        else
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, shown + (caret ? "|" : ""));
        // Help
        QFont help = font;
        help.setPixelSize((int)(19 * u));
        p.setFont(help);
        p.setPen(QColor(220, 225, 235));
        QRectF helpRect(left + 6 * u, box.top() - 34 * u, panelW, 28 * u);
        p.drawText(helpRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromUtf8("Entrée : envoyer   ·   Tab : destinataire   ·   Échap : fermer"));
        bottom = helpRect.top() - 8 * u;
    }

    // The last lines, the newest at the bottom (all of them while typing, else only the recent ones)
    const int maxLines = chatOpen ? 8 : 5;
    QList<ChatLine> lines;
    for (int i = chatLines.size() - 1; i >= 0 && lines.size() < maxLines; i--)
    {
        const ChatLine& l = chatLines[i];
        if (!chatOpen && frameCount - l.frame >= kChatShowFrames + 40)
            break;
        lines.prepend(l);
    }
    const qreal pad = 16 * u;
    qreal y = bottom;
    for (int i = lines.size() - 1; i >= 0; i--)
    {
        const ChatLine& l = lines[i];
        QString head = l.from;
        if (l.isPrivate)
            head = l.to.isEmpty() || l.to == localName ? l.from + QString::fromUtf8(" (privé)")
                                                       : l.from + QString::fromUtf8(" → ") + l.to;
        head += "  ";
        qreal headW = fmBold.horizontalAdvance(head);
        QRectF textArea(left + pad + headW, 0, panelW - 2 * pad - headW, 1000 * u);
        QRectF need = fm.boundingRect(textArea, Qt::TextWordWrap | Qt::AlignLeft, l.text);
        qreal h = std::max(need.height(), fmBold.height()) + 16 * u;
        y -= h;
        // Fades out in its last second
        qreal age = frameCount - l.frame;
        qreal alpha = chatOpen ? 1.0 : std::clamp(1.0 - (age - kChatShowFrames) / 40.0, 0.0, 1.0);
        p.setOpacity(alpha);
        QRectF bubble(left, y, panelW, h - 6 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(l.isPrivate ? QColor(60, 30, 80, 200) : QColor(12, 16, 28, 175));
        p.drawRoundedRect(bubble, 14 * u, 14 * u);
        p.setFont(bold);
        p.setPen(PlayerColor(l.slot).lighter(130));
        p.drawText(QRectF(left + pad, y + 5 * u, headW, fmBold.height()), Qt::AlignLeft | Qt::AlignTop, head);
        p.setFont(font);
        p.setPen(QColor(240, 244, 250));
        p.drawText(QRectF(left + pad + headW, y + 5 * u, textArea.width(), need.height() + 4 * u),
                   Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, l.text);
        y -= 6 * u;
    }
    p.setOpacity(1.0);
    p.restore();
}

void GameUi::drawCoopStatus(QPainter& p, int width, int height)
{
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    QFont font("Segoe UI");
    font.setPixelSize((int)(26 * u));
    font.setBold(true);
    p.save();
    if (toastFrames > 0)
    {
        qreal t = std::min(1.0, std::min(toastFrames / 30.0, (240 - toastFrames) / 12.0)); // fades in and out
        QFontMetrics fm(font);
        qreal w = fm.horizontalAdvance(toastText) + 96 * u;
        QRectF r(width / 2.0 - w / 2, (18 + 12 * t) * u, w, 64 * u);
        p.setOpacity(t);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRoundedRect(r.translated(0, 5 * u), 32 * u, 32 * u);
        p.setBrush(QColor(22, 18, 30, 235));
        p.drawRoundedRect(r, 32 * u, 32 * u);
        p.setBrush(toastColor);
        p.drawEllipse(QPointF(r.left() + 34 * u, r.center().y()), 9 * u, 9 * u);
        p.setPen(QPen(toastColor, 2 * u));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 32 * u, 32 * u);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(r.adjusted(58 * u, 0, -24 * u, 0), Qt::AlignVCenter | Qt::AlignLeft, toastText);
        p.setOpacity(1);
    }
    if (coopMode && networkStatus != 2)
    {
        QString text = networkStatus == -3 ? QString::fromUtf8("Session expirée : relance Ho-Lympe")
            : networkStatus == -4 ? QString::fromUtf8("Serveur plein (6 joueurs)")
            : networkStatus == -5 ? QString::fromUtf8("Tu as été expulsé du serveur")
            : networkStatus <= 0 ? QString::fromUtf8("Connexion au serveur…")
            : QString::fromUtf8("En ligne · personne d'autre pour l'instant");
        font.setPixelSize((int)(22 * u));
        QFontMetrics fm(font);
        qreal w = fm.horizontalAdvance(text) + 80 * u;
        QRectF r(width - w - 30 * u, 24 * u, w, 50 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 24, 40, 200));
        p.drawRoundedRect(r, 25 * u, 25 * u);
        p.setBrush(networkStatus == 1 ? QColor(95, 211, 138) : networkStatus < -2 ? QColor(239, 106, 90) : QColor(255, 196, 60));
        p.drawEllipse(QPointF(r.left() + 28 * u, r.center().y()), 7 * u, 7 * u);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(r.adjusted(48 * u, 0, -16 * u, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    p.restore();
}

void GameUi::NuzlockeMessage(const QString& text, const QColor& color)
{
    QMutexLocker locker(&mutex);
    pushToast(text, color);
}

void GameUi::SetNuzlockeOver()
{
    QMutexLocker locker(&mutex);
    nuzOver = true;
    dirty = true;
}

bool GameUi::IsNuzlockeOver()
{
    QMutexLocker locker(&mutex);
    return nuzOver;
}

void GameUi::SetNuzlockeLost(const QByteArray& dead)
{
    QMutexLocker locker(&mutex);
    int count = 0;
    for (int i = 0; i + 4 <= dead.size(); i += 4)
        if (dead[i] || dead[i + 1] || dead[i + 2] || dead[i + 3])
            count++;
    nuzLost = count;
}

QString GameUi::LocationName()
{
    QMutexLocker locker(&mutex);
    return locationName;
}

QJsonObject GameUi::SaveSummary()
{
    QMutexLocker locker(&mutex);
    return saveSummary;
}

// Our game's place, team, badges and play time (CoopInfo): for the launcher and the server
void GameUi::updateInfo(u8* mailbox)
{
    const u8* info = mailbox + CoopLink::kOffInfo;
    if (!info[65]) // valid
        return;
    QString location = DecodeGameText((const u16*)info, 24);
    QJsonArray team;
    for (int i = 0; i < 6; i++)
    {
        int species = info[48 + i * 2] | (info[49 + i * 2] << 8);
        if (species)
            team.append(species);
    }
    int playtime = info[60] | (info[61] << 8) | (info[62] << 16) | (info[63] << 24);
    locationName = location;
    saveSummary = QJsonObject { { "location", location }, { "team", team }, { "badges", info[64] }, { "playtime", playtime }, { "lost", nuzLost } , { "look", LookString() } };
}

void GameUi::SetTopScreenTransform(const float* mtx, int widgetWidth, int widgetHeight)
{
    QMutexLocker locker(&mutex);
    topVisible = mtx != nullptr && widgetWidth > 0 && widgetHeight > 0;
    if (topVisible)
    {
        memcpy(topMtx, mtx, sizeof(topMtx));
        topWidgetW = (float)widgetWidth;
        topWidgetH = (float)widgetHeight;
    }
    dirty = true;
}

// Places the names: the game gives its field camera and the characters' world positions,
// projected here like the DS 3D engine does (look-at camera, perspective)
void GameUi::updateNameplates(u8* mailbox)
{
    const u8* names = mailbox + CoopLink::kOffNames;
    auto u32At = [](const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); };
    auto fx = [&](const u8* p) { return (float)(s32)u32At(p) / 4096.0f; };

    u32 frame = u32At(names + 16);
    namesIdleFrames = frame != lastNamesFrame ? 0 : std::min(namesIdleFrames + 1, 100);
    lastNamesFrame = frame;

    Nameplate next[kPlates];
    float fovySin = fx(names + 56), fovyCos = fx(names + 60), aspect = fx(names + 64);
    bool shown = coopMode && topVisible && namesIdleFrames < 4 && screen.type == 0 && state.type == 0
        && coverAlpha <= 0.0f && fovySin > 0.0f && aspect > 0.0f;
    if (shown)
    {
        float cam[3], target[3], up[3];
        for (int i = 0; i < 3; i++)
        {
            cam[i] = fx(names + 20 + i * 4);
            target[i] = fx(names + 32 + i * 4);
            up[i] = fx(names + 44 + i * 4);
        }
        auto normalize = [](float* v) {
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 0)
                for (int i = 0; i < 3; i++) v[i] /= len;
        };
        auto cross = [](const float* a, const float* b, float* out) {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        };
        auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        float forward[3] = { target[0] - cam[0], target[1] - cam[1], target[2] - cam[2] }, right[3], camUp[3];
        normalize(forward);
        cross(forward, up, right);
        normalize(right);
        cross(right, forward, camUp);
        float cot = fovyCos / fovySin;

        auto project = [&](const u8* pos, Nameplate& plate) {
            float d[3] = { fx(pos) - cam[0], fx(pos + 4) - cam[1], fx(pos + 8) - cam[2] };
            float z = dot(d, forward);
            if (z <= 1.0f)
                return;
            plate.x = 128.0f + 128.0f * (cot / aspect) * dot(d, right) / z;
            plate.y = 96.0f - 96.0f * cot * dot(d, camUp) / z;
            plate.visible = plate.x > -40 && plate.x < 296 && plate.y > -20 && plate.y < 240;
        };
        // Us, then each other player whose character is on our map
        if (!localName.isEmpty())
        {
            project(mailbox + 16, next[0]); // CoopMailbox.local.pos
            next[0].name = localName;
            next[0].color = PlayerColor(coopMySlot);
        }
        for (const CoopPlayer& p : coopPlayers)
        {
            if (p.remote < 0 || p.remote >= kMaxRemotePlayers)
                continue;
            const u8* remote = mailbox + CoopLink::kOffRemotes + p.remote * CoopLink::kRemoteSize;
            if (!remote[CoopLink::kRemGhostVisible] || !remote[CoopLink::kRemActive])
                continue;
            Nameplate& plate = next[1 + p.remote];
            project(remote + CoopLink::kRemGhostPos, plate);
            plate.name = p.name;
            plate.color = PlayerColor(p.slot);
        }
    }

    for (int i = 0; i < kPlates; i++)
    {
        if (next[i].visible != plates[i].visible || next[i].name != plates[i].name
            || std::fabs(next[i].x - plates[i].x) > 0.05f || std::fabs(next[i].y - plates[i].y) > 0.05f)
            dirty = true;
        plates[i] = next[i];
    }
}

bool GameUi::anyPlateVisible() const
{
    for (const Nameplate& p : plates)
        if (p.visible)
            return true;
    return false;
}

void GameUi::drawNameplates(QPainter& p, int width, int height)
{
    if (!topVisible || !anyPlateVisible())
        return;
    float sx = width / topWidgetW, sy = height / topWidgetH;
    qreal scale = std::hypot(topMtx[0], topMtx[1]) * sx; // image pixels per DS pixel
    static constexpr float kHeadHeight = 24.0f;            // DS pixels from the feet to above the head

    // Small white text outlined in the player's color, readable on any ground
    QFont font("Segoe UI");
    font.setPixelSize(std::max(11, (int)(5.5 * scale)));
    font.setBold(true);
    QFontMetricsF fm(font);
    p.save();
    for (int i = kPlates - 1; i >= 0; i--)
    {
        const Nameplate& plate = plates[i];
        if (!plate.visible || plate.name.isEmpty())
            continue;
        float x = plate.x, y = plate.y - kHeadHeight;
        QPointF anchor((topMtx[0] * x + topMtx[2] * y + topMtx[4]) * sx, (topMtx[1] * x + topMtx[3] * y + topMtx[5]) * sy);
        QPainterPath path;
        path.addText(anchor.x() - fm.horizontalAdvance(plate.name) / 2, anchor.y() - fm.descent(), font, plate.name);
        QColor outline = plate.color.darker(170);
        outline.setAlpha(230);
        p.setPen(QPen(outline, std::max(2.5, scale * 0.75), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawPath(path);
    }
    p.restore();
}

bool GameUi::Render(QImage& image, int width, int height, bool& changed)
{
    QMutexLocker locker(&mutex);
    changed = false;
    bool coopVisible = toastFrames > 0 || nuzOver || (coopMode && (networkStatus != 2 || anyPlateVisible() || chatVisible() || !giftOffers.isEmpty()
                                                              || tradeUi.open || tradeRequest.pending));
    if ((state.type == 0 && screen.type == 0 && coverAlpha <= 0.0f && !coopVisible) || width <= 0 || height <= 0)
        return false;
    if (dirty || width != lastWidth || height != lastHeight || image.width() != width || image.height() != height)
    {
        draw(image, width, height);
        lastWidth = width;
        lastHeight = height;
        dirty = false;
        changed = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Drawing (Brilliant Diamond / Shining Pearl inspired)
// ---------------------------------------------------------------------------

static const char* ActionLabel(int action)
{
    switch (action)
    {
    case kActionPokedex: return "Pokédex";
    case kActionPokemon: return "Pokémon";
    case kActionBag: return "Sac";
    case kActionTrainerCard: return "Carte";
    case kActionSave: return "Sauver";
    case kActionOptions: return "Options";
    case kActionRetire: return "Abandon";
    case kActionPokegear:
    case kActionPokegearAlt: return "Pokégear";
    case kActionPlayers: return "Joueurs";
    case kActionCemetery: return "Cimetière";
    default: return "";
    }
}

static QColor ActionColor(int action)
{
    switch (action)
    {
    case kActionPokedex: return QColor(226, 70, 70);
    case kActionPokemon: return QColor(232, 96, 72);
    case kActionBag: return QColor(222, 150, 58);
    case kActionTrainerCard: return QColor(70, 150, 220);
    case kActionSave: return QColor(76, 176, 120);
    case kActionOptions: return QColor(120, 128, 150);
    case kActionPokegear:
    case kActionPokegearAlt: return QColor(240, 186, 40);
    case kActionPlayers: return QColor(150, 110, 220);
    case kActionCemetery: return QColor(110, 70, 100);
    default: return QColor(150, 150, 160);
    }
}

// Simple white glyphs drawn inside the round icon (r = icon bounds)
static void DrawActionGlyph(QPainter& p, int action, const QRectF& r)
{
    const qreal s = r.width();
    const QPointF c = r.center();
    QPen pen(Qt::white, s * 0.06, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (action)
    {
    case kActionPokemon: // Poké Ball
    {
        QRectF ball(c.x() - s * 0.27, c.y() - s * 0.27, s * 0.54, s * 0.54);
        p.setBrush(Qt::white);
        p.drawPie(ball, 0, 180 * 16);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ball);
        p.drawLine(QPointF(ball.left(), c.y()), QPointF(ball.right(), c.y()));
        p.setBrush(ActionColor(action));
        p.drawEllipse(c, s * 0.08, s * 0.08);
        break;
    }
    case kActionPokedex: // device with a screen
    {
        QRectF body(c.x() - s * 0.2, c.y() - s * 0.26, s * 0.4, s * 0.52);
        p.drawRoundedRect(body, s * 0.05, s * 0.05);
        p.setBrush(Qt::white);
        p.drawRect(QRectF(body.left() + s * 0.07, body.top() + s * 0.08, s * 0.26, s * 0.16));
        p.drawEllipse(QPointF(c.x(), body.bottom() - s * 0.11), s * 0.04, s * 0.04);
        break;
    }
    case kActionBag:
    {
        QPainterPath bag;
        bag.addRoundedRect(QRectF(c.x() - s * 0.24, c.y() - s * 0.12, s * 0.48, s * 0.38), s * 0.08, s * 0.08);
        p.drawPath(bag);
        p.drawArc(QRectF(c.x() - s * 0.12, c.y() - s * 0.28, s * 0.24, s * 0.3), 0, 180 * 16);
        p.drawLine(QPointF(c.x() - s * 0.24, c.y() + s * 0.02), QPointF(c.x() + s * 0.24, c.y() + s * 0.02));
        break;
    }
    case kActionTrainerCard:
    {
        QRectF card(c.x() - s * 0.3, c.y() - s * 0.19, s * 0.6, s * 0.38);
        p.drawRoundedRect(card, s * 0.05, s * 0.05);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(card.left() + s * 0.17, c.y() - s * 0.03), s * 0.06, s * 0.06);
        p.drawPie(QRectF(card.left() + s * 0.07, c.y() + s * 0.04, s * 0.2, s * 0.16), 0, 180 * 16);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(c.x() + s * 0.02, c.y() - s * 0.06), QPointF(card.right() - s * 0.07, c.y() - s * 0.06));
        p.drawLine(QPointF(c.x() + s * 0.02, c.y() + s * 0.07), QPointF(card.right() - s * 0.12, c.y() + s * 0.07));
        break;
    }
    case kActionSave: // notebook
    {
        QRectF book(c.x() - s * 0.2, c.y() - s * 0.26, s * 0.4, s * 0.52);
        p.drawRoundedRect(book, s * 0.04, s * 0.04);
        p.drawLine(QPointF(book.left() + s * 0.1, book.top()), QPointF(book.left() + s * 0.1, book.bottom()));
        p.drawLine(QPointF(c.x(), c.y() - s * 0.08), QPointF(book.right() - s * 0.06, c.y() - s * 0.08));
        p.drawLine(QPointF(c.x(), c.y() + s * 0.04), QPointF(book.right() - s * 0.06, c.y() + s * 0.04));
        break;
    }
    case kActionOptions: // gear
    {
        for (int i = 0; i < 8; i++)
        {
            double a = i * M_PI / 4;
            p.drawLine(QPointF(c.x() + std::cos(a) * s * 0.2, c.y() + std::sin(a) * s * 0.2),
                       QPointF(c.x() + std::cos(a) * s * 0.29, c.y() + std::sin(a) * s * 0.29));
        }
        p.drawEllipse(c, s * 0.2, s * 0.2);
        p.drawEllipse(c, s * 0.07, s * 0.07);
        break;
    }
    case kActionPokegear:
    case kActionPokegearAlt: // round device
    {
        p.drawEllipse(c, s * 0.25, s * 0.25);
        p.drawLine(QPointF(c.x() - s * 0.25, c.y()), QPointF(c.x() + s * 0.25, c.y()));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(c.x(), c.y() + s * 0.12), s * 0.05, s * 0.05);
        break;
    }
    case kActionCemetery: // a gravestone
    {
        p.setBrush(Qt::NoBrush);
        QPainterPath stone;
        stone.moveTo(c.x() - s * 0.18, c.y() + s * 0.24);
        stone.lineTo(c.x() - s * 0.18, c.y() - s * 0.06);
        stone.arcTo(QRectF(c.x() - s * 0.18, c.y() - s * 0.24, s * 0.36, s * 0.36), 180, -180);
        stone.lineTo(c.x() + s * 0.18, c.y() + s * 0.24);
        stone.closeSubpath();
        p.drawPath(stone);
        p.drawLine(QPointF(c.x(), c.y() - s * 0.1), QPointF(c.x(), c.y() + s * 0.12));
        p.drawLine(QPointF(c.x() - s * 0.08, c.y() - s * 0.02), QPointF(c.x() + s * 0.08, c.y() - s * 0.02));
        break;
    }
    case kActionPlayers: // two people
    {
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(c.x() - s * 0.1, c.y() - s * 0.1), s * 0.08, s * 0.08);
        p.drawPie(QRectF(c.x() - s * 0.26, c.y() + s * 0.02, s * 0.32, s * 0.28), 0, 180 * 16);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(c.x() + s * 0.12, c.y() - s * 0.13), s * 0.07, s * 0.07);
        p.drawArc(QRectF(c.x() - s * 0.02, c.y() - s * 0.03, s * 0.28, s * 0.26), 0, 180 * 16);
        break;
    }
    default:
        p.drawLine(QPointF(c.x() - s * 0.15, c.y() - s * 0.15), QPointF(c.x() + s * 0.15, c.y() + s * 0.15));
        p.drawLine(QPointF(c.x() + s * 0.15, c.y() - s * 0.15), QPointF(c.x() - s * 0.15, c.y() + s * 0.15));
        break;
    }
}

static QString FormatMoney(u32 money)
{
    QString digits = QString::number(money);
    QString out;
    for (int i = 0; i < digits.size(); i++)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += QChar(0x202F); // narrow no-break space
        out += digits[i];
    }
    return out + QString::fromUtf8(" ₽");
}

// A rounded card with a colored stripe on its left edge: the stripe is clipped to the
// card's shape (no corner sticking out) and the outline is drawn last, over everything
static void DrawAccentCard(QPainter& p, const QRectF& box, qreal radius, const QBrush& fill, const QColor& accent, qreal stripeW, const QPen& outline)
{
    QPainterPath shape;
    shape.addRoundedRect(box, radius, radius);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(shape);
    p.setClipPath(shape, Qt::IntersectClip);
    p.setBrush(accent);
    p.drawRect(QRectF(box.left(), box.top(), stripeW, box.height()));
    p.restore();
    if (outline.style() != Qt::NoPen)
    {
        p.save();
        p.setPen(outline);
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
        p.restore();
    }
}

static void DrawButtonHint(QPainter& p, const QPointF& pos, qreal size, const QString& button, const QString& text, const QColor& color,
                           const QColor& textColor)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(pos.x(), pos.y(), size, size));
    QFont f("Segoe UI");
    f.setPixelSize((int)(size * 0.62));
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(pos.x(), pos.y(), size, size), Qt::AlignCenter, button);
    f.setBold(false);
    p.setFont(f);
    p.setPen(textColor);
    p.drawText(QRectF(pos.x() + size * 1.25, pos.y(), size * 6, size), Qt::AlignVCenter | Qt::AlignLeft, text);
}

void GameUi::draw(QImage& image, int width, int height)
{
    if (image.width() != width || image.height() != height)
        image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter p(&image);
    if (screen.type != kScreenMainMenu)
        clickTargets.clear();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    if (screen.type == kScreenParty)
        drawParty(p, width, height);
    else if (screen.type == kScreenBag)
        drawBag(p, width, height);
    else if (screen.type == kScreenSummary)
        drawSummary(p, width, height);
    else if (screen.type == kScreenBattle)
        drawBattle(p, width, height);
    else if (screen.type == kScreenBattleBag)
        drawBattleBag(p, width, height);
    else if (screen.type == kScreenBattleParty)
        drawBattleParty(p, width, height);
    else if (screen.type == kScreenOptions)
        drawOptions(p, width, height);
    else if (screen.type == kScreenTrainerCard)
        drawTrainerCard(p, width, height);
    else if (screen.type == kScreenPokedex)
        drawPokedex(p, width, height);
    else if (screen.type == kScreenPhone)
        drawPhone(p, width, height);
    else if (screen.type == kScreenGearMap)
        drawGearMap(p, width, height);
    else if (screen.type == kScreenRadio)
        drawRadio(p, width, height);
    else if (screen.type == kScreenMainMenu)
        drawMainMenu(p, width, height);
    else if (screen.type == kScreenStarter)
        drawStarter(p, width, height);
    else if (screen.type == kScreenNaming)
        drawNaming(p, width, height);
    else if (screen.type == kScreenLook)
        drawLook(p, width, height);
    else if (screen.type == kScreenShop)
        drawShop(p, width, height);
    if (screen.type == 0 && coverAlpha > 0.0f)
        p.fillRect(QRectF(0, 0, width, height), QColor(0, 0, 0, (int)(255 * coverAlpha)));
    drawNameplates(p, width, height);
    if (screen.type == kScreenBattle && nuzBattleCatch != 0)
        drawCatchBadge(p, width, height);
    if (state.type != 0)
        drawStartMenu(p, width, height);
    if (playersOpen)
        drawPlayers(p, width, height);
    if (cemeteryOpen)
        drawCemetery(p, width, height);
    if (playersOpen && playersAction >= 0)
        drawPlayersAction(p, width, height);
    if (giftOpen)
        drawGift(p, width, height);
    if (!giftOffers.isEmpty())
        drawGiftOffer(p, width, height);
    if (tradeUi.open)
        drawTrade(p, width, height);
    else if (tradeRequest.pending)
        drawTradeRequest(p, width, height);
    if (coopMode)
        drawChat(p, width, height);
    drawCoopStatus(p, width, height);
    if (nuzOver)
        drawNuzlockeOver(p, width, height);
    p.end();
}

// Nuzlocke, in a wild battle: can this Pokemon be caught?
void GameUi::drawCatchBadge(QPainter& p, int width, int height)
{
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    QString text;
    QColor color;
    switch (nuzBattleCatch)
    {
    case 1: text = QString::fromUtf8("✓  Capture possible"); color = QColor(70, 170, 100); break;
    case 2: text = QString::fromUtf8("✗  Déjà une rencontre ici"); color = QColor(200, 70, 70); break;
    case 3: text = QString::fromUtf8("Doublon : ne compte pas"); color = QColor(220, 150, 50); break;
    case 4: text = QString::fromUtf8("✦  Shiny : capture libre"); color = QColor(180, 120, 230); break;
    default: return;
    }
    QFont font("Segoe UI");
    font.setPixelSize((int)(26 * u));
    font.setBold(true);
    QFontMetrics fm(font);
    QRectF r(30 * u, 30 * u, fm.horizontalAdvance(text) + 60 * u, 54 * u);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(r.translated(0, 4 * u), 27 * u, 27 * u);
    p.setBrush(color);
    p.drawRoundedRect(r, 27 * u, 27 * u);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(r, Qt::AlignCenter, text);
    p.restore();
}

// Nuzlocke: the whole team fell
void GameUi::drawNuzlockeOver(QPainter& p, int width, int height)
{
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    p.save();
    p.fillRect(QRectF(0, 0, width, height), QColor(8, 4, 10, 235));
    QRadialGradient glow(QPointF(width / 2.0, height * 0.42), height * 0.6);
    glow.setColorAt(0, QColor(160, 30, 40, 90));
    glow.setColorAt(1, QColor(160, 30, 40, 0));
    p.fillRect(QRectF(0, 0, width, height), glow);
    QFont font("Segoe UI");
    font.setBold(true);
    font.setPixelSize((int)(80 * u));
    p.setFont(font);
    p.setPen(QColor(240, 90, 90));
    p.drawText(QRectF(0, height * 0.30, width, 110 * u), Qt::AlignCenter, QString::fromUtf8("Fin de la tentative"));
    font.setBold(false);
    font.setPixelSize((int)(34 * u));
    p.setFont(font);
    p.setPen(QColor(230, 220, 230));
    p.drawText(QRectF(0, height * 0.30 + 120 * u, width, 50 * u), Qt::AlignCenter, QString::fromUtf8("Toute ton équipe est tombée."));
    font.setPixelSize((int)(28 * u));
    p.setFont(font);
    p.setPen(QColor(180, 170, 190));
    p.drawText(QRectF(0, height * 0.30 + 180 * u, width, 50 * u), Qt::AlignCenter,
               QString::fromUtf8("Ho-Lympe prépare une nouvelle tentative, avec de nouveaux Pokémon…"));
    p.restore();
}

// Online players: one card per player (us first), with their place, team and badges
// Nuzlocke: the Pokemon fallen during this run (cemetery.json), over the menu
void GameUi::drawCemetery(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = height / 1080.0;
    p.fillRect(QRectF(0, 0, width, height), QColor(8, 6, 16, 170));

    const int visible = 5;
    const int count = (int)cemetery.size();
    const int first = std::max(0, std::min(cemeteryCursor - visible / 2, count - visible));
    const qreal panelW = 1100 * u, rowH = 118 * u, gap = 12 * u, headerH = 120 * u, footerH = 80 * u;
    const int shown = std::min(count, visible);
    const qreal panelH = headerH + (shown == 0 ? 140 * u : shown * (rowH + gap)) + footerH;
    QRectF panel((width - panelW) / 2, (height - panelH) / 2, panelW, panelH);
    QLinearGradient bg(panel.topLeft(), panel.bottomLeft());
    bg.setColorAt(0, QColor(44, 34, 56, 250));
    bg.setColorAt(1, QColor(24, 18, 32, 250));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRoundedRect(panel.translated(0, 10 * u), 40 * u, 40 * u);
    p.setBrush(bg);
    p.drawRoundedRect(panel, 40 * u, 40 * u);

    QFont title("Segoe UI");
    title.setPixelSize((int)(42 * u));
    title.setBold(true);
    p.setFont(title);
    p.setPen(QColor(240, 230, 245));
    p.drawText(QRectF(panel.left() + 48 * u, panel.top() + 28 * u, panelW, 64 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Cimetière"));
    QFont pillFont("Segoe UI");
    pillFont.setPixelSize((int)(26 * u));
    pillFont.setBold(true);
    QRectF pill(panel.right() - 48 * u - 200 * u, panel.top() + 36 * u, 200 * u, 48 * u);
    p.setBrush(QColor(255, 255, 255, 20));
    p.drawRoundedRect(pill, 24 * u, 24 * u);
    p.setFont(pillFont);
    p.setPen(QColor(240, 120, 130));
    p.drawText(pill, Qt::AlignCenter, QString::fromUtf8("☠ %1").arg(count));

    QFont nameFont("Segoe UI");
    nameFont.setPixelSize((int)(32 * u));
    nameFont.setBold(true);
    QFont small("Segoe UI");
    small.setPixelSize((int)(24 * u));
    if (count == 0)
    {
        p.setFont(small);
        p.setPen(QColor(190, 180, 200));
        p.drawText(QRectF(panel.left(), panel.top() + headerH, panelW, 140 * u), Qt::AlignCenter,
                   QString::fromUtf8("Personne n'est tombé pendant cette tentative. Pour l'instant…"));
    }
    for (int i = 0; i < shown; i++)
    {
        const QJsonObject& g = cemetery[first + i];
        bool selected = first + i == cemeteryCursor;
        QRectF card(panel.left() + 40 * u, panel.top() + headerH + i * (rowH + gap), panelW - 80 * u, rowH);
        p.setPen(selected ? QPen(QColor(240, 120, 130), 3 * u) : Qt::NoPen);
        p.setBrush(selected ? QColor(255, 255, 255, 30) : QColor(255, 255, 255, 14));
        p.drawRoundedRect(card, 24 * u, 24 * u);
        const QImage& icon = pokemonIcon(g.value("species").toInt(), false);
        if (!icon.isNull())
        {
            p.save();
            p.setOpacity(0.75); // a little faded
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRectF(card.left() + 20 * u, card.center().y() - 52 * u, 92 * u, 92 * u), icon.copy(0, 0, 32, 32));
            p.restore();
        }
        qreal tx = card.left() + 132 * u;
        p.setFont(nameFont);
        p.setPen(QColor(245, 240, 248));
        p.drawText(QRectF(tx, card.top() + 14 * u, card.width() - 360 * u, 46 * u), Qt::AlignVCenter | Qt::AlignLeft, g.value("name").toString());
        p.setFont(small);
        p.setPen(QColor(185, 175, 200));
        p.drawText(QRectF(tx, card.top() + 62 * u, card.width() - 360 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromUtf8("Tombé à %1").arg(g.value("location").toString()));
        p.setFont(nameFont);
        p.setPen(QColor(240, 200, 120));
        p.drawText(QRectF(card.right() - 230 * u, card.top() + 14 * u, 200 * u, 46 * u), Qt::AlignVCenter | Qt::AlignRight,
                   QString("N. %1").arg(g.value("level").toInt()));
        p.setFont(small);
        p.setPen(QColor(150, 140, 165));
        p.drawText(QRectF(card.right() - 330 * u, card.top() + 62 * u, 300 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight,
                   QDateTime::fromSecsSinceEpoch(g.value("date").toInteger()).toString("dd/MM  HH:mm"));
    }
    p.setFont(small);
    p.setPen(QColor(170, 160, 185));
    p.drawText(QRectF(panel.left(), panel.bottom() - footerH, panelW, footerH - 10 * u), Qt::AlignCenter,
               count > visible ? QString::fromUtf8("Haut / Bas pour parcourir  ·  B pour fermer") : QString::fromUtf8("B pour fermer"));
    p.restore();
}

void GameUi::drawPlayers(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = height / 1080.0;
    p.fillRect(QRectF(0, 0, width, height), QColor(10, 14, 28, 150));

    struct Row { QString name, location; QList<int> team; int badges; int slot; bool me; };
    QList<Row> rows;
    {
        QList<int> team;
        for (const QJsonValue& v : saveSummary.value("team").toArray())
            team.append(v.toInt());
        rows.append({ localName, locationName, team, saveSummary.value("badges").toInt(), coopMySlot, true });
    }
    for (const CoopPlayer& cp : coopPlayers)
        rows.append({ cp.name, cp.location, cp.team, cp.badges, cp.slot, false });

    const qreal panelW = 1180 * u, rowH = 132 * u, gap = 14 * u, headerH = 120 * u, footerH = 86 * u;
    const qreal panelH = headerH + rows.size() * (rowH + gap) + (rows.size() == 1 ? 90 * u : 0) + footerH;
    QRectF panel((width - panelW) / 2, (height - panelH) / 2, panelW, panelH);
    for (int i = 6; i > 0; i--)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 12));
        p.drawRoundedRect(panel.translated(0, 6 * u).adjusted(-i * 3 * u, -i * 3 * u, i * 3 * u, i * 3 * u), 44 * u, 44 * u);
    }
    QLinearGradient bg(panel.topLeft(), panel.bottomLeft());
    bg.setColorAt(0, QColor(250, 252, 255, 248));
    bg.setColorAt(1, QColor(232, 238, 246, 248));
    p.setBrush(bg);
    p.drawRoundedRect(panel, 40 * u, 40 * u);

    QFont title("Segoe UI");
    title.setPixelSize((int)(42 * u));
    title.setBold(true);
    p.setFont(title);
    p.setPen(QColor(40, 48, 66));
    p.drawText(QRectF(panel.left() + 48 * u, panel.top() + 28 * u, panelW, 64 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Joueurs en ligne"));
    QFont pillFont("Segoe UI");
    pillFont.setPixelSize((int)(26 * u));
    pillFont.setBold(true);
    QString count = QString::number(rows.size()) + " / 6";
    QRectF pill(panel.right() - 48 * u - 150 * u, panel.top() + 36 * u, 150 * u, 48 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 48, 66));
    p.drawRoundedRect(pill, 24 * u, 24 * u);
    p.setFont(pillFont);
    p.setPen(QColor(120, 220, 150));
    p.drawText(pill, Qt::AlignCenter, count);

    QFont nameFont("Segoe UI");
    nameFont.setPixelSize((int)(34 * u));
    nameFont.setBold(true);
    QFont small("Segoe UI");
    small.setPixelSize((int)(25 * u));
    QFont tag("Segoe UI");
    tag.setPixelSize((int)(20 * u));
    tag.setBold(true);
    int frame = (frameCount / 16) % 2;
    for (int i = 0; i < rows.size(); i++)
    {
        const Row& r = rows[i];
        bool selected = i == playersCursor;
        QColor color = PlayerColor(r.slot);
        QRectF card(panel.left() + 40 * u, panel.top() + headerH + i * (rowH + gap), panelW - 80 * u, rowH);
        DrawAccentCard(p, card, 26 * u, selected ? QColor(255, 248, 228) : QColor(255, 255, 255), color, 12 * u,
                       selected ? QPen(QColor(255, 196, 60), 4 * u) : QPen(QColor(210, 218, 230), 2 * u));
        // Avatar: the player's initial on their color
        QRectF avatar(card.left() + 34 * u, card.center().y() - 40 * u, 80 * u, 80 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(avatar);
        p.setFont(nameFont);
        p.setPen(Qt::white);
        p.drawText(avatar, Qt::AlignCenter, r.name.left(1).toUpper());
        // Name, "toi", place
        qreal tx = avatar.right() + 26 * u;
        p.setFont(nameFont);
        p.setPen(QColor(30, 36, 52));
        QFontMetricsF fm(nameFont);
        p.drawText(QRectF(tx, card.top() + 18 * u, 420 * u, 48 * u), Qt::AlignVCenter | Qt::AlignLeft, r.name);
        if (r.me)
        {
            QRectF me(tx + fm.horizontalAdvance(r.name) + 14 * u, card.top() + 28 * u, 66 * u, 30 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(color.lighter(150));
            p.drawRoundedRect(me, 15 * u, 15 * u);
            p.setFont(tag);
            p.setPen(color.darker(200));
            p.drawText(me, Qt::AlignCenter, "Toi");
        }
        p.setFont(small);
        p.setPen(QColor(96, 104, 122));
        QString where = r.location.isEmpty() ? QString::fromUtf8("En route…") : r.location;
        p.drawText(QRectF(tx, card.top() + 70 * u, 460 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft, where);
        // Team
        qreal ix = card.left() + 500 * u;
        for (int t = 0; t < 6; t++)
        {
            QRectF slot(ix + t * 66 * u, card.center().y() - 30 * u, 60 * u, 60 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(232, 238, 246));
            p.drawEllipse(slot);
            if (t < r.team.size())
            {
                const QImage& icon = pokemonIcon(r.team[t], false);
                if (!icon.isNull())
                {
                    p.save();
                    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                    p.drawImage(slot.adjusted(-6 * u, -10 * u, 6 * u, 2 * u), icon.copy(0, frame * 32, 32, 32));
                    p.restore();
                }
            }
        }
        // Badges
        QRectF badges(card.right() - 120 * u, card.center().y() - 24 * u, 96 * u, 48 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 48, 66));
        p.drawRoundedRect(badges, 24 * u, 24 * u);
        p.setFont(tag);
        p.setPen(QColor(255, 214, 90));
        p.drawText(badges, Qt::AlignCenter, QString::fromUtf8("◆ %1").arg(r.badges));
    }
    if (rows.size() == 1)
    {
        p.setFont(small);
        p.setPen(QColor(110, 118, 136));
        p.drawText(QRectF(panel.left(), panel.top() + headerH + rowH + gap, panelW, 80 * u), Qt::AlignCenter,
                   QString::fromUtf8("Personne d'autre n'est en ligne pour l'instant."));
    }
    DrawButtonHint(p, QPointF(panel.left() + 48 * u, panel.bottom() - footerH + 20 * u), 46 * u, "B", "Retour", QColor(220, 80, 80));
    if (playersCursor > 0)
        DrawButtonHint(p, QPointF(panel.left() + 300 * u, panel.bottom() - footerH + 20 * u), 46 * u, "A", QString::fromUtf8("Écrire / Offrir / Échanger"), QColor(76, 176, 120));
    p.restore();
}

void GameUi::drawStartMenu(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = height / 1080.0; // design units: 1080p
    const qreal t = state.open;
    const qreal ease = 1.0 - (1.0 - t) * (1.0 - t);

    // Dim the game slightly
    p.fillRect(QRectF(0, 0, width, height), QColor(10, 20, 40, (int)(70 * t)));

    std::vector<int> visible = visibleEntries();
    int count = (int)visible.size();
    const int columns = MenuColumns(count);
    int rows = std::max(1, (count + columns - 1) / columns);

    const qreal headerH = 110 * u;
    const qreal footerH = 90 * u;
    // Cells as big as before, smaller if the rows wouldn't fit in the screen's height
    const qreal cell = std::min<qreal>(190 * u, (height * 0.92 - headerH - footerH) / rows - 20 * u);
    const qreal panelW = columns * cell + 80 * u;
    const qreal panelH = headerH + rows * (cell + 20 * u) + footerH;
    const qreal panelX = width - panelW - 60 * u + (1.0 - ease) * (panelW + 80 * u);
    const qreal panelY = (height - panelH) / 2;

    p.setOpacity(t);

    // Panel with a soft shadow
    QRectF panel(panelX, panelY, panelW, panelH);
    for (int i = 6; i > 0; i--)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 10));
        p.drawRoundedRect(panel.translated(0, 6 * u).adjusted(-i * 3 * u, -i * 3 * u, i * 3 * u, i * 3 * u), 44 * u, 44 * u);
    }
    QLinearGradient bg(panel.topLeft(), panel.bottomLeft());
    bg.setColorAt(0, QColor(250, 252, 255, 245));
    bg.setColorAt(1, QColor(232, 238, 246, 245));
    p.setBrush(bg);
    p.drawRoundedRect(panel, 40 * u, 40 * u);

    // Header: title and money
    QFont titleFont("Segoe UI");
    titleFont.setPixelSize((int)(40 * u));
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(QColor(40, 48, 66));
    p.drawText(QRectF(panelX + 44 * u, panelY + 24 * u, panelW, 60 * u), Qt::AlignVCenter | Qt::AlignLeft, "Menu");
    QFont moneyFont("Segoe UI");
    moneyFont.setPixelSize((int)(30 * u));
    moneyFont.setBold(true);
    p.setFont(moneyFont);
    QRectF moneyPill(panelX + panelW - 44 * u - 210 * u, panelY + 30 * u, 210 * u, 50 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 48, 66));
    p.drawRoundedRect(moneyPill, 25 * u, 25 * u);
    p.setPen(QColor(255, 214, 90));
    p.drawText(moneyPill, Qt::AlignCenter, FormatMoney(state.money));

    // Entries: round icons in a grid
    QFont labelFont("Segoe UI");
    labelFont.setPixelSize((int)(30 * u));
    for (int i = 0; i < count; i++)
    {
        int entry = visible[i];
        int action = state.entries[entry];
        bool selected = entry == state.cursor;
        qreal cx = panelX + 40 * u + (i % columns) * cell + cell / 2;
        qreal cy = panelY + headerH + (i / columns) * (cell + 20 * u) + cell * 0.42;
        qreal radius = (selected ? 66 : 58) * u * cell / (190 * u);
        QRectF icon(cx - radius, cy - radius, radius * 2, radius * 2);

        if (selected)
        {
            QRadialGradient glow(QPointF(cx, cy), radius * 1.45);
            glow.setColorAt(0.6, QColor(255, 196, 60, 160));
            glow.setColorAt(1.0, QColor(255, 196, 60, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(QPointF(cx, cy), radius * 1.45, radius * 1.45);
        }

        QColor base = ActionColor(action);
        QRadialGradient fill(QPointF(cx - radius * 0.3, cy - radius * 0.4), radius * 1.6);
        fill.setColorAt(0, base.lighter(135));
        fill.setColorAt(1, base.darker(115));
        p.setPen(selected ? QPen(QColor(255, 255, 255), 6 * u) : QPen(QColor(255, 255, 255, 200), 3 * u));
        p.setBrush(fill);
        p.drawEllipse(icon);
        DrawActionGlyph(p, action, icon);

        labelFont.setBold(selected);
        p.setFont(labelFont);
        p.setPen(selected ? QColor(30, 36, 52) : QColor(80, 88, 104));
        p.drawText(QRectF(cx - cell / 2, cy + cell * 0.37, cell, 44 * u), Qt::AlignCenter, ActionLabel(action));
    }

    // Footer: button hints
    qreal hintY = panelY + panelH - footerH + 22 * u;
    DrawButtonHint(p, QPointF(panelX + 44 * u, hintY), 46 * u, "A", "Choisir", QColor(80, 170, 90));
    DrawButtonHint(p, QPointF(panelX + 44 * u + 230 * u, hintY), 46 * u, "B", "Retour", QColor(220, 80, 80));
    p.restore();
}

// ---------------------------------------------------------------------------
// Game text
// ---------------------------------------------------------------------------

static const std::unordered_map<u16, const char*>& GameCharmap()
{
    static const struct { u16 code; const char* text; } kTable[] = {
#include "GameCharmap.inc"
    };
    static std::unordered_map<u16, const char*> map;
    if (map.empty())
    {
        for (const auto& entry : kTable)
            map[entry.code] = entry.text;
    }
    return map;
}

QString DecodeGameText(const u16* text, int maxLen)
{
    const auto& map = GameCharmap();
    QString out;
    for (int i = 0; i < maxLen; i++)
    {
        u16 c = text[i];
        if (c == 0xFFFF)
            break;
        if (c == 0xFFFE)
        {
            // Control code: command, number of parameters, parameters
            if (i + 2 >= maxLen)
                break;
            i += 2 + text[i + 2];
            continue;
        }
        if (c == 0xE000 || c == 0x25BC || c == 0x25BD)
        {
            out += ' ';
            continue;
        }
        auto it = map.find(c);
        if (it != map.end())
            out += QString::fromUtf8(it->second);
    }
    return out.simplified();
}

bool EncodeGameText(const QString& text, u16* out, int maxLen)
{
    // Unicode -> game code; Latin characters exist twice (Japanese full width and Western),
    // the Western ones (0x0100 and above) are used
    static std::unordered_map<char16_t, u16> reverse;
    if (reverse.empty())
    {
        for (const auto& entry : GameCharmap())
        {
            QString s = QString::fromUtf8(entry.second);
            if (s.size() != 1)
                continue;
            auto it = reverse.find(s[0].unicode());
            if (it == reverse.end() || (entry.first >= 0x0100 && it->second < 0x0100))
                reverse[s[0].unicode()] = entry.first;
        }
    }
    int n = 0;
    for (QChar c : text)
    {
        if (n >= maxLen)
            break;
        auto it = reverse.find(c.unicode());
        if (it == reverse.end())
            return false;
        out[n++] = it->second;
    }
    out[n] = 0xFFFF;
    return true;
}

// ---------------------------------------------------------------------------
// Screens mirrored from the game
// ---------------------------------------------------------------------------

void GameUi::updateScreen(u8* mailbox)
{
    const u8* src = mailbox + kScrOffset;
    u32 frame = src[kScrFrame] | (src[kScrFrame + 1] << 8) | (src[kScrFrame + 2] << 16) | ((u32)src[kScrFrame + 3] << 24);
    int type = src[kScrType];

    // A screen that stops updating is gone (e.g. its app ended without clearing it)
    if (frame == screen.lastFrame)
        screen.staleFrames++;
    else
        screen.staleFrames = 0;
    screen.lastFrame = frame;
    int staleLimit = 20;
    if (type == kScreenBattle)
    {
        // A battle menu stops right away (a move chosen: its animation shows), except the main
        // menu's Attaque / Sac / Pokémon: the next menu comes after the button's animation or a
        // fade, this one stays meanwhile (no glimpse of the DS menu)
        int menuId = src[1], cursor = src[2], battleType = src[4];
        bool toSubmenu = (menuId == 1 || menuId == 2) && battleType == 0 && cursor <= 2;
        staleLimit = toSubmenu ? 90 : 2;
    }
    if (screen.staleFrames > staleLimit)
        type = 0;

    bool changed = type != screen.type
        || memcmp(screen.raw, src, kScrFrame) != 0
        || memcmp(screen.raw + kScrMessage, src + kScrMessage, kScreenSize - kScrMessage) != 0;
    memcpy(screen.raw, src, kScreenSize);
    screen.type = type;
    if (type != 0 && (++screen.animFrame % 8) == 0)
        changed = true; // icon animation
    if (changed)
        dirty = true;
}

const QImage& GameUi::pokemonIcon(int species, bool isEgg)
{
    int index = isEgg ? 501 : (species > 0 && species <= 493 ? species + 7 : 7);
    auto it = iconCache.find(index);
    if (it == iconCache.end())
    {
        QString path = HoLympeAssetsDir() + QString("/pokemon_icons/%1.png").arg(index);
        it = iconCache.emplace(index, QImage(path)).first;
    }
    return it->second;
}

static u16 ReadU16(const u8* p)
{
    return p[0] | (p[1] << 8);
}

static QString StatusLabel(int status, QColor& color)
{
    switch (status)
    {
    case 1: color = QColor(232, 196, 40); return "PAR";
    case 2: color = QColor(110, 190, 230); return "GEL";
    case 3: color = QColor(150, 140, 170); return "SOM";
    case 4: color = QColor(170, 90, 190); return "PSN";
    case 5: color = QColor(232, 110, 60); return "BRL";
    case 6: color = QColor(200, 60, 60); return "K.O.";
    default: return QString();
    }
}

static QColor HpColor(int hp, int maxHp)
{
    if (maxHp <= 0 || hp * 2 > maxHp)
        return QColor(76, 205, 110);
    if (hp * 5 > maxHp)
        return QColor(245, 196, 50);
    return QColor(235, 80, 70);
}

void GameUi::drawParty(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int cursor = scr[kScrCursor];
    const bool menuOpen = scr[kScrMenuOpen] != 0;
    QFont font("Segoe UI");

    // Background: deep teal with soft diagonal stripes
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(28, 120, 150));
    bg.setColorAt(1, QColor(18, 70, 110));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    // Layout: 2 columns x 3 rows of cards, the action menu on the right when open
    const qreal margin = 90 * u;
    const qreal gap = 30 * u;
    const qreal menuW = 380 * u;
    const qreal cardW = std::min<qreal>(760 * u, (width - margin * 2 - gap - (menuOpen ? menuW + gap : 0)) / 2);
    const qreal cardH = 190 * u;
    const qreal gridW = cardW * 2 + gap;
    const qreal gridX = menuOpen ? margin : (width - gridW) / 2;
    const qreal gridY = 150 * u;

    // Title
    font.setPixelSize((int)(52 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(gridX, 40 * u, 800 * u, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Équipe"));

    for (int slot = 0; slot < 6; slot++)
    {
        const u8* mon = scr + kScrMons + slot * kMonSize;
        QRectF card(gridX + (slot % 2) * (cardW + gap), gridY + (slot / 2) * (cardH + gap), cardW, cardH);
        bool selected = slot == cursor;

        if (!mon[15]) // empty slot
        {
            p.setPen(QPen(QColor(255, 255, 255, 70), 3 * u, Qt::DashLine));
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRoundedRect(card, 34 * u, 34 * u);
            continue;
        }

        int species = ReadU16(mon), level = ReadU16(mon + 2), hp = ReadU16(mon + 4), maxHp = ReadU16(mon + 6);
        int heldItem = ReadU16(mon + 8), status = ReadU16(mon + 10), gender = mon[12];
        bool isEgg = mon[13] != 0;
        QString name = DecodeGameText((const u16*)(mon + 16), 12);

        if (selected)
        {
            card.translate(0, -6 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 196, 60, 110));
            p.drawRoundedRect(card.adjusted(-10 * u, -10 * u, 10 * u, 10 * u), 42 * u, 42 * u);
        }
        QLinearGradient cardBg(card.topLeft(), card.bottomLeft());
        cardBg.setColorAt(0, QColor(255, 255, 255, 250));
        cardBg.setColorAt(1, QColor(228, 236, 244, 250));
        p.setPen(selected ? QPen(QColor(255, 170, 40), 6 * u) : QPen(QColor(255, 255, 255, 120), 2 * u));
        p.setBrush(cardBg);
        p.drawRoundedRect(card, 34 * u, 34 * u);

        // Icon in a circle (animated like in the game)
        QRectF circle(card.left() + 24 * u, card.center().y() - 70 * u, 140 * u, 140 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(selected ? QColor(255, 226, 150) : QColor(214, 228, 240));
        p.drawEllipse(circle);
        const QImage& icon = pokemonIcon(species, isEgg);
        if (!icon.isNull())
        {
            int frame = (screen.animFrame / (selected ? 8 : 16)) % 2;
            QImage sprite = icon.copy(0, frame * 32, 32, 32);
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRectF(circle.center().x() - 64 * u, circle.center().y() - 72 * u, 128 * u, 128 * u), sprite);
            p.restore();
        }

        // Line 1: name and gender
        const qreal textX = circle.right() + 28 * u;
        const qreal textW = card.right() - textX - 30 * u;
        font.setPixelSize((int)(36 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        QString shownName = p.fontMetrics().elidedText(name, Qt::ElideRight, (int)(textW - 50 * u));
        QRectF nameRect(textX, card.top() + 20 * u, textW, 50 * u);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, shownName);
        qreal nameW = p.fontMetrics().horizontalAdvance(shownName);
        if (!isEgg && gender < 2)
        {
            p.setPen(gender == 0 ? QColor(60, 130, 230) : QColor(230, 80, 120));
            p.drawText(QRectF(textX + nameW + 10 * u, nameRect.top(), 50 * u, nameRect.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       gender == 0 ? QString::fromUtf8("♂") : QString::fromUtf8("♀"));
        }
        if (isEgg)
            continue;

        // Line 2: HP bar
        QRectF bar(textX, card.top() + 84 * u, textW, 22 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 66, 80));
        p.drawRoundedRect(bar, 11 * u, 11 * u);
        qreal ratio = maxHp > 0 ? (qreal)hp / maxHp : 0;
        if (ratio > 0)
        {
            p.setBrush(HpColor(hp, maxHp));
            p.drawRoundedRect(QRectF(bar.left() + 3 * u, bar.top() + 3 * u, (bar.width() - 6 * u) * ratio, bar.height() - 6 * u), 8 * u, 8 * u);
        }

        // Line 3: level, status and held item badges, HP numbers
        QRectF line3(textX, bar.bottom() + 14 * u, textW, 40 * u);
        font.setPixelSize((int)(28 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(70, 80, 100));
        QString levelText = QString("N. %1").arg(level);
        p.drawText(line3, Qt::AlignVCenter | Qt::AlignLeft, levelText);
        qreal badgeX = textX + p.fontMetrics().horizontalAdvance(levelText) + 16 * u;
        font.setBold(false);
        p.setFont(font);
        p.drawText(line3, Qt::AlignVCenter | Qt::AlignRight, QString("%1 / %2").arg(hp).arg(maxHp));

        QColor statusColor;
        QString statusText = StatusLabel(status, statusColor);
        font.setPixelSize((int)(22 * u));
        font.setBold(true);
        p.setFont(font);
        if (!statusText.isEmpty())
        {
            QRectF badge(badgeX, line3.top() + 4 * u, 80 * u, 32 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(statusColor);
            p.drawRoundedRect(badge, 16 * u, 16 * u);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, statusText);
            badgeX += 92 * u;
        }
        if (heldItem != 0)
        {
            // Small item marker (a bag), like the game's held item icon
            QRectF badge(badgeX, line3.top() + 4 * u, 32 * u, 32 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(222, 150, 58));
            p.drawEllipse(badge);
            p.setPen(QPen(Qt::white, 3 * u));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(badge.adjusted(8 * u, 11 * u, -8 * u, -7 * u), 3 * u, 3 * u);
            p.drawArc(badge.adjusted(11 * u, 6 * u, -11 * u, -16 * u), 0, 180 * 16);
        }
    }

    // Back button
    font.setPixelSize((int)(34 * u));
    font.setBold(true);
    p.setFont(font);
    const qreal bottomY = gridY + 3 * (cardH + gap) + 10 * u;
    QRectF back(gridX + gridW - 260 * u, bottomY, 260 * u, 76 * u);
    bool backSelected = cursor >= 6;
    p.setPen(backSelected ? QPen(QColor(255, 170, 40), 6 * u) : QPen(Qt::NoPen));
    p.setBrush(backSelected ? QColor(255, 236, 190) : QColor(255, 255, 255, 220));
    p.drawRoundedRect(back, 38 * u, 38 * u);
    p.setPen(QColor(40, 48, 66));
    p.drawText(back, Qt::AlignCenter, "Retour");

    // Message: prompt next to the back button, dialog box at the bottom
    int messageKind = scr[kScrMessageKind];
    if (messageKind != 0)
    {
        QString message = DecodeGameText((const u16*)(scr + kScrMessage), kTextLen);
        QRectF box(gridX, bottomY, gridW - 290 * u, 76 * u);
        if (messageKind == 2)
            box = QRectF(gridX, height - 230 * u, gridW, 170 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 240));
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(34 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(box.adjusted(36 * u, 0, -36 * u, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, message);
    }

    // Action menu
    if (menuOpen)
    {
        int count = std::min<int>(scr[kScrMenuCount], 8);
        qreal itemH = 82 * u;
        QRectF panel(width - margin - menuW, gridY, menuW, count * (itemH + 14 * u) + 40 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(250, 252, 255, 245));
        p.drawRoundedRect(panel, 34 * u, 34 * u);
        font.setPixelSize((int)(32 * u));
        for (int i = 0; i < count; i++)
        {
            bool sel = i == scr[kScrMenuCursor];
            QRectF item(panel.left() + 20 * u, panel.top() + 20 * u + i * (itemH + 14 * u), panel.width() - 40 * u, itemH);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QColor(255, 196, 60) : QColor(226, 234, 244));
            p.drawRoundedRect(item, itemH / 2, itemH / 2);
            font.setBold(sel);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(item, Qt::AlignCenter, DecodeGameText((const u16*)(scr + kScrMenuItems + i * kMenuItemLen * 2), kMenuItemLen));
        }
    }

    // Button hints
    if (messageKind != 2)
    {
        qreal hintY = height - 70 * u;
        DrawButtonHint(p, QPointF(gridX, hintY), 44 * u, "A", "Choisir", QColor(80, 170, 90), Qt::white);
        DrawButtonHint(p, QPointF(gridX + 230 * u, hintY), 44 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Bag (CoopUiBagScreen)
// ---------------------------------------------------------------------------

enum
{
    kBagPocket = 1,
    kBagItemCount = 2,
    kBagSelected = 3,
    kBagMenuOpen = 4,
    kBagMenuCount = 5,
    kBagMenuCursor = 6,
    kBagMessageKind = 7,
    kBagFirstRow = 12,
    kBagRowCount = 13,
    kBagQuantityShown = 14,
    kBagYesNoShown = 15,
    kBagQuantity = 16,
    kBagYesNoCursor = 18,
    kBagDescription = 28,  // u16[96]
    kBagMessage = 220,     // u16[96]
    kBagMenuItems = 412,   // u16[5][12]
    kBagRows = 532,        // 12 x {u16 itemId, u16 quantity, u16 name[16]}
    kBagRowSize = 36,
    kBagNameLen = 16,
    kBagMenuLen = 12,
};

static const char* PocketName(int pocket)
{
    static const char* kNames[] = { "Objets", "Soins", "Balls", "CT & CS", "Baies", "Lettres", "Combat", "Objets rares" };
    return pocket >= 0 && pocket < 8 ? kNames[pocket] : "";
}

static QColor PocketColor(int pocket)
{
    static const QColor kColors[] = {
        QColor(232, 120, 60), QColor(80, 180, 110), QColor(220, 70, 70), QColor(90, 120, 220),
        QColor(150, 190, 60), QColor(110, 170, 210), QColor(210, 90, 160), QColor(200, 160, 50),
    };
    return pocket >= 0 && pocket < 8 ? kColors[pocket] : QColor(150, 150, 150);
}

const QImage& GameUi::itemIcon(int itemId)
{
    auto it = itemIconCache.find(itemId);
    if (it == itemIconCache.end())
    {
        QString path = HoLympeAssetsDir() + QString("/item_icons/%1.png").arg(itemId);
        it = itemIconCache.emplace(itemId, QImage(path)).first;
    }
    return it->second;
}

static QString TmLabel(int itemId)
{
    if (itemId >= 328 && itemId < 420)
        return QString("CT%1").arg(itemId - 328 + 1, 2, 10, QChar('0'));
    if (itemId >= 420 && itemId < 428)
        return QString("CS%1").arg(itemId - 420 + 1, 2, 10, QChar('0'));
    return QString();
}

// The Poke Mart (CoopUiShopScreen): the list of items for sale, the chosen one, how many, yes / no
enum
{
    kShopMode = 1, kShopCount = 2, kShopSelected = 3, kShopFirstRow = 4, kShopRowCount = 5, kShopPoints = 6, kShopResult = 7,
    kShopMoney = 12, kShopQuantity = 16, kShopMaxQuantity = 18, kShopDescription = 20, kShopMessage = 212,
    kShopRows = 404, kShopRowSize = 34, kShopNameLen = 14, kShopMaxRows = 16,
};

void GameUi::drawShop(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int mode = scr[kShopMode];
    const int count = scr[kShopCount];
    const int selected = scr[kShopSelected];
    const int firstRow = scr[kShopFirstRow];
    const int rowCount = std::min<int>(scr[kShopRowCount], kShopMaxRows);
    const bool points = scr[kShopPoints] != 0;
    const u32 money = scr[kShopMoney] | (scr[kShopMoney + 1] << 8) | (scr[kShopMoney + 2] << 16) | ((u32)scr[kShopMoney + 3] << 24);
    const QColor accent(52, 120, 220);
    auto price = [&](u32 amount) { return points ? QString::fromUtf8("%1 PA").arg(amount) : FormatMoney(amount); };
    QFont font("Segoe UI");

    // Background: the Poke Mart's blue
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(40, 100, 200));
    bg.setColorAt(1, QColor(16, 40, 96));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 120 * u, 1760 * u);
    const qreal left = (width - contentW) / 2;

    // Title and money
    font.setPixelSize((int)(52 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, 36 * u, 600 * u, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Boutique"));
    QRectF purse(left + contentW - 380 * u, 44 * u, 380 * u, 66 * u);
    p.setBrush(QColor(20, 28, 48, 220));
    p.drawRoundedRect(purse, 33 * u, 33 * u);
    font.setPixelSize((int)(36 * u));
    p.setFont(font);
    p.setPen(QColor(255, 214, 90));
    p.drawText(purse, Qt::AlignCenter, price(money));

    // The items for sale
    const qreal listW = contentW * 0.56;
    const qreal listTop = 150 * u;
    const qreal rowH = 46 * u;
    QRectF listPanel(left, listTop, listW, rowH * kShopMaxRows + 40 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255, 240));
    p.drawRoundedRect(listPanel, 34 * u, 34 * u);
    for (int i = 0; i < rowCount; i++)
    {
        const u8* row = scr + kShopRows + i * kShopRowSize;
        int itemId = ReadU16(row), itemPrice = ReadU16(row + 2), owned = ReadU16(row + 4);
        bool sel = firstRow + i == selected;
        QRectF r(listPanel.left() + 20 * u, listPanel.top() + 20 * u + i * rowH, listPanel.width() - 40 * u, rowH - 4 * u);
        if (sel)
        {
            p.setPen(QPen(accent.darker(110), 4 * u));
            p.setBrush(accent.lighter(175));
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        const QImage& icon = itemIcon(itemId);
        if (!icon.isNull())
        {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRectF(r.left() + 14 * u, r.center().y() - 20 * u, 40 * u, 40 * u), icon);
            p.restore();
        }
        QString name = DecodeGameText((const u16*)(row + 6), kShopNameLen);
        QString tm = TmLabel(itemId);
        if (!tm.isEmpty())
            name = tm + "  " + name;
        font.setPixelSize((int)(28 * u));
        font.setBold(sel);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(r.adjusted(70 * u, 0, -330 * u, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
        p.drawText(r.adjusted(0, 0, -120 * u, 0), Qt::AlignVCenter | Qt::AlignRight, price(itemPrice));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(120, 128, 144));
        p.drawText(r.adjusted(0, 0, -24 * u, 0), Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("× %1").arg(owned));
    }
    if (count > kShopMaxRows)
    {
        qreal trackH = listPanel.height() - 60 * u;
        qreal thumbH = std::max<qreal>(40 * u, trackH * kShopMaxRows / count);
        qreal thumbY = listPanel.top() + 30 * u + (trackH - thumbH) * firstRow / std::max(1, count - kShopMaxRows);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 40));
        p.drawRoundedRect(QRectF(listPanel.right() - 14 * u, listPanel.top() + 30 * u, 6 * u, trackH), 3 * u, 3 * u);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(listPanel.right() - 15 * u, thumbY, 8 * u, thumbH), 4 * u, 4 * u);
    }

    // The chosen item: icon, name, what it does, how many the player has
    QRectF detail(left + listW + 40 * u, listTop, contentW - listW - 40 * u, listPanel.height());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255, 240));
    p.drawRoundedRect(detail, 34 * u, 34 * u);
    int selPrice = 0, selOwned = 0;
    QString selName;
    if (count > 0 && selected >= firstRow && selected < firstRow + rowCount)
    {
        const u8* row = scr + kShopRows + (selected - firstRow) * kShopRowSize;
        int itemId = ReadU16(row);
        selPrice = ReadU16(row + 2);
        selOwned = ReadU16(row + 4);
        selName = DecodeGameText((const u16*)(row + 6), kShopNameLen);
        QRectF circle(detail.center().x() - 100 * u, detail.top() + 36 * u, 200 * u, 200 * u);
        p.setBrush(accent.lighter(180));
        p.drawEllipse(circle);
        const QImage& icon = itemIcon(itemId);
        if (!icon.isNull())
        {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(circle.adjusted(28 * u, 28 * u, -28 * u, -28 * u), icon);
            p.restore();
        }
        QString name = DecodeGameText((const u16*)(row + 6), kShopNameLen);
        QString tm = TmLabel(itemId);
        font.setPixelSize((int)(40 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(detail.left() + 30 * u, circle.bottom() + 16 * u, detail.width() - 60 * u, 60 * u), Qt::AlignCenter,
                   tm.isEmpty() ? name : tm + "  " + name);
        font.setPixelSize((int)(32 * u));
        p.setFont(font);
        p.setPen(accent.darker(130));
        p.drawText(QRectF(detail.left() + 30 * u, circle.bottom() + 76 * u, detail.width() - 60 * u, 46 * u), Qt::AlignCenter,
                   QString::fromUtf8("%1   ·   tu en as %2").arg(price(selPrice)).arg(selOwned));
        if (mode == 0)
        {
            font.setPixelSize((int)(30 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(70, 80, 100));
            p.drawText(QRectF(detail.left() + 44 * u, circle.bottom() + 140 * u, detail.width() - 88 * u, detail.bottom() - circle.bottom() - 170 * u),
                       Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, DecodeGameText((const u16*)(scr + kShopDescription), kTextLen));
        }
    }

    // How many: the quantity and the total
    if (mode == 1)
    {
        int quantity = ReadU16(scr + kShopQuantity);
        QRectF box(detail.left() + 40 * u, detail.bottom() - 330 * u, detail.width() - 80 * u, 290 * u);
        p.setPen(QPen(accent, 5 * u));
        p.setBrush(Qt::white);
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(30 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(70, 80, 100));
        p.drawText(QRectF(box.left(), box.top() + 18 * u, box.width(), 44 * u), Qt::AlignCenter, QString::fromUtf8("Combien ?"));
        font.setPixelSize((int)(64 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(box.left(), box.top() + 66 * u, box.width(), 90 * u), Qt::AlignCenter, QString::fromUtf8("▼   × %1   ▲").arg(quantity));
        font.setPixelSize((int)(38 * u));
        p.setFont(font);
        p.setPen(accent.darker(130));
        p.drawText(QRectF(box.left(), box.top() + 160 * u, box.width(), 56 * u), Qt::AlignCenter,
                   QString::fromUtf8("Total : %1").arg(price((u32)selPrice * quantity)));
        font.setPixelSize((int)(24 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(120, 128, 144));
        p.drawText(QRectF(box.left(), box.top() + 222 * u, box.width(), 40 * u), Qt::AlignCenter,
                   QString::fromUtf8("▲▼ : 1 de plus / de moins   ·   ◀▶ : 10   ·   max %1").arg(ReadU16(scr + kShopMaxQuantity)));
    }

    // Message (the question, thanks...) and the buttons to use: the DS shop shows pictures on its
    // touch screen, the words are the launcher's
    const qreal bottomY = listPanel.bottom() + 24 * u;
    QString message;
    int quantity = ReadU16(scr + kShopQuantity);
    if (mode == 2)
        message = QString::fromUtf8("Acheter %1 × %2 pour %3 ?").arg(quantity).arg(selName).arg(price((u32)selPrice * quantity));
    else if (mode == 3)
    {
        switch (scr[kShopResult])
        {
        case 10: message = QString::fromUtf8("Tu n'as pas assez d'argent pour ça."); break;
        case 11: message = QString::fromUtf8("Tu n'as plus de place pour cet objet."); break;
        case 12: message = QString::fromUtf8("Ton sac est plein."); break;
        case 13: message = QString::fromUtf8("Et en cadeau : une Honor Ball !"); break;
        default: message = QString::fromUtf8("Merci pour ton achat !"); break;
        }
    }
    if (mode != 0 && !message.isEmpty())
    {
        QRectF box(left, bottomY, contentW - (mode == 2 ? 420 * u : 0), std::min<qreal>(120 * u, height - bottomY - 20 * u));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 245));
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(32 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(box.adjusted(40 * u, 0, -40 * u, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, message);
        if (mode == 3)
            DrawButtonHint(p, QPointF(box.right() - 170 * u, box.center().y() - 22 * u), 44 * u, "A", "OK", QColor(80, 170, 90), QColor(32, 40, 56));
    }
    if (mode == 2)
    {
        // Yes / No: A / B
        QRectF yes(left + contentW - 400 * u, bottomY, 190 * u, 120 * u), no(left + contentW - 190 * u, bottomY, 190 * u, 120 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(80, 170, 90));
        p.drawRoundedRect(yes, 30 * u, 30 * u);
        p.setBrush(QColor(220, 80, 80));
        p.drawRoundedRect(no, 30 * u, 30 * u);
        font.setPixelSize((int)(34 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(yes, Qt::AlignCenter, QString::fromUtf8("A  Oui"));
        p.drawText(no, Qt::AlignCenter, QString::fromUtf8("B  Non"));
    }
    else if (mode == 0 || message.isEmpty())
    {
        DrawButtonHint(p, QPointF(left, bottomY + 14 * u), 44 * u, "A", mode == 1 ? "Valider" : "Acheter", QColor(80, 170, 90), Qt::white);
        DrawButtonHint(p, QPointF(left + 260 * u, bottomY + 14 * u), 44 * u, "B", mode == 1 ? "Retour" : "Quitter", QColor(220, 80, 80), Qt::white);
        if (mode == 0)
        {
            DrawButtonHint(p, QPointF(left + 520 * u, bottomY + 14 * u), 44 * u, QString::fromUtf8("▲▼"), "Choisir", QColor(90, 110, 140), Qt::white);
            DrawButtonHint(p, QPointF(left + 780 * u, bottomY + 14 * u), 44 * u, QString::fromUtf8("◀▶"), QString::fromUtf8("6 par 6"), QColor(90, 110, 140), Qt::white);
        }
    }
    p.restore();
}

void GameUi::drawBag(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int pocket = scr[kBagPocket];
    const int itemCount = scr[kBagItemCount];
    const int selected = scr[kBagSelected];
    const int firstRow = scr[kBagFirstRow];
    const int rowCount = std::min<int>(scr[kBagRowCount], 12);
    const QColor accent = PocketColor(pocket);
    QFont font("Segoe UI");

    // Background in the pocket's color
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, accent.darker(140));
    bg.setColorAt(1, accent.darker(260));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 120 * u, 1760 * u);
    const qreal left = (width - contentW) / 2;

    // Title and pocket tabs
    font.setPixelSize((int)(52 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, 36 * u, 400 * u, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, "Sac");
    const qreal tabSize = 74 * u;
    const qreal tabsX = left + 200 * u;
    for (int i = 0; i < 8; i++)
    {
        QRectF tab(tabsX + i * (tabSize + 18 * u), 40 * u, tabSize, tabSize);
        bool current = i == pocket;
        p.setPen(current ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
        p.setBrush(current ? PocketColor(i) : PocketColor(i).darker(150));
        p.drawEllipse(tab);
        font.setPixelSize((int)(30 * u));
        p.setFont(font);
        p.setPen(QColor(255, 255, 255, current ? 255 : 150));
        p.drawText(tab, Qt::AlignCenter, QString::number(i + 1));
    }
    font.setPixelSize((int)(40 * u));
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(tabsX + 8 * (tabSize + 18 * u) + 20 * u, 40 * u, 500 * u, tabSize), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("◀  %1  ▶").arg(PocketName(pocket)));

    // Item list
    const qreal listW = contentW * 0.56;
    const qreal listTop = 150 * u;
    const qreal rowH = 60 * u;
    QRectF listPanel(left, listTop, listW, rowH * 12 + 40 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255, 240));
    p.drawRoundedRect(listPanel, 34 * u, 34 * u);
    if (itemCount == 0)
    {
        font.setPixelSize((int)(34 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(120, 128, 144));
        p.drawText(listPanel, Qt::AlignCenter, "Cette poche est vide.");
    }
    for (int i = 0; i < rowCount; i++)
    {
        const u8* row = scr + kBagRows + i * kBagRowSize;
        int itemId = ReadU16(row), quantity = ReadU16(row + 2);
        bool sel = firstRow + i == selected;
        QRectF r(listPanel.left() + 20 * u, listPanel.top() + 20 * u + i * rowH, listPanel.width() - 40 * u, rowH - 6 * u);
        if (sel)
        {
            p.setPen(QPen(accent.darker(110), 4 * u));
            p.setBrush(accent.lighter(170));
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        const QImage& icon = itemIcon(itemId);
        if (!icon.isNull())
        {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRectF(r.left() + 16 * u, r.center().y() - 26 * u, 52 * u, 52 * u), icon);
            p.restore();
        }
        QString name = DecodeGameText((const u16*)(row + 4), kBagNameLen);
        QString tm = TmLabel(itemId);
        if (!tm.isEmpty())
            name = tm + "  " + name;
        font.setPixelSize((int)(32 * u));
        font.setBold(sel);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(r.adjusted(90 * u, 0, -150 * u, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
        if (pocket != 7)
            p.drawText(r.adjusted(0, 0, -30 * u, 0), Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("× %1").arg(quantity));
    }
    // Scroll indicator
    if (itemCount > 12)
    {
        qreal trackH = listPanel.height() - 60 * u;
        qreal thumbH = std::max<qreal>(40 * u, trackH * 12 / itemCount);
        qreal thumbY = listPanel.top() + 30 * u + (trackH - thumbH) * firstRow / std::max(1, itemCount - 12);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 40));
        p.drawRoundedRect(QRectF(listPanel.right() - 14 * u, listPanel.top() + 30 * u, 6 * u, trackH), 3 * u, 3 * u);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(listPanel.right() - 15 * u, thumbY, 8 * u, thumbH), 4 * u, 4 * u);
    }

    // Detail panel: selected item and its description
    QRectF detail(left + listW + 40 * u, listTop, contentW - listW - 40 * u, listPanel.height());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255, 240));
    p.drawRoundedRect(detail, 34 * u, 34 * u);
    if (itemCount > 0 && selected >= firstRow && selected < firstRow + rowCount)
    {
        const u8* row = scr + kBagRows + (selected - firstRow) * kBagRowSize;
        int itemId = ReadU16(row);
        QRectF circle(detail.center().x() - 110 * u, detail.top() + 40 * u, 220 * u, 220 * u);
        p.setBrush(accent.lighter(175));
        p.drawEllipse(circle);
        const QImage& icon = itemIcon(itemId);
        if (!icon.isNull())
        {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(circle.adjusted(30 * u, 30 * u, -30 * u, -30 * u), icon);
            p.restore();
        }
        QString name = DecodeGameText((const u16*)(row + 4), kBagNameLen);
        QString tm = TmLabel(itemId);
        font.setPixelSize((int)(40 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(detail.left() + 30 * u, circle.bottom() + 20 * u, detail.width() - 60 * u, 60 * u), Qt::AlignCenter,
                   tm.isEmpty() ? name : tm + "  " + name);
        font.setPixelSize((int)(30 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(70, 80, 100));
        if (!scr[kBagMenuOpen] && !scr[kBagQuantityShown])
            p.drawText(QRectF(detail.left() + 44 * u, circle.bottom() + 100 * u, detail.width() - 88 * u, detail.bottom() - circle.bottom() - 130 * u),
                       Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, DecodeGameText((const u16*)(scr + kBagDescription), kTextLen));
    }

    // Action menu, over the detail panel
    if (scr[kBagMenuOpen])
    {
        int count = std::min<int>(scr[kBagMenuCount], 5);
        qreal itemH = 78 * u;
        QRectF panel(detail.center().x() - 170 * u, detail.bottom() - count * (itemH + 12 * u) - 60 * u, 340 * u, count * (itemH + 12 * u) + 40 * u);
        p.setPen(QPen(QColor(0, 0, 0, 30), 2 * u));
        p.setBrush(QColor(255, 255, 255, 250));
        p.drawRoundedRect(panel, 30 * u, 30 * u);
        font.setPixelSize((int)(30 * u));
        for (int i = 0; i < count; i++)
        {
            bool sel = i == scr[kBagMenuCursor];
            QRectF item(panel.left() + 18 * u, panel.top() + 20 * u + i * (itemH + 12 * u), panel.width() - 36 * u, itemH);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QColor(255, 196, 60) : QColor(232, 238, 246));
            p.drawRoundedRect(item, itemH / 2, itemH / 2);
            font.setBold(sel);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(item, Qt::AlignCenter, DecodeGameText((const u16*)(scr + kBagMenuItems + i * kBagMenuLen * 2), kBagMenuLen));
        }
    }

    // Quantity to toss/sell
    if (scr[kBagQuantityShown])
    {
        QRectF box(detail.center().x() - 150 * u, detail.bottom() - 260 * u, 300 * u, 110 * u);
        p.setPen(QPen(accent, 5 * u));
        p.setBrush(Qt::white);
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(50 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(box, Qt::AlignCenter, QString::fromUtf8("▲  × %1  ▼").arg(ReadU16(scr + kBagQuantity)));
    }

    // Message box at the bottom
    const qreal bottomY = listPanel.bottom() + 24 * u;
    int messageKind = scr[kBagMessageKind];
    if (messageKind != 0)
    {
        QRectF box(left, bottomY, contentW, std::min<qreal>(120 * u, height - bottomY - 20 * u));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 245));
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(34 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(box.adjusted(40 * u, 0, -40 * u, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
                   DecodeGameText((const u16*)(scr + kBagMessage), kTextLen));
    }
    else
    {
        DrawButtonHint(p, QPointF(left, bottomY + 14 * u), 44 * u, "A", "Choisir", QColor(80, 170, 90), Qt::white);
        DrawButtonHint(p, QPointF(left + 230 * u, bottomY + 14 * u), 44 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
        DrawButtonHint(p, QPointF(left + 460 * u, bottomY + 14 * u), 44 * u, QString::fromUtf8("◀▶"), "Poches", QColor(90, 110, 140), Qt::white);
    }

    // Yes / No
    if (scr[kBagYesNoShown])
    {
        QRectF panel(left + contentW - 300 * u, bottomY - 220 * u, 280 * u, 200 * u);
        p.setPen(QPen(QColor(0, 0, 0, 30), 2 * u));
        p.setBrush(Qt::white);
        p.drawRoundedRect(panel, 30 * u, 30 * u);
        font.setPixelSize((int)(34 * u));
        for (int i = 0; i < 2; i++)
        {
            bool sel = i == scr[kBagYesNoCursor];
            QRectF item(panel.left() + 20 * u, panel.top() + 20 * u + i * 86 * u, panel.width() - 40 * u, 74 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QColor(255, 196, 60) : QColor(232, 238, 246));
            p.drawRoundedRect(item, 37 * u, 37 * u);
            font.setBold(sel);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(item, Qt::AlignCenter, i == 0 ? "Oui" : "Non");
        }
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Summary (CoopUiSummaryScreen)
// ---------------------------------------------------------------------------

enum
{
    kSumPage = 1,
    kSumState = 2,
    kSumMoveCursor = 3,
    kSumIsEgg = 4,
    kSumGender = 5,
    kSumShiny = 6,
    kSumNature = 7,
    kSumSpecies = 12,
    kSumLevel = 14,
    kSumHp = 16,
    kSumMaxHp = 18,
    kSumStats = 20,        // u16[5]: atk, def, sp.atk, sp.def, speed
    kSumHeldItem = 30,
    kSumAbility = 32,
    kSumForm = 34,
    kSumTypes = 35,        // u8[2]
    kSumStatus = 37,
    kSumMetLevel = 38,
    kSumHasMoveToLearn = 39,
    kSumExp = 40,
    kSumExpStart = 44,
    kSumExpNext = 48,
    kSumOtId = 52,
    kSumMoves = 56,        // u16[5]
    kSumPp = 66,           // u8[5]
    kSumMaxPp = 71,
    kSumMoveTypes = 76,
    kSumMovePower = 81,
    kSumMoveAccuracy = 86,
    kSumMoveCategory = 91,
    kSumNickname = 96,     // u16[12]
    kSumOtName = 120,      // u16[8]
    kSumAbilityName = 136, // u16[16]
    kSumItemName = 168,    // u16[16]
    kSumMoveNames = 200,   // u16[5][16]
    kSumNameLen = 16,
};

static const char* TypeName(int type)
{
    static const char* kNames[] = { "Normal", "Combat", "Vol", "Poison", "Sol", "Roche", "Insecte", "Spectre", "Acier", "???",
                                    "Feu", "Eau", "Plante", "Électrik", "Psy", "Glace", "Dragon", "Ténèbres" };
    return type >= 0 && type < 18 ? kNames[type] : "???";
}

static QColor TypeColor(int type)
{
    static const QColor kColors[] = {
        QColor(168, 168, 120), QColor(192, 48, 40), QColor(168, 144, 240), QColor(160, 64, 160), QColor(224, 192, 104),
        QColor(184, 160, 56), QColor(168, 184, 32), QColor(112, 88, 152), QColor(184, 184, 208), QColor(104, 160, 144),
        QColor(240, 128, 48), QColor(104, 144, 240), QColor(120, 200, 80), QColor(248, 208, 48), QColor(248, 88, 136),
        QColor(152, 216, 216), QColor(112, 56, 248), QColor(112, 88, 72),
    };
    return type >= 0 && type < 18 ? kColors[type] : QColor(150, 150, 150);
}

static const char* NatureName(int nature)
{
    static const char* kNames[] = { "Hardi", "Solo", "Brave", "Rigide", "Mauvais", "Assuré", "Docile", "Relax", "Malin", "Lâche",
                                    "Timide", "Pressé", "Sérieux", "Jovial", "Naïf", "Modeste", "Doux", "Discret", "Pudique", "Foufou",
                                    "Calme", "Gentil", "Malpoli", "Prudent", "Bizarre" };
    return nature >= 0 && nature < 25 ? kNames[nature] : "";
}

// Nature effect on a stat (stats in the summary order: atk, def, sp.atk, sp.def, speed): +1, -1 or 0
static int NatureEffect(int nature, int stat)
{
    // Natures are ordered by (raised stat, lowered stat) in the order atk, def, speed, sp.atk, sp.def
    static const int kNatureOrderToSummary[5] = { 0, 1, 4, 2, 3 };
    int up = kNatureOrderToSummary[nature / 5], down = kNatureOrderToSummary[nature % 5];
    if (up == down)
        return 0;
    return stat == up ? 1 : stat == down ? -1 : 0;
}

static void DrawTypeBadge(QPainter& p, const QRectF& r, int type, qreal u)
{
    p.setPen(Qt::NoPen);
    p.setBrush(TypeColor(type));
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    QFont f("Segoe UI");
    f.setPixelSize((int)(r.height() * 0.58));
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(r, Qt::AlignCenter, QString::fromUtf8(TypeName(type)));
}

const QImage& GameUi::pokemonSprite(int species)
{
    auto it = spriteCache.find(species);
    if (it == spriteCache.end())
    {
        QString path = HoLympeAssetsDir() + QString("/pokemon_sprites/%1.png").arg(species);
        it = spriteCache.emplace(species, QImage(path)).first;
    }
    return it->second;
}

static u32 ReadU32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

void GameUi::drawSummary(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int page = scr[kSumPage];
    const int state = scr[kSumState];
    const bool isEgg = scr[kSumIsEgg] != 0;
    const int species = ReadU16(scr + kSumSpecies);
    const int type1 = scr[kSumTypes], type2 = scr[kSumTypes + 1];
    const QColor accent = TypeColor(type1);
    QFont font("Segoe UI");

    // Background in the Pokémon's main type color
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, accent.darker(130));
    bg.setColorAt(1, accent.darker(280));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 120 * u, 1760 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 60 * u;
    const qreal leftW = contentW * 0.38;

    // Left: sprite on a platform, name, level, types, held item
    QRectF platform(left + leftW / 2 - 230 * u, top + 330 * u, 460 * u, 90 * u);
    QRadialGradient platformGlow(platform.center(), platform.width() / 2);
    platformGlow.setColorAt(0, QColor(255, 255, 255, 110));
    platformGlow.setColorAt(1, QColor(255, 255, 255, 0));
    p.setBrush(platformGlow);
    p.drawEllipse(platform);
    const QImage& sprite = isEgg ? pokemonIcon(0, true) : pokemonSprite(species);
    if (!sprite.isNull())
    {
        int frames = sprite.width() >= sprite.height() * 2 ? 2 : 1;
        int frameW = sprite.width() / frames;
        int frame = frames == 2 ? (screen.animFrame / 24) % 2 : 0;
        QImage img = sprite.copy(frame * frameW, 0, frameW, sprite.height() / (isEgg ? 2 : 1));
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        qreal size = 400 * u;
        p.drawImage(QRectF(platform.center().x() - size / 2, platform.center().y() - size + 30 * u, size, size), img);
        p.restore();
    }

    QString name = DecodeGameText((const u16*)(scr + kSumNickname), 12);
    font.setPixelSize((int)(54 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    QRectF nameRect(left, top + 450 * u, leftW, 70 * u);
    p.drawText(nameRect, Qt::AlignCenter, name);
    if (!isEgg)
    {
        int gender = scr[kSumGender];
        font.setPixelSize((int)(36 * u));
        p.setFont(font);
        QString line = QString("N. %1").arg(ReadU16(scr + kSumLevel));
        if (gender < 2)
            line += gender == 0 ? QString::fromUtf8("   ♂") : QString::fromUtf8("   ♀");
        if (scr[kSumShiny])
            line += QString::fromUtf8("   ✦");
        p.drawText(QRectF(left, nameRect.bottom(), leftW, 50 * u), Qt::AlignCenter, line);

        qreal badgeW = 170 * u, badgeH = 48 * u;
        qreal badgesY = nameRect.bottom() + 70 * u;
        if (type1 == type2)
            DrawTypeBadge(p, QRectF(left + leftW / 2 - badgeW / 2, badgesY, badgeW, badgeH), type1, u);
        else
        {
            DrawTypeBadge(p, QRectF(left + leftW / 2 - badgeW - 10 * u, badgesY, badgeW, badgeH), type1, u);
            DrawTypeBadge(p, QRectF(left + leftW / 2 + 10 * u, badgesY, badgeW, badgeH), type2, u);
        }

        // Held item
        QRectF itemBox(left + 40 * u, badgesY + 90 * u, leftW - 80 * u, 90 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 230));
        p.drawRoundedRect(itemBox, 45 * u, 45 * u);
        int heldItem = ReadU16(scr + kSumHeldItem);
        font.setPixelSize((int)(32 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        if (heldItem != 0)
        {
            const QImage& icon = itemIcon(heldItem);
            if (!icon.isNull())
            {
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(QRectF(itemBox.left() + 24 * u, itemBox.center().y() - 30 * u, 60 * u, 60 * u), icon);
                p.restore();
            }
            p.drawText(itemBox.adjusted(100 * u, 0, -24 * u, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       DecodeGameText((const u16*)(scr + kSumItemName), kSumNameLen));
        }
        else
            p.drawText(itemBox, Qt::AlignCenter, QString::fromUtf8("Aucun objet"));
    }

    // Right: page tabs and content
    const qreal rightX = left + leftW + 40 * u;
    const qreal rightW = contentW - leftW - 40 * u;
    const char* tabs[] = { "Infos", "Capacités", "Rubans" };
    int shownPage = page == 3 ? 1 : page;
    for (int i = 0; i < 3; i++)
    {
        QRectF tab(rightX + i * (rightW / 3), top, rightW / 3 - 16 * u, 74 * u);
        bool current = i == shownPage;
        p.setPen(Qt::NoPen);
        p.setBrush(current ? QColor(255, 255, 255) : QColor(255, 255, 255, 60));
        p.drawRoundedRect(tab, 37 * u, 37 * u);
        font.setPixelSize((int)(34 * u));
        font.setBold(current);
        p.setFont(font);
        p.setPen(current ? accent.darker(160) : QColor(255, 255, 255));
        p.drawText(tab, Qt::AlignCenter, QString::fromUtf8(tabs[i]));
    }

    QRectF panel(rightX, top + 100 * u, rightW, height - top - 100 * u - 120 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255, 242));
    p.drawRoundedRect(panel, 36 * u, 36 * u);

    auto drawRow = [&](qreal y, const QString& label, const QString& value) {
        font.setPixelSize((int)(32 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(100, 110, 128));
        p.drawText(QRectF(panel.left() + 50 * u, y, 330 * u, 56 * u), Qt::AlignVCenter | Qt::AlignLeft, label);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(panel.left() + 380 * u, y, panel.width() - 430 * u, 56 * u), Qt::AlignVCenter | Qt::AlignLeft, value);
    };

    if (isEgg)
    {
        font.setPixelSize((int)(36 * u));
        p.setFont(font);
        p.setPen(QColor(70, 80, 100));
        p.drawText(panel, Qt::AlignCenter | Qt::TextWordWrap, QString::fromUtf8("Un Œuf mystérieux.\nQue va-t-il en sortir ?"));
    }
    else if (shownPage == 0)
    {
        qreal y = panel.top() + 40 * u;
        drawRow(y, QString::fromUtf8("N° Pokédex"), QString("%1").arg(species, 3, 10, QChar('0'))); y += 66 * u;
        drawRow(y, QString::fromUtf8("Dresseur"), DecodeGameText((const u16*)(scr + kSumOtName), 8)); y += 66 * u;
        drawRow(y, "ID", QString("%1").arg(ReadU16(scr + kSumOtId), 5, 10, QChar('0'))); y += 66 * u;
        drawRow(y, "Nature", QString::fromUtf8(NatureName(scr[kSumNature]))); y += 66 * u;
        drawRow(y, "Talent", DecodeGameText((const u16*)(scr + kSumAbilityName), kSumNameLen)); y += 66 * u;
        drawRow(y, QString::fromUtf8("Rencontré au"), QString("N. %1").arg(scr[kSumMetLevel])); y += 86 * u;

        // Experience
        u32 exp = ReadU32(scr + kSumExp), start = ReadU32(scr + kSumExpStart), next = ReadU32(scr + kSumExpNext);
        drawRow(y, QString::fromUtf8("Expérience"), QString::number(exp)); y += 66 * u;
        drawRow(y, "Niveau suivant", next > exp ? QString("%1 pts").arg(next - exp) : QString("—")); y += 76 * u;
        QRectF bar(panel.left() + 50 * u, y, panel.width() - 100 * u, 24 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 66, 80));
        p.drawRoundedRect(bar, 12 * u, 12 * u);
        qreal ratio = next > start ? std::min(1.0, (double)(exp - start) / (next - start)) : 1.0;
        p.setBrush(QColor(80, 170, 240));
        p.drawRoundedRect(QRectF(bar.left() + 3 * u, bar.top() + 3 * u, (bar.width() - 6 * u) * ratio, bar.height() - 6 * u), 9 * u, 9 * u);
    }
    else if (shownPage == 1)
    {
        // Stats with bars (nature: red = raised, blue = lowered)
        const char* statNames[] = { "PV", "Attaque", "Défense", "Atq. Spé.", "Déf. Spé.", "Vitesse" };
        int hp = ReadU16(scr + kSumHp), maxHp = ReadU16(scr + kSumMaxHp);
        qreal y = panel.top() + 30 * u;
        const qreal statsW = panel.width() * 0.46;
        for (int i = 0; i < 6; i++)
        {
            int value = i == 0 ? maxHp : ReadU16(scr + kSumStats + (i - 1) * 2);
            int effect = i == 0 ? 0 : NatureEffect(scr[kSumNature], i - 1);
            font.setPixelSize((int)(28 * u));
            font.setBold(effect != 0);
            p.setFont(font);
            p.setPen(effect > 0 ? QColor(220, 70, 70) : effect < 0 ? QColor(60, 120, 220) : QColor(90, 100, 118));
            p.drawText(QRectF(panel.left() + 40 * u, y, 190 * u, 44 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(statNames[i]));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(panel.left() + 230 * u, y, 140 * u, 44 * u), Qt::AlignVCenter | Qt::AlignRight,
                       i == 0 ? QString("%1/%2").arg(hp).arg(maxHp) : QString::number(value));
            QRectF bar(panel.left() + 390 * u, y + 14 * u, statsW - 410 * u, 16 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 226, 236));
            p.drawRoundedRect(bar, 8 * u, 8 * u);
            qreal ratio = std::min(1.0, value / 255.0);
            p.setBrush(i == 0 ? HpColor(hp, maxHp) : accent);
            p.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max<qreal>(bar.height(), bar.width() * ratio), bar.height()), 8 * u, 8 * u);
            y += 56 * u;
        }
        // Ability under the stats
        font.setPixelSize((int)(28 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(100, 110, 128));
        p.drawText(QRectF(panel.left() + 40 * u, y + 10 * u, 200 * u, 44 * u), Qt::AlignVCenter | Qt::AlignLeft, "Talent");
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(panel.left() + 200 * u, y + 10 * u, statsW - 220 * u, 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
                   DecodeGameText((const u16*)(scr + kSumAbilityName), kSumNameLen));

        // Moves
        const bool selecting = state >= 3 && state <= 9;
        int moveCount = scr[kSumHasMoveToLearn] ? 5 : 4;
        qreal mx = panel.left() + statsW + 10 * u, mw = panel.width() - statsW - 40 * u;
        qreal my = panel.top() + 30 * u;
        qreal mh = (panel.height() - 60 * u) / 5 - 12 * u;
        const char* categories[] = { "Physique", "Spécial", "Statut" };
        for (int i = 0; i < moveCount; i++)
        {
            int move = ReadU16(scr + kSumMoves + i * 2);
            QRectF card(mx, my + i * (mh + 12 * u), mw, mh);
            bool sel = selecting && i == scr[kSumMoveCursor];
            if (move == 0)
            {
                p.setPen(QPen(QColor(0, 0, 0, 40), 2 * u, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(card, 24 * u, 24 * u);
                continue;
            }
            int mtype = scr[kSumMoveTypes + i];
            DrawAccentCard(p, card, 24 * u, i == 4 ? QColor(255, 244, 214) : QColor(236, 241, 248), TypeColor(mtype), 16 * u,
                           sel ? QPen(QColor(255, 170, 40), 5 * u) : QPen(Qt::NoPen));
            font.setPixelSize((int)(30 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(card.left() + 36 * u, card.top() + 8 * u, card.width() - 200 * u, card.height() / 2), Qt::AlignVCenter | Qt::AlignLeft,
                       DecodeGameText((const u16*)(scr + kSumMoveNames + i * kSumNameLen * 2), kSumNameLen));
            font.setPixelSize((int)(26 * u));
            font.setBold(false);
            p.setFont(font);
            p.drawText(QRectF(card.right() - 180 * u, card.top() + 8 * u, 160 * u, card.height() / 2), Qt::AlignVCenter | Qt::AlignRight,
                       QString("PP %1/%2").arg(scr[kSumPp + i]).arg(scr[kSumMaxPp + i]));
            QString details = QString::fromUtf8("%1  ·  %2").arg(QString::fromUtf8(TypeName(mtype))).arg(QString::fromUtf8(categories[std::min<int>(scr[kSumMoveCategory + i], 2)]));
            if (scr[kSumMovePower + i] > 1)
                details += QString::fromUtf8("  ·  Puiss. %1").arg(scr[kSumMovePower + i]);
            if (scr[kSumMoveAccuracy + i] > 0)
                details += QString::fromUtf8("  ·  Préc. %1").arg(scr[kSumMoveAccuracy + i]);
            p.setPen(QColor(100, 110, 128));
            p.drawText(QRectF(card.left() + 36 * u, card.center().y(), card.width() - 56 * u, card.height() / 2 - 6 * u), Qt::AlignVCenter | Qt::AlignLeft, details);
        }
    }
    else
    {
        font.setPixelSize((int)(36 * u));
        p.setFont(font);
        p.setPen(QColor(70, 80, 100));
        p.drawText(panel, Qt::AlignCenter, QString::fromUtf8("Rubans"));
    }

    // Button hints
    qreal hintY = height - 90 * u;
    DrawButtonHint(p, QPointF(left, hintY), 44 * u, QString::fromUtf8("◀▶"), "Pages", QColor(90, 110, 140), Qt::white);
    DrawButtonHint(p, QPointF(left + 260 * u, hintY), 44 * u, "L R", QString::fromUtf8("Pokémon"), QColor(90, 110, 140), Qt::white);
    DrawButtonHint(p, QPointF(left + 560 * u, hintY), 44 * u, "A", "Attaques", QColor(80, 170, 90), Qt::white);
    DrawButtonHint(p, QPointF(left + 820 * u, hintY), 44 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    p.restore();
}

// ---------------------------------------------------------------------------
// Battle command menu (CoopUiBattleScreen), drawn over the game's message box
// ---------------------------------------------------------------------------

enum
{
    kBatMenuId = 1,
    kBatCursor = 2,
    kBatCancelRun = 3,
    kBatType = 4,
    kBatMoves = 12,        // u16[4]
    kBatPp = 20,
    kBatMaxPp = 24,
    kBatMoveTypes = 28,
    kBatMovePower = 32,
    kBatMoveAccuracy = 36,
    kBatMoveCategory = 40,
    kBatLabels = 48,       // u16[8][24]
    kBatLabelLen = 24,
    kBatPrompt = 432,      // u16[96]
};

enum
{
    kBattleMenuPalPark = 9,
    kBattleMenuPalPark2 = 10,
    kBattleMenuFight = 11,
    kBattleMenuTarget = 12,
    kBattleMenuYesNo = 13,
    kBattleMenuSwitchOrKeep = 17,
    kBattleMenuVsRecorder = 18,
};

// One button of the battle menu: rounded card in its color, white outline when selected
static void DrawBattleButton(QPainter& p, const QRectF& r, const QColor& color, bool selected, bool enabled, qreal u)
{
    QRectF box = selected ? r.adjusted(-4 * u, -4 * u, 4 * u, 4 * u) : r;
    qreal radius = std::min<qreal>(box.height() / 2, 34 * u);
    if (selected)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 60));
        p.drawRoundedRect(box.adjusted(-8 * u, -8 * u, 8 * u, 8 * u), radius + 8 * u, radius + 8 * u);
    }
    QLinearGradient g(box.topLeft(), box.bottomLeft());
    QColor c = enabled ? color : QColor(110, 116, 130);
    g.setColorAt(0, selected ? c.lighter(118) : c);
    g.setColorAt(1, selected ? c : c.darker(125));
    p.setPen(selected ? QPen(Qt::white, 5 * u) : QPen(QColor(0, 0, 0, 50), 2 * u));
    p.setBrush(g);
    p.drawRoundedRect(box, radius, radius);
}

void GameUi::drawBattle(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int menuId = scr[kBatMenuId];
    const int cursor = scr[kBatCursor];
    auto label = [&](int i) { return DecodeGameText((const u16*)(scr + kBatLabels + i * kBatLabelLen * 2), kBatLabelLen).trimmed(); };
    QFont font("Segoe UI");

    // The band covers the game's message box (bottom quarter of the screen)
    const qreal bandTop = height * 0.745;
    const QRectF band(0, bandTop, width, height - bandTop);
    QLinearGradient bg(band.topLeft(), band.bottomLeft());
    bg.setColorAt(0, QColor(18, 28, 52));
    bg.setColorAt(1, QColor(10, 16, 32));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRect(band);
    p.setBrush(QColor(255, 255, 255, 40));
    p.drawRect(QRectF(0, bandTop, width, 3 * u));

    const qreal gameW = std::min<qreal>(width, height * 4.0 / 3.0);
    const qreal gameLeft = (width - gameW) / 2;
    const qreal pad = 26 * u;
    const QRectF left(gameLeft + pad, bandTop + pad, gameW * 0.36, band.height() - 2 * pad);
    const QRectF area(left.right() + pad, bandTop + pad, gameLeft + gameW - left.right() - 2 * pad, band.height() - 2 * pad);

    auto drawPrompt = [&](const QString& text) {
        font.setPixelSize((int)(38 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(left, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text);
    };
    auto drawLabel = [&](const QRectF& r, const QString& text, int size, int flags = Qt::AlignCenter) {
        font.setPixelSize((int)(size * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(0, 0, 0, 70));
        p.drawText(r.translated(2 * u, 2 * u), flags, text);
        p.setPen(Qt::white);
        p.drawText(r, flags, text);
    };
    QString prompt = DecodeGameText((const u16*)(scr + kBatPrompt), 96).simplified();

    if (menuId == kBattleMenuFight)
    {
        const qreal gap = 16 * u;
        const qreal cancelH = 52 * u;
        const qreal cardW = (area.width() - gap) / 2;
        const qreal cardH = (area.height() - cancelH - 2 * gap) / 2;
        const char* categories[] = { "Physique", "Spécial", "Statut" };
        for (int i = 0; i < 4; i++)
        {
            QRectF card(area.left() + (i % 2) * (cardW + gap), area.top() + (i / 2) * (cardH + gap), cardW, cardH);
            int move = ReadU16(scr + kBatMoves + i * 2);
            bool selected = cursor == i + 1;
            if (move == 0)
            {
                p.setPen(QPen(QColor(255, 255, 255, 40), 2 * u, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(card, 30 * u, 30 * u);
                continue;
            }
            int type = scr[kBatMoveTypes + i];
            int pp = scr[kBatPp + i], maxPp = scr[kBatMaxPp + i];
            DrawBattleButton(p, card, TypeColor(type).darker(115), selected, pp > 0, u);
            drawLabel(QRectF(card.left() + 28 * u, card.top(), card.width() - 200 * u, card.height()), label(i), 34,
                      Qt::AlignVCenter | Qt::AlignLeft);
            // PP, colored when running low
            QColor ppColor = pp == 0 ? QColor(255, 120, 110) : pp * 4 <= maxPp ? QColor(255, 200, 90) : QColor(255, 255, 255, 230);
            font.setPixelSize((int)(28 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(ppColor);
            p.drawText(QRectF(card.right() - 190 * u, card.top(), 166 * u, card.height()), Qt::AlignVCenter | Qt::AlignRight,
                       QString("PP %1/%2").arg(pp).arg(maxPp));
        }
        QRectF cancel(area.left(), area.bottom() - cancelH, area.width(), cancelH);
        DrawBattleButton(p, cancel, QColor(70, 84, 110), cursor == 0, true, u);
        drawLabel(cancel, label(4).isEmpty() ? QString("Retour") : label(4), 28);

        // Left: details of the highlighted move
        int sel = cursor >= 1 && cursor <= 4 ? cursor - 1 : -1;
        if (sel >= 0 && ReadU16(scr + kBatMoves + sel * 2) != 0)
        {
            int type = scr[kBatMoveTypes + sel];
            QRectF badge(left.left(), left.top() + 10 * u, 170 * u, 46 * u);
            DrawTypeBadge(p, badge, type, u);
            font.setPixelSize((int)(28 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(220, 228, 240));
            p.drawText(QRectF(badge.right() + 20 * u, badge.top(), left.width() - badge.width() - 20 * u, badge.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::fromUtf8(categories[std::min<int>(scr[kBatMoveCategory + sel], 2)]));
            QString power = scr[kBatMovePower + sel] > 1 ? QString::number(scr[kBatMovePower + sel]) : QString::fromUtf8("—");
            QString accuracy = scr[kBatMoveAccuracy + sel] > 0 ? QString::number(scr[kBatMoveAccuracy + sel]) : QString::fromUtf8("—");
            font.setPixelSize((int)(32 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(QRectF(left.left(), badge.bottom() + 20 * u, left.width(), 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::fromUtf8("Puissance  %1").arg(power));
            p.drawText(QRectF(left.left(), badge.bottom() + 70 * u, left.width(), 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::fromUtf8("Précision  %1").arg(accuracy));
        }
        else
            drawPrompt(prompt);
    }
    else if (menuId == kBattleMenuTarget)
    {
        drawPrompt(prompt);
        // Opponents on top (left, right), allies below, like on the battlefield
        const int layout[4] = { 3, 1, 0, 2 };
        const qreal gap = 16 * u;
        const qreal cancelH = 52 * u;
        const qreal cardW = (area.width() - gap) / 2;
        const qreal cardH = (area.height() - cancelH - 2 * gap) / 2;
        for (int i = 0; i < 4; i++)
        {
            int target = layout[i];
            QRectF card(area.left() + (i % 2) * (cardW + gap), area.top() + (i / 2) * (cardH + gap), cardW, cardH);
            QString name = label(target);
            DrawBattleButton(p, card, target & 1 ? QColor(200, 70, 80) : QColor(60, 120, 210), cursor == target, !name.isEmpty(), u);
            drawLabel(card, name, 32);
        }
        QRectF cancel(area.left(), area.bottom() - cancelH, area.width(), cancelH);
        DrawBattleButton(p, cancel, QColor(70, 84, 110), cursor == 4, true, u);
        drawLabel(cancel, label(4).isEmpty() ? QString("Retour") : label(4), 28);
    }
    else if (menuId >= kBattleMenuYesNo && menuId <= kBattleMenuSwitchOrKeep)
    {
        drawPrompt(prompt);
        const qreal gap = 18 * u;
        const qreal h = (area.height() - gap) / 2;
        for (int i = 0; i < 2; i++)
        {
            QRectF r(area.left() + area.width() * 0.2, area.top() + i * (h + gap), area.width() * 0.6, h);
            DrawBattleButton(p, r, i == 0 ? QColor(60, 150, 90) : QColor(200, 70, 80), cursor == i, true, u);
            drawLabel(r, label(i), 34);
        }
    }
    else if (menuId == kBattleMenuPalPark || menuId == kBattleMenuPalPark2 || menuId == kBattleMenuVsRecorder)
    {
        // Rare menus: their buttons in a column
        drawPrompt(prompt);
        std::vector<QString> labels;
        for (int i = 0; i < 8; i++)
            if (!label(i).isEmpty())
                labels.push_back(label(i));
        const qreal gap = 18 * u;
        const qreal h = labels.empty() ? 0 : (area.height() - gap * (labels.size() - 1)) / labels.size();
        for (size_t i = 0; i < labels.size(); i++)
        {
            QRectF r(area.left() + area.width() * 0.2, area.top() + i * (h + gap), area.width() * 0.6, h);
            DrawBattleButton(p, r, QColor(60, 120, 210), cursor == (int)i, true, u);
            drawLabel(r, labels[i], 34);
        }
    }
    else
    {
        // Main menu: FIGHT on top, BAG / RUN / POKéMON below (same layout as the game, for the cursor)
        drawPrompt(prompt);
        const qreal gap = 16 * u;
        const qreal topH = (area.height() - gap) * 0.54;
        const qreal bottomH = area.height() - gap - topH;
        const qreal smallW = (area.width() - 2 * gap) / 3;
        QRectF fight(area.left(), area.top(), area.width(), topH);
        QRectF bag(area.left(), fight.bottom() + gap, smallW, bottomH);
        QRectF run(bag.right() + gap, bag.top(), smallW, bottomH);
        QRectF pokemon(run.right() + gap, bag.top(), smallW, bottomH);
        DrawBattleButton(p, fight, QColor(214, 62, 62), cursor == 0, true, u);
        DrawBattleButton(p, bag, QColor(222, 142, 40), cursor == 1, true, u);
        DrawBattleButton(p, pokemon, QColor(64, 160, 84), cursor == 2, true, u);
        DrawBattleButton(p, run, scr[kBatCancelRun] ? QColor(90, 100, 120) : QColor(56, 120, 210), cursor == 3, true, u);
        drawLabel(fight, label(0), 46);
        drawLabel(bag, label(1), 32);
        drawLabel(pokemon, label(2), 32);
        drawLabel(run, label(3), 32);
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Battle bag (CoopUiBattleBagScreen)
// ---------------------------------------------------------------------------

enum
{
    kBbScreen = 1,
    kBbPocket = 2,
    kBbCursor = 3,
    kBbPage = 4,
    kBbLastPage = 5,
    kBbMessageShown = 7,
    kBbPagePosition = 12,
    kBbPocketCounts = 13,  // u8[5]
    kBbLastUsedItem = 18,
    kBbSelectedItem = 20,
    kBbItems = 22,         // u16[6]
    kBbQuantities = 34,    // u16[6]
    kBbItemNames = 48,     // u16[6][16]
    kBbLastUsedName = 240, // u16[16]
    kBbDescription = 272,  // u16[96]
    kBbMessage = 464,      // u16[96]
    kBbNameLen = 16,
};

static const char* BattlePocketName(int pocket)
{
    static const char* kNames[] = { "Soins PV/PP", "Soins statut", "Poké Balls", "Objets de combat", "" };
    return pocket >= 0 && pocket < 5 ? kNames[pocket] : "";
}

static QColor BattlePocketColor(int pocket)
{
    static const QColor kColors[] = { QColor(76, 170, 110), QColor(230, 150, 60), QColor(220, 72, 72), QColor(90, 120, 220), QColor(120, 120, 140) };
    return pocket >= 0 && pocket < 5 ? kColors[pocket] : kColors[4];
}

void GameUi::drawBattleBag(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int screenId = scr[kBbScreen];
    const int pocket = std::min<int>(scr[kBbPocket], 4);
    const int cursor = scr[kBbCursor];
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) { return DecodeGameText((const u16*)(scr + offset), len).simplified(); };

    // Background: soft gradient in the pocket's color (neutral on the pockets screen)
    QColor accent = screenId == 0 ? QColor(70, 96, 150) : BattlePocketColor(pocket);
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, accent.lighter(115));
    bg.setColorAt(1, accent.darker(220));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 160 * u, 1600 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 70 * u;

    // Title
    font.setPixelSize((int)(56 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    QString title = screenId == 0 ? QString::fromUtf8("Sac") : QString::fromUtf8(BattlePocketName(pocket));
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, title);

    auto card = [&](const QRectF& r, bool selected, const QColor& tint, bool enabled = true) {
        QRectF box = selected ? r.adjusted(-6 * u, -6 * u, 6 * u, 6 * u) : r;
        if (selected)
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 70));
            p.drawRoundedRect(box.adjusted(-8 * u, -8 * u, 8 * u, 8 * u), 40 * u, 40 * u);
        }
        DrawAccentCard(p, box, 32 * u, enabled ? QColor(250, 252, 255, selected ? 255 : 236) : QColor(220, 224, 232, 200), tint, 18 * u,
                       selected ? QPen(QColor(255, 196, 60), 6 * u) : QPen(Qt::NoPen));
        return box;
    };
    auto drawIcon = [&](const QRectF& r, int item) {
        const QImage& icon = itemIcon(item);
        if (icon.isNull())
            return;
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(r, icon);
        p.restore();
    };
    auto backButton = [&](const QRectF& r, bool selected, const QString& label) {
        QRectF box = selected ? r.adjusted(-4 * u, -4 * u, 4 * u, 4 * u) : r;
        p.setPen(selected ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
        p.setBrush(QColor(220, 80, 80));
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        font.setPixelSize((int)(34 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    };

    const qreal gridTop = top + 120 * u;
    const qreal bottomBarH = 110 * u;
    const qreal gridH = height - gridTop - bottomBarH - 100 * u;
    const qreal gap = 30 * u;

    if (screenId == 0)
    {
        // Pockets: HP/PP top-left, status bottom-left, balls top-right, battle items bottom-right
        const int layout[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
        const qreal cardW = (contentW - gap) / 2, cardH = (gridH - gap) / 2;
        for (int i = 0; i < 4; i++)
        {
            QRectF r(left + layout[i][0] * (cardW + gap), gridTop + layout[i][1] * (cardH + gap), cardW, cardH);
            int count = scr[kBbPocketCounts + i];
            QRectF box = card(r, cursor == i, BattlePocketColor(i), count > 0);
            font.setPixelSize((int)(46 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(box.adjusted(60 * u, 0, -40 * u, -box.height() * 0.25), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(BattlePocketName(i)));
            font.setPixelSize((int)(30 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(100, 110, 128));
            p.drawText(box.adjusted(60 * u, box.height() * 0.45, -40 * u, -20 * u), Qt::AlignVCenter | Qt::AlignLeft,
                       count == 0 ? QString::fromUtf8("Vide") : count == 1 ? QString::fromUtf8("1 objet") : QString::fromUtf8("%1 objets").arg(count));
        }
        // Last used item and back
        QRectF bar(left, height - bottomBarH - 60 * u, contentW - 300 * u, bottomBarH);
        int lastUsed = ReadU16(scr + kBbLastUsedItem);
        QRectF box = card(bar, cursor == 4, QColor(150, 150, 170), lastUsed != 0);
        font.setPixelSize((int)(32 * u));
        font.setBold(lastUsed != 0);
        p.setFont(font);
        p.setPen(lastUsed ? QColor(32, 40, 56) : QColor(130, 136, 150));
        if (lastUsed)
        {
            drawIcon(QRectF(box.left() + 40 * u, box.center().y() - 36 * u, 72 * u, 72 * u), lastUsed);
            p.drawText(box.adjusted(130 * u, 0, -30 * u, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::fromUtf8("Dernier objet utilisé : %1").arg(text(kBbLastUsedName, kBbNameLen)));
        }
        else
            p.drawText(box, Qt::AlignCenter, QString::fromUtf8("Aucun objet utilisé récemment"));
        backButton(QRectF(left + contentW - 270 * u, bar.top() + 10 * u, 270 * u, bottomBarH - 20 * u), cursor == 5, "Retour");
    }
    else if (screenId == 1)
    {
        // Items of the pocket: 2 columns x 3 rows, like the game
        int page = scr[kBbPage], lastPage = scr[kBbLastPage];
        font.setPixelSize((int)(34 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(255, 255, 255, 220));
        if (lastPage > 0)
            p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignRight,
                       QString::fromUtf8("◀  Page %1 / %2  ▶").arg(page + 1).arg(lastPage + 1));
        const qreal cardW = (contentW - gap) / 2, cardH = (gridH - 2 * gap) / 3;
        for (int i = 0; i < 6; i++)
        {
            QRectF r(left + (i % 2) * (cardW + gap), gridTop + (i / 2) * (cardH + gap), cardW, cardH);
            int item = ReadU16(scr + kBbItems + i * 2);
            if (item == 0)
            {
                p.setPen(QPen(QColor(255, 255, 255, 50), 2 * u, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r, 32 * u, 32 * u);
                continue;
            }
            QRectF box = card(r, cursor == i, BattlePocketColor(pocket));
            drawIcon(QRectF(box.left() + 50 * u, box.center().y() - 44 * u, 88 * u, 88 * u), item);
            font.setPixelSize((int)(38 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(box.adjusted(160 * u, 0, -170 * u, 0), Qt::AlignVCenter | Qt::AlignLeft, text(kBbItemNames + i * kBbNameLen * 2, kBbNameLen));
            font.setPixelSize((int)(34 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(90, 100, 118));
            p.drawText(box.adjusted(0, 0, -40 * u, 0), Qt::AlignVCenter | Qt::AlignRight,
                       QString::fromUtf8("× %1").arg(ReadU16(scr + kBbQuantities + i * 2)));
        }
        backButton(QRectF(left + contentW - 270 * u, height - bottomBarH - 50 * u, 270 * u, bottomBarH - 20 * u), cursor == 6, "Retour");
        if (lastPage > 0)
            DrawButtonHint(p, QPointF(left, height - bottomBarH - 30 * u), 50 * u, QString::fromUtf8("◀▶"), "Changer de page",
                           QColor(90, 110, 140), Qt::white);
    }
    else
    {
        // Use an item: big card with its description
        int item = ReadU16(scr + kBbSelectedItem);
        int position = scr[kBbPagePosition];
        QRectF r(left + contentW * 0.12, gridTop + 20 * u, contentW * 0.76, gridH * 0.72);
        QRectF box = card(r, false, BattlePocketColor(pocket));
        drawIcon(QRectF(box.left() + 70 * u, box.top() + 60 * u, 150 * u, 150 * u), item);
        font.setPixelSize((int)(54 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        QString name = position < 6 ? text(kBbItemNames + position * kBbNameLen * 2, kBbNameLen) : QString();
        p.drawText(QRectF(box.left() + 260 * u, box.top() + 60 * u, box.width() - 300 * u, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, name);
        if (position < 6)
        {
            font.setPixelSize((int)(36 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(90, 100, 118));
            p.drawText(QRectF(box.left() + 260 * u, box.top() + 140 * u, box.width() - 300 * u, 60 * u), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::fromUtf8("En stock : %1").arg(ReadU16(scr + kBbQuantities + position * 2)));
        }
        font.setPixelSize((int)(38 * u));
        p.setFont(font);
        p.setPen(QColor(50, 58, 74));
        p.drawText(QRectF(box.left() + 70 * u, box.top() + 250 * u, box.width() - 140 * u, box.height() - 290 * u),
                   Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, text(kBbDescription, 96));

        QRectF use(r.left(), r.bottom() + 50 * u, r.width() * 0.62, 110 * u);
        QRectF useBox = use;
        if (cursor == 0)
            useBox = use.adjusted(-4 * u, -4 * u, 4 * u, 4 * u);
        p.setPen(cursor == 0 ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
        p.setBrush(QColor(70, 170, 100));
        p.drawRoundedRect(useBox, useBox.height() / 2, useBox.height() / 2);
        font.setPixelSize((int)(42 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(useBox, Qt::AlignCenter, QString::fromUtf8("Utiliser"));
        backButton(QRectF(use.right() + 40 * u, use.top() + 10 * u, r.right() - use.right() - 40 * u, use.height() - 20 * u), cursor == 1, "Retour");
    }

    // Game message (e.g. the item can't be used now)
    if (scr[kBbMessageShown])
    {
        QRectF box(left, height - 260 * u, contentW, 190 * u);
        p.setPen(QPen(QColor(255, 255, 255, 60), 3 * u));
        p.setBrush(QColor(18, 28, 52, 245));
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(40 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box.adjusted(50 * u, 20 * u, -50 * u, -20 * u), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text(kBbMessage, 96));
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Battle party (CoopUiBattlePartyScreen)
// ---------------------------------------------------------------------------

enum
{
    kBpScreen = 1,
    kBpCursor = 2,
    kBpCount = 3,
    kBpSelected = 4,
    kBpMode = 5,
    kBpMessageShown = 6,
    kBpMons = 12,          // 6 x 36 bytes
    kBpMonSize = 36,
    kBpStats = 228,        // u16[5]
    kBpTypes = 240,
    kBpMoveToLearn = 242,
    kBpAbilityName = 248,
    kBpItemName = 280,
    kBpMoves = 312,        // u16[5]
    kBpPp = 322,
    kBpMaxPp = 327,
    kBpMoveTypes = 332,
    kBpMovePower = 337,
    kBpMoveAccuracy = 342,
    kBpMoveCategory = 347,
    kBpMoveNames = 352,    // u16[5][16]
    kBpMessage = 512,      // u16[96]
    kBpNameLen = 16,
};

void GameUi::drawBattleParty(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int screenId = scr[kBpScreen];
    const int cursor = scr[kBpCursor];
    const int selected = std::min<int>(scr[kBpSelected], 5);
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) { return DecodeGameText((const u16*)(scr + offset), len).simplified(); };
    auto mon = [&](int i) { return scr + kBpMons + i * kBpMonSize; };

    // Background like the field party screen
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(46, 120, 170));
    bg.setColorAt(1, QColor(20, 50, 96));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 160 * u, 1640 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 60 * u;
    const qreal gap = 26 * u;

    auto button = [&](const QRectF& r, const QString& label, bool selectedButton, const QColor& color, int size = 36) {
        QRectF box = selectedButton ? r.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : r;
        if (selectedButton)
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 70));
            p.drawRoundedRect(box.adjusted(-8 * u, -8 * u, 8 * u, 8 * u), box.height() / 2 + 8 * u, box.height() / 2 + 8 * u);
        }
        p.setPen(selectedButton ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
        p.setBrush(color);
        p.drawRoundedRect(box, std::min<qreal>(box.height() / 2, 40 * u), std::min<qreal>(box.height() / 2, 40 * u));
        font.setPixelSize((int)(size * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    };
    auto title = [&](const QString& t) {
        font.setPixelSize((int)(54 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, t);
    };
    auto hpBar = [&](const QRectF& r, int hp, int maxHp) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(60, 66, 80));
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        qreal ratio = maxHp > 0 ? std::min(1.0, (double)hp / maxHp) : 0;
        if (ratio > 0)
        {
            p.setBrush(HpColor(hp, maxHp));
            p.drawRoundedRect(QRectF(r.left() + 3 * u, r.top() + 3 * u, std::max<qreal>(r.height(), (r.width() - 6 * u) * ratio), r.height() - 6 * u),
                              r.height() / 2 - 3 * u, r.height() / 2 - 3 * u);
        }
    };
    auto drawImage = [&](const QRectF& r, const QImage& img) {
        if (img.isNull())
            return;
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(r, img);
        p.restore();
    };
    // Header with the selected Pokémon: sprite, name, level, types, HP
    auto monHeader = [&](const QRectF& area) {
        const u8* m = mon(selected);
        int species = ReadU16(m);
        bool egg = m[11] != 0;
        QRectF panel = area;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 236));
        p.drawRoundedRect(panel, 36 * u, 36 * u);
        const QImage& sprite = egg ? pokemonIcon(0, true) : pokemonSprite(species);
        if (!sprite.isNull())
        {
            int frames = sprite.width() >= sprite.height() * 2 ? 2 : 1;
            int frameW = sprite.width() / frames;
            QImage img = sprite.copy(frames == 2 ? ((screen.animFrame / 24) % 2) * frameW : 0, 0, frameW, sprite.height() / (egg ? 2 : 1));
            qreal size = std::min(panel.width() * 0.7, panel.height() * 0.48);
            drawImage(QRectF(panel.center().x() - size / 2, panel.top() + 20 * u, size, size), img);
        }
        qreal y = panel.top() + panel.height() * 0.52;
        font.setPixelSize((int)(46 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(panel.left(), y, panel.width(), 60 * u), Qt::AlignCenter, text(kBpMons + selected * kBpMonSize + 12, 12));
        y += 62 * u;
        if (!egg)
        {
            font.setPixelSize((int)(32 * u));
            p.setFont(font);
            QString line = QString("N. %1").arg(m[2]);
            if (m[3] < 2)
                line += m[3] == 0 ? QString::fromUtf8("   ♂") : QString::fromUtf8("   ♀");
            p.drawText(QRectF(panel.left(), y, panel.width(), 44 * u), Qt::AlignCenter, line);
            y += 60 * u;
            int t1 = scr[kBpTypes], t2 = scr[kBpTypes + 1];
            qreal bw = 150 * u, bh = 42 * u;
            if (t1 == t2)
                DrawTypeBadge(p, QRectF(panel.center().x() - bw / 2, y, bw, bh), t1, u);
            else
            {
                DrawTypeBadge(p, QRectF(panel.center().x() - bw - 8 * u, y, bw, bh), t1, u);
                DrawTypeBadge(p, QRectF(panel.center().x() + 8 * u, y, bw, bh), t2, u);
            }
            y += 70 * u;
            int hp = ReadU16(m + 4), maxHp = ReadU16(m + 6);
            hpBar(QRectF(panel.left() + 50 * u, y, panel.width() - 100 * u, 24 * u), hp, maxHp);
            font.setPixelSize((int)(30 * u));
            p.setFont(font);
            p.setPen(QColor(60, 66, 80));
            p.drawText(QRectF(panel.left(), y + 30 * u, panel.width() - 50 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight,
                       QString("%1 / %2").arg(hp).arg(maxHp));
        }
    };
    // A move card (name, type, PP)
    const char* categories[] = { "Physique", "Spécial", "Statut" };
    auto moveCard = [&](const QRectF& r, int i, bool selectedCard, bool details) {
        int move = ReadU16(scr + kBpMoves + i * 2);
        if (move == 0)
        {
            p.setPen(QPen(QColor(255, 255, 255, 50), 2 * u, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 28 * u, 28 * u);
            return;
        }
        int type = scr[kBpMoveTypes + i];
        QRectF box = selectedCard ? r.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : r;
        DrawAccentCard(p, box, 28 * u, i == 4 ? QColor(255, 246, 220) : QColor(250, 252, 255, 240), TypeColor(type), 18 * u,
                       selectedCard ? QPen(QColor(255, 196, 60), 6 * u) : QPen(Qt::NoPen));
        font.setPixelSize((int)(34 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        QRectF nameRect(box.left() + 40 * u, box.top(), box.width() - 260 * u, details ? box.height() / 2 + 10 * u : box.height());
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, text(kBpMoveNames + i * kBpNameLen * 2, kBpNameLen));
        font.setPixelSize((int)(28 * u));
        p.setFont(font);
        p.drawText(QRectF(box.right() - 220 * u, box.top(), 196 * u, nameRect.height()), Qt::AlignVCenter | Qt::AlignRight,
                   QString("PP %1/%2").arg(scr[kBpPp + i]).arg(scr[kBpMaxPp + i]));
        if (details)
        {
            QString d = QString::fromUtf8("%1  ·  %2").arg(QString::fromUtf8(TypeName(type))).arg(QString::fromUtf8(categories[std::min<int>(scr[kBpMoveCategory + i], 2)]));
            if (scr[kBpMovePower + i] > 1)
                d += QString::fromUtf8("  ·  Puiss. %1").arg(scr[kBpMovePower + i]);
            if (scr[kBpMoveAccuracy + i] > 0)
                d += QString::fromUtf8("  ·  Préc. %1").arg(scr[kBpMoveAccuracy + i]);
            font.setBold(false);
            font.setPixelSize((int)(26 * u));
            p.setFont(font);
            p.setPen(QColor(100, 110, 128));
            p.drawText(QRectF(box.left() + 40 * u, box.center().y(), box.width() - 60 * u, box.height() / 2 - 8 * u), Qt::AlignVCenter | Qt::AlignLeft, d);
        }
    };
    auto messageBox = [&](const QRectF& box) {
        p.setPen(QPen(QColor(255, 255, 255, 60), 3 * u));
        p.setBrush(QColor(18, 28, 52, 245));
        p.drawRoundedRect(box, 30 * u, 30 * u);
        font.setPixelSize((int)(38 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box.adjusted(44 * u, 10 * u, -44 * u, -10 * u), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text(kBpMessage, 96));
    };

    const QColor kBack(220, 80, 80), kBlue(56, 120, 210), kGreen(60, 160, 90), kGrey(90, 104, 130);
    const qreal bottomY = height - 150 * u;
    bool messageDrawn = false;

    if (screenId == 0)
    {
        // Party: 2 columns x 3 rows, slot 0 top-left, slot 1 top-right
        title(QString::fromUtf8("Équipe"));
        const qreal gridTop = top + 110 * u;
        const qreal cardW = (contentW - gap) / 2, cardH = (bottomY - gridTop - 40 * u - 2 * gap) / 3;
        for (int i = 0; i < 6; i++)
        {
            QRectF r(left + (i % 2) * (cardW + gap), gridTop + (i / 2) * (cardH + gap), cardW, cardH);
            const u8* m = mon(i);
            int species = ReadU16(m);
            if (i >= scr[kBpCount] || species == 0)
            {
                p.setPen(QPen(QColor(255, 255, 255, 50), 2 * u, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r, 32 * u, 32 * u);
                continue;
            }
            bool sel = cursor == i;
            QRectF box = sel ? r.adjusted(-6 * u, -6 * u, 6 * u, 6 * u) : r;
            int hp = ReadU16(m + 4), maxHp = ReadU16(m + 6);
            bool egg = m[11] != 0;
            p.setPen(sel ? QPen(QColor(255, 196, 60), 6 * u) : QPen(Qt::NoPen));
            p.setBrush(hp == 0 && !egg ? QColor(250, 222, 222, 240) : QColor(250, 252, 255, sel ? 255 : 236));
            p.drawRoundedRect(box, 32 * u, 32 * u);
            const QImage& icon = pokemonIcon(species, egg);
            if (!icon.isNull())
            {
                int frame = (screen.animFrame / (sel ? 8 : 16)) % 2;
                drawImage(QRectF(box.left() + 20 * u, box.center().y() - 70 * u, 140 * u, 140 * u), icon.copy(0, frame * 32, 32, 32));
            }
            font.setPixelSize((int)(38 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(box.left() + 170 * u, box.top() + 14 * u, box.width() - 300 * u, box.height() * 0.45), Qt::AlignVCenter | Qt::AlignLeft,
                       text(kBpMons + i * kBpMonSize + 12, 12));
            if (!egg)
            {
                font.setPixelSize((int)(30 * u));
                p.setFont(font);
                p.drawText(QRectF(box.right() - 160 * u, box.top() + 14 * u, 136 * u, box.height() * 0.45), Qt::AlignVCenter | Qt::AlignRight,
                           QString("N. %1").arg(m[2]));
                QRectF bar(box.left() + 170 * u, box.top() + box.height() * 0.58, box.width() - 380 * u, 22 * u);
                hpBar(bar, hp, maxHp);
                font.setPixelSize((int)(28 * u));
                font.setBold(false);
                p.setFont(font);
                p.setPen(QColor(70, 80, 96));
                p.drawText(QRectF(bar.right() + 12 * u, bar.center().y() - 20 * u, 190 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft,
                           QString("%1/%2").arg(hp).arg(maxHp));
                QColor statusColor;
                QString status = StatusLabel(hp == 0 ? 6 : m[10], statusColor);
                if (!status.isEmpty())
                {
                    QRectF badge(box.left() + 170 * u, bar.bottom() + 12 * u, 90 * u, 34 * u);
                    p.setPen(Qt::NoPen);
                    p.setBrush(statusColor);
                    p.drawRoundedRect(badge, 17 * u, 17 * u);
                    font.setPixelSize((int)(22 * u));
                    font.setBold(true);
                    p.setFont(font);
                    p.setPen(Qt::white);
                    p.drawText(badge, Qt::AlignCenter, status);
                }
                int item = ReadU16(m + 8);
                if (item)
                    drawImage(QRectF(box.left() + 120 * u, box.bottom() - 70 * u, 50 * u, 50 * u), itemIcon(item));
            }
        }
        QRectF back(left + contentW - 280 * u, bottomY, 280 * u, 96 * u);
        if (scr[kBpMode] != 1)
            button(back, "Retour", cursor == 6, kBack);
        if (scr[kBpMessageShown])
        {
            messageBox(QRectF(left, bottomY - 10 * u, contentW - 320 * u, 116 * u));
            messageDrawn = true;
        }
    }
    else if (screenId == 1)
    {
        // Selected Pokémon: send it out, summary, moves
        title(QString::fromUtf8("Équipe"));
        QRectF header(left, top + 110 * u, contentW * 0.42, bottomY - top - 110 * u + 96 * u);
        monHeader(header);
        qreal bx = header.right() + 60 * u, bw = left + contentW - bx;
        qreal bh = 120 * u;
        qreal by = header.top() + 40 * u;
        button(QRectF(bx, by, bw, bh * 1.4), QString::fromUtf8("Envoyer au combat"), cursor == 0, kGreen, 44);
        by += bh * 1.4 + 40 * u;
        button(QRectF(bx, by, bw, bh), QString::fromUtf8("Résumé"), cursor == 1, kBlue);
        by += bh + 30 * u;
        button(QRectF(bx, by, bw, bh), QString::fromUtf8("Capacités"), cursor == 2, kBlue);
        button(QRectF(bx + bw - 280 * u, bottomY, 280 * u, 96 * u), "Retour", cursor == 3, kBack);
    }
    else if (screenId == 2)
    {
        // Summary: stats and ability
        title(QString::fromUtf8("Résumé"));
        QRectF header(left, top + 110 * u, contentW * 0.36, bottomY - top - 150 * u);
        monHeader(header);
        QRectF panel(header.right() + 40 * u, header.top(), left + contentW - header.right() - 40 * u, header.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(250, 252, 255, 240));
        p.drawRoundedRect(panel, 36 * u, 36 * u);
        const char* statNames[] = { "Attaque", "Défense", "Atq. Spé.", "Déf. Spé.", "Vitesse" };
        qreal y = panel.top() + 40 * u;
        for (int i = 0; i < 5; i++)
        {
            int value = ReadU16(scr + kBpStats + i * 2);
            font.setPixelSize((int)(32 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(90, 100, 118));
            p.drawText(QRectF(panel.left() + 50 * u, y, 260 * u, 50 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(statNames[i]));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(panel.left() + 310 * u, y, 100 * u, 50 * u), Qt::AlignVCenter | Qt::AlignRight, QString::number(value));
            QRectF bar(panel.left() + 440 * u, y + 17 * u, panel.width() - 490 * u, 16 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 226, 236));
            p.drawRoundedRect(bar, 8 * u, 8 * u);
            p.setBrush(QColor(80, 150, 220));
            p.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max<qreal>(bar.height(), bar.width() * std::min(1.0, value / 255.0)), bar.height()), 8 * u, 8 * u);
            y += 64 * u;
        }
        y += 20 * u;
        auto row = [&](const QString& label, const QString& value) {
            font.setPixelSize((int)(32 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(90, 100, 118));
            p.drawText(QRectF(panel.left() + 50 * u, y, 260 * u, 50 * u), Qt::AlignVCenter | Qt::AlignLeft, label);
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(panel.left() + 310 * u, y, panel.width() - 360 * u, 50 * u), Qt::AlignVCenter | Qt::AlignLeft, value);
            y += 64 * u;
        };
        row("Talent", text(kBpAbilityName, kBpNameLen));
        int item = ReadU16(mon(selected) + 8);
        row("Objet", item ? text(kBpItemName, kBpNameLen) : QString::fromUtf8("Aucun"));
        qreal by = bottomY;
        button(QRectF(left, by, 130 * u, 96 * u), QString::fromUtf8("▲"), cursor == 0, kGrey);
        button(QRectF(left + 150 * u, by, 130 * u, 96 * u), QString::fromUtf8("▼"), cursor == 1, kGrey);
        button(QRectF(left + 320 * u, by, 360 * u, 96 * u), QString::fromUtf8("Capacités"), cursor == 2, kBlue);
        button(QRectF(left + contentW - 280 * u, by, 280 * u, 96 * u), "Retour", cursor == 3, kBack);
    }
    else
    {
        // Moves: list (3), details (4), restore PP (5), learn a move (6, 7)
        bool learn = screenId == 6 || screenId == 7;
        title(screenId == 5 ? QString::fromUtf8("Restaurer les PP") : learn ? QString::fromUtf8("Nouvelle capacité") : QString::fromUtf8("Capacités"));
        QRectF header(left, top + 110 * u, contentW * 0.32, bottomY - top - 150 * u);
        monHeader(header);
        qreal mx = header.right() + 40 * u, mw = left + contentW - mx;
        int count = learn ? 5 : 4;
        qreal mh = (header.height() - (count - 1) * 18 * u) / count;
        for (int i = 0; i < count; i++)
        {
            bool sel = screenId == 7 ? false : cursor == i;
            if (screenId == 4)
                sel = cursor == i;
            moveCard(QRectF(mx, header.top() + i * (mh + 18 * u), mw, mh), i, sel, true);
        }
        qreal by = bottomY;
        if (screenId == 3)
        {
            button(QRectF(left, by, 130 * u, 96 * u), QString::fromUtf8("▲"), cursor == 4, kGrey);
            button(QRectF(left + 150 * u, by, 130 * u, 96 * u), QString::fromUtf8("▼"), cursor == 5, kGrey);
            button(QRectF(left + 320 * u, by, 360 * u, 96 * u), QString::fromUtf8("Résumé"), cursor == 6, kBlue);
            button(QRectF(left + contentW - 280 * u, by, 280 * u, 96 * u), "Retour", cursor == 7, kBack);
        }
        else if (screenId == 4 || screenId == 5)
            button(QRectF(left + contentW - 280 * u, by, 280 * u, 96 * u), "Retour", cursor == 4, kBack);
        else if (screenId == 6)
            button(QRectF(left + contentW - 280 * u, by, 280 * u, 96 * u), "Retour", cursor == 6, kBack);
        else
        {
            button(QRectF(left, by, 520 * u, 96 * u), QString::fromUtf8("Oublier cette capacité"), cursor == 0, kGreen);
            button(QRectF(left + contentW - 280 * u, by, 280 * u, 96 * u), "Retour", cursor == 2, kBack);
        }
    }

    if (scr[kBpMessageShown] && !messageDrawn)
        messageBox(QRectF(left, height - 300 * u, contentW, 150 * u));
    p.restore();
}

// ---------------------------------------------------------------------------
// Options (CoopUiOptionsScreen)
// ---------------------------------------------------------------------------

enum
{
    kOptCursor = 1,
    kOptValues = 2,      // u8[6]
    kOptConfirm = 12,
    kOptNumChoices = 13, // u8[5]
    kOptNames = 20,      // u16[6][24]
    kOptChoices = 308,   // u16[5][3][16]
    kOptQuit = 788,
    kOptConfirmLabel = 820,
    kOptTitle = 852,
    kOptNameLen = 24,
    kOptValueLen = 16,
};

void GameUi::drawOptions(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int cursor = scr[kOptCursor];
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) {
        QString t = DecodeGameText((const u16*)(scr + offset), len).simplified();
        // The game writes its labels in capitals: show them like BDSP (words like "L=A" stay as they are)
        QStringList words = t.split(' ');
        for (int w = 0; w < words.size(); w++)
        {
            if (words[w].size() > 1 && !words[w].contains('='))
                words[w] = (w == 0 ? words[w].left(1) : words[w].left(1).toLower()) + words[w].mid(1).toLower();
        }
        return words.join(' ');
    };

    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(84, 98, 130));
    bg.setColorAt(1, QColor(30, 36, 58));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 160 * u, 1500 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 60 * u;

    font.setPixelSize((int)(56 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, text(kOptTitle, kOptNameLen));

    // One row per setting: its name on the left, its choices on the right
    const qreal rowsTop = top + 120 * u;
    const qreal rowH = 104 * u, rowGap = 18 * u;
    const qreal choicesX = left + contentW * 0.40;
    const qreal choicesW = left + contentW - 40 * u - choicesX;
    for (int i = 0; i < 6; i++)
    {
        QRectF row(left, rowsTop + i * (rowH + rowGap), contentW, rowH);
        bool sel = cursor == i;
        QPainterPath shape;
        shape.addRoundedRect(row, 30 * u, 30 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? QColor(255, 255, 255) : QColor(255, 255, 255, 215));
        p.drawPath(shape);
        if (sel)
        {
            p.setPen(QPen(QColor(255, 196, 60), 6 * u));
            p.setBrush(Qt::NoBrush);
            p.drawPath(shape);
        }
        font.setPixelSize((int)(36 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(row.left() + 50 * u, row.top(), choicesX - row.left() - 60 * u, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   text(kOptNames + i * kOptNameLen * 2, kOptNameLen));

        int value = scr[kOptValues + i];
        if (i == 5)
        {
            // Frame: 20 choices, shown as a carousel
            QRectF pill(choicesX + choicesW / 2 - 170 * u, row.center().y() - 32 * u, 340 * u, 64 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(70, 110, 190));
            p.drawRoundedRect(pill, 32 * u, 32 * u);
            font.setPixelSize((int)(32 * u));
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(pill, Qt::AlignCenter, QString::fromUtf8("Cadre %1").arg(value + 1));
            p.setPen(sel ? QColor(70, 110, 190) : QColor(170, 176, 190));
            p.drawText(QRectF(pill.left() - 90 * u, pill.top(), 70 * u, pill.height()), Qt::AlignCenter, QString::fromUtf8("◀"));
            p.drawText(QRectF(pill.right() + 20 * u, pill.top(), 70 * u, pill.height()), Qt::AlignCenter, QString::fromUtf8("▶"));
            continue;
        }
        int count = std::max(1, std::min<int>(scr[kOptNumChoices + i], 3));
        qreal gap = 16 * u;
        qreal w = (choicesW - gap * (count - 1)) / count;
        for (int c = 0; c < count; c++)
        {
            QRectF pill(choicesX + c * (w + gap), row.center().y() - 32 * u, w, 64 * u);
            bool chosen = c == value;
            p.setPen(Qt::NoPen);
            p.setBrush(chosen ? QColor(70, 110, 190) : QColor(226, 230, 238));
            p.drawRoundedRect(pill, 32 * u, 32 * u);
            font.setPixelSize((int)(30 * u));
            font.setBold(chosen);
            p.setFont(font);
            p.setPen(chosen ? Qt::white : QColor(110, 118, 134));
            p.drawText(pill, Qt::AlignCenter, text(kOptChoices + (i * 3 + c) * kOptValueLen * 2, kOptValueLen));
        }
    }

    // Last row: quit / confirm
    const qreal by = rowsTop + 6 * (rowH + rowGap) + 10 * u;
    const bool onButtons = cursor == 6;
    const int confirm = scr[kOptConfirm];
    auto button = [&](const QRectF& r, const QString& label, bool selectedButton, const QColor& color) {
        QRectF box = selectedButton ? r.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : r;
        p.setPen(selectedButton ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
        p.setBrush(selectedButton ? color : color.darker(140));
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        font.setPixelSize((int)(36 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    };
    const qreal bw = 340 * u, bh = 96 * u;
    button(QRectF(left + contentW - 2 * bw - 30 * u, by, bw, bh), text(kOptQuit, kOptValueLen), onButtons && confirm == 0, QColor(220, 80, 80));
    button(QRectF(left + contentW - bw, by, bw, bh), text(kOptConfirmLabel, kOptValueLen), onButtons && confirm == 1, QColor(60, 160, 90));
    DrawButtonHint(p, QPointF(left, by + 23 * u), 50 * u, QString::fromUtf8("◀▶"), "Changer", QColor(90, 110, 140), Qt::white);
    p.restore();
}

// ---------------------------------------------------------------------------
// Trainer card (CoopUiTrainerCardScreen)
// ---------------------------------------------------------------------------

enum
{
    kTcGender = 1,
    kTcBadgeCount = 2,
    kTcDexEnabled = 3,
    kTcTrainerId = 12,
    kTcDexOwned = 14,
    kTcMoney = 16,
    kTcScore = 20,
    kTcHours = 24,
    kTcMinutes = 26,
    kTcBadges = 28,
    kTcName = 32,
};

void GameUi::drawTrainerCard(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    QFont font("Segoe UI");
    const bool female = scr[kTcGender] == 1;
    const QColor accent = female ? QColor(220, 90, 130) : QColor(60, 120, 210);

    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, accent.lighter(120));
    bg.setColorAt(1, accent.darker(260));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    // The card
    const qreal cardW = std::min<qreal>(width - 200 * u, 1500 * u);
    const qreal cardH = std::min<qreal>(height - 220 * u, 860 * u);
    QRectF card((width - cardW) / 2, (height - cardH) / 2 - 30 * u, cardW, cardH);
    QPainterPath shape;
    shape.addRoundedRect(card, 48 * u, 48 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawRoundedRect(card.translated(0, 14 * u), 48 * u, 48 * u);
    p.setBrush(QColor(250, 251, 255));
    p.drawPath(shape);
    p.save();
    p.setClipPath(shape);
    QLinearGradient head(card.topLeft(), card.topRight());
    head.setColorAt(0, accent);
    head.setColorAt(1, accent.darker(150));
    p.setBrush(head);
    p.drawRect(QRectF(card.left(), card.top(), card.width(), 150 * u));
    p.restore();

    font.setPixelSize((int)(40 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(card.left() + 60 * u, card.top(), card.width() * 0.6, 150 * u), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("CARTE DRESSEUR"));
    font.setPixelSize((int)(34 * u));
    font.setBold(false);
    p.setFont(font);
    p.drawText(QRectF(card.left(), card.top(), card.width() - 60 * u, 150 * u), Qt::AlignVCenter | Qt::AlignRight,
               QString("ID  %1").arg(ReadU16(scr + kTcTrainerId), 5, 10, QChar('0')));

    // Avatar and name
    QString name = DecodeGameText((const u16*)(scr + kTcName), 8).simplified();
    QRectF avatar(card.left() + 70 * u, card.top() + 200 * u, 220 * u, 220 * u);
    QRadialGradient glow(avatar.center(), avatar.width() / 2);
    glow.setColorAt(0, accent.lighter(140));
    glow.setColorAt(1, accent);
    p.setBrush(glow);
    p.drawEllipse(avatar);
    font.setPixelSize((int)(110 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(avatar, Qt::AlignCenter, name.left(1).toUpper());
    font.setPixelSize((int)(64 * u));
    p.setFont(font);
    p.setPen(QColor(32, 40, 56));
    p.drawText(QRectF(avatar.right() + 50 * u, avatar.top() + 20 * u, card.width() * 0.4, 90 * u), Qt::AlignVCenter | Qt::AlignLeft, name);

    // Info rows
    auto row = [&](qreal y, const QString& label, const QString& value) {
        QRectF r(avatar.right() + 50 * u, y, card.right() - avatar.right() - 110 * u, 64 * u);
        p.setPen(QPen(QColor(226, 230, 238), 2 * u));
        p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.bottom()));
        font.setPixelSize((int)(34 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(100, 110, 128));
        p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, label);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(r, Qt::AlignVCenter | Qt::AlignRight, value);
    };
    u32 money = scr[kTcMoney] | (scr[kTcMoney + 1] << 8) | (scr[kTcMoney + 2] << 16) | ((u32)scr[kTcMoney + 3] << 24);
    u32 score = scr[kTcScore] | (scr[kTcScore + 1] << 8) | (scr[kTcScore + 2] << 16) | ((u32)scr[kTcScore + 3] << 24);
    qreal y = avatar.top() + 100 * u;
    row(y, QString::fromUtf8("Argent"), FormatMoney(money)); y += 64 * u;
    if (scr[kTcDexEnabled])
    {
        row(y, QString::fromUtf8("Pokédex"), QString::number(ReadU16(scr + kTcDexOwned)));
        y += 64 * u;
    }
    row(y, QString::fromUtf8("Temps de jeu"), QString("%1:%2").arg(ReadU16(scr + kTcHours)).arg(scr[kTcMinutes], 2, 10, QChar('0'))); y += 64 * u;
    row(y, QString::fromUtf8("Score"), QString::number(score));

    // Badges: Johto then Kanto
    static const char* kBadgeNames[16] = { "Zéphyr", "Essaim", "Plaine", "Brouillard", "Choc", "Minéral", "Glacier", "Lever",
                                           "Roche", "Cascade", "Foudre", "Prisme", "Âme", "Marais", "Volcan", "Terre" };
    static const QColor kBadgeColors[16] = {
        QColor(150, 170, 200), QColor(200, 70, 70), QColor(240, 140, 170), QColor(140, 100, 190), QColor(240, 150, 60), QColor(180, 190, 200),
        QColor(120, 200, 230), QColor(60, 110, 200), QColor(150, 140, 130), QColor(80, 160, 230), QColor(240, 200, 60), QColor(120, 200, 120),
        QColor(220, 100, 180), QColor(230, 190, 70), QColor(230, 90, 60), QColor(110, 190, 90),
    };
    const u16 badges = ReadU16(scr + kTcBadges);
    const qreal badgesTop = card.bottom() - 270 * u;
    font.setPixelSize((int)(30 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(100, 110, 128));
    p.drawText(QRectF(card.left() + 70 * u, badgesTop - 56 * u, 250 * u, 46 * u), Qt::AlignVCenter | Qt::AlignLeft,
               QString::fromUtf8("Badges  ·  %1 / 16").arg(scr[kTcBadgeCount]));
    const qreal slotW = (card.width() - 140 * u) / 8;
    for (int i = 0; i < 16; i++)
    {
        int col = i % 8, line = i / 8;
        QPointF c(card.left() + 70 * u + slotW * (col + 0.5), badgesTop + 46 * u + line * 128 * u);
        qreal r = 34 * u;
        bool owned = badges & (1 << i);
        p.setPen(owned ? QPen(Qt::white, 4 * u) : QPen(QColor(210, 214, 224), 3 * u, Qt::DashLine));
        p.setBrush(owned ? kBadgeColors[i] : QColor(238, 240, 245));
        p.drawEllipse(c, r, r);
        if (owned)
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 110));
            p.drawEllipse(QPointF(c.x() - r * 0.3, c.y() - r * 0.3), r * 0.3, r * 0.3);
        }
        font.setPixelSize((int)(20 * u));
        font.setBold(owned);
        p.setFont(font);
        p.setPen(owned ? QColor(50, 58, 74) : QColor(170, 176, 190));
        p.drawText(QRectF(c.x() - slotW / 2, c.y() + r + 2 * u, slotW, 28 * u), Qt::AlignCenter, QString::fromUtf8(kBadgeNames[i]));
    }

    DrawButtonHint(p, QPointF(card.left(), card.bottom() + 40 * u), 50 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    p.restore();
}

// ---------------------------------------------------------------------------
// Pokédex (CoopUiPokedexScreen)
// ---------------------------------------------------------------------------

enum
{
    kDexPage = 1,
    kDexNational = 2,
    kDexHasNational = 4,
    kDexCursor = 12,
    kDexCount = 14,
    kDexSeen = 16,
    kDexCaught = 18,
    kDexRows = 20,         // 9 x 6 bytes
    kDexRowSize = 6,
    kDexNames = 76,        // u16[9][12]
    kDexNameLen = 12,
    kDexSpecies = 292,
    kDexSpeciesState = 294,
    kDexTypes = 296,
    kDexHeight = 300,
    kDexWeight = 304,
    kDexCategory = 308,    // u16[24]
    kDexDescription = 356, // u16[160]
    kDexRowCount = 9,
    kDexSubPage = 676,
    kDexAreaCount = 677,
    kDexNumForms = 678,
    kDexAreas = 680,       // u8[40][4]
};

void GameUi::drawPokedex(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int page = scr[kDexPage];
    const bool national = scr[kDexNational] != 0;
    QFont font("Segoe UI");
    auto read32 = [&](int o) { return (u32)(scr[o] | (scr[o + 1] << 8) | (scr[o + 2] << 16) | ((u32)scr[o + 3] << 24)); };
    const QColor red(206, 46, 52), redDark(140, 20, 30);

    // Background: Pokédex red with soft stripes
    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, red);
    bg.setColorAt(1, redDark);
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 12));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 140 * u, 1700 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 50 * u;
    const QString dexName = national ? QString::fromUtf8("Pokédex national") : QString::fromUtf8("Pokédex de Johto");

    auto counters = [&](const QRectF& r) {
        // Seen / caught pills
        qreal w = (r.width() - 20 * u) / 2;
        for (int i = 0; i < 2; i++)
        {
            QRectF pill(r.left() + i * (w + 20 * u), r.top(), w, r.height());
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 235));
            p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
            font.setPixelSize((int)(r.height() * 0.36));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(100, 110, 128));
            p.drawText(pill.adjusted(pill.height() * 0.5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, i == 0 ? QString::fromUtf8("Vus") : QString::fromUtf8("Capturés"));
            font.setBold(true);
            font.setPixelSize((int)(r.height() * 0.46));
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(pill.adjusted(0, 0, -pill.height() * 0.5, 0), Qt::AlignVCenter | Qt::AlignRight,
                       QString::number(ReadU16(scr + (i == 0 ? kDexSeen : kDexCaught))));
        }
    };
    auto drawImage = [&](const QRectF& r, const QImage& img) {
        if (img.isNull())
            return;
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(r, img);
        p.restore();
    };
    auto pokeball = [&](const QPointF& c, qreal r, bool filled) {
        p.setPen(QPen(filled ? QColor(60, 66, 80) : QColor(200, 204, 214), r * 0.18));
        p.setBrush(filled ? QColor(230, 60, 60) : Qt::NoBrush);
        p.drawPie(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 0, 180 * 16);
        p.setBrush(filled ? Qt::white : Qt::NoBrush);
        p.drawPie(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 180 * 16, 180 * 16);
        p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
        p.setBrush(Qt::white);
        p.drawEllipse(c, r * 0.32, r * 0.32);
    };
    // Information on the Pokémon under the cursor
    auto speciesPanel = [&](const QRectF& panel, bool large) {
        int species = ReadU16(scr + kDexSpecies);
        int state = scr[kDexSpeciesState];
        QPainterPath shape;
        shape.addRoundedRect(panel, 40 * u, 40 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(250, 251, 255));
        p.drawPath(shape);
        int number = 0;
        QString name;
        for (int i = 0; i < kDexRowCount; i++)
        {
            const u8* row = scr + kDexRows + i * kDexRowSize;
            if (i == kDexRowCount / 2)
            {
                number = ReadU16(row + 2);
                name = DecodeGameText((const u16*)(scr + kDexNames + i * kDexNameLen * 2), kDexNameLen).simplified();
            }
        }
        // Sprite on a soft disc
        qreal spriteSize = large ? std::min(panel.height() * 0.62, panel.width() * 0.42) : std::min(panel.width() * 0.5, panel.height() * 0.38);
        QRectF spriteArea = large ? QRectF(panel.left() + 50 * u, panel.top() + 60 * u, spriteSize, spriteSize)
                                  : QRectF(panel.center().x() - spriteSize / 2, panel.top() + 30 * u, spriteSize, spriteSize);
        QRadialGradient glow(spriteArea.center(), spriteSize / 2);
        glow.setColorAt(0, QColor(255, 230, 230));
        glow.setColorAt(1, QColor(255, 230, 230, 0));
        p.setBrush(glow);
        p.drawEllipse(spriteArea);
        if (state > 0 && species > 0)
        {
            const QImage& sprite = pokemonSprite(species);
            if (!sprite.isNull())
            {
                int frames = sprite.width() >= sprite.height() * 2 ? 2 : 1;
                int frameW = sprite.width() / frames;
                drawImage(spriteArea, sprite.copy(0, 0, frameW, sprite.height()));
            }
        }
        else
        {
            font.setPixelSize((int)(spriteSize * 0.5));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(220, 200, 204));
            p.drawText(spriteArea, Qt::AlignCenter, "?");
        }
        QRectF info = large ? QRectF(spriteArea.right() + 50 * u, panel.top() + 60 * u, panel.right() - spriteArea.right() - 100 * u, panel.height() - 120 * u)
                            : QRectF(panel.left() + 50 * u, spriteArea.bottom() + 20 * u, panel.width() - 100 * u, panel.bottom() - spriteArea.bottom() - 50 * u);
        qreal y = info.top();
        font.setPixelSize((int)(30 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(red);
        p.drawText(QRectF(info.left(), y, info.width(), 40 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("N° %1").arg(number, 3, 10, QChar('0')));
        y += 44 * u;
        font.setPixelSize((int)((large ? 60 : 50) * u));
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(info.left(), y, info.width(), 70 * u), Qt::AlignVCenter | Qt::AlignLeft, state > 0 && !name.isEmpty() ? name : QString("???"));
        if (state == 2)
            pokeball(QPointF(info.right() - 26 * u, y + 35 * u), 22 * u, true);
        y += 76 * u;
        if (state < 2)
        {
            font.setPixelSize((int)(30 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(120, 128, 144));
            p.drawText(QRectF(info.left(), y, info.width(), 50 * u), Qt::AlignVCenter | Qt::AlignLeft,
                       state == 1 ? QString::fromUtf8("Vu, pas encore capturé") : QString::fromUtf8("Pas encore rencontré"));
            return;
        }
        font.setPixelSize((int)(30 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(100, 110, 128));
        p.drawText(QRectF(info.left(), y, info.width(), 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
                   DecodeGameText((const u16*)(scr + kDexCategory), 24).simplified());
        y += 56 * u;
        int t1 = scr[kDexTypes], t2 = scr[kDexTypes + 1];
        qreal bw = 160 * u, bh = 44 * u;
        DrawTypeBadge(p, QRectF(info.left(), y, bw, bh), t1, u);
        if (t2 != t1)
            DrawTypeBadge(p, QRectF(info.left() + bw + 16 * u, y, bw, bh), t2, u);
        y += bh + 24 * u;
        // Height and weight: the game stores decimeters and hectograms
        double meters = read32(kDexHeight) / 10.0;
        double kilos = read32(kDexWeight) / 10.0;
        font.setPixelSize((int)(30 * u));
        p.setFont(font);
        p.setPen(QColor(60, 66, 80));
        p.drawText(QRectF(info.left(), y, info.width(), 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromUtf8("Taille  %1 m      Poids  %2 kg").arg(meters, 0, 'f', 1).arg(kilos, 0, 'f', 1));
        y += 60 * u;
        QRectF desc(info.left(), y, info.width(), info.bottom() - y);
        if (!large)
            desc = QRectF(info.left(), y, info.width(), panel.bottom() - 30 * u - y);
        font.setPixelSize((int)((large ? 34 : 29) * u));
        p.setFont(font);
        p.setPen(QColor(50, 58, 74));
        p.drawText(desc, Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, DecodeGameText((const u16*)(scr + kDexDescription), 160).simplified());
    };

    if (page == 0)
    {
        // Cover
        QRectF card(width / 2.0 - 520 * u, height / 2.0 - 360 * u, 1040 * u, 640 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawRoundedRect(card.translated(0, 14 * u), 50 * u, 50 * u);
        p.setBrush(QColor(250, 251, 255));
        p.drawRoundedRect(card, 50 * u, 50 * u);
        pokeball(QPointF(card.center().x(), card.top() + 150 * u), 80 * u, true);
        font.setPixelSize((int)(64 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(card.left(), card.top() + 250 * u, card.width(), 90 * u), Qt::AlignCenter, dexName);
        if (scr[kDexHasNational])
        {
            font.setPixelSize((int)(46 * u));
            p.setFont(font);
            p.setPen(red);
            p.drawText(QRectF(card.left() + 40 * u, card.top() + 250 * u, 100 * u, 90 * u), Qt::AlignCenter, QString::fromUtf8("◀"));
            p.drawText(QRectF(card.right() - 140 * u, card.top() + 250 * u, 100 * u, 90 * u), Qt::AlignCenter, QString::fromUtf8("▶"));
        }
        counters(QRectF(card.left() + 120 * u, card.top() + 390 * u, card.width() - 240 * u, 100 * u));
        DrawButtonHint(p, QPointF(card.left() + 160 * u, card.bottom() - 100 * u), 50 * u, "A", "Ouvrir", QColor(80, 170, 90));
        DrawButtonHint(p, QPointF(card.left() + 600 * u, card.bottom() - 100 * u), 50 * u, "B", "Quitter", QColor(220, 80, 80));
    }
    else if (page == 1)
    {
        // List: details on the left, list on the right
        font.setPixelSize((int)(50 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(left, top, contentW * 0.5, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, dexName);
        counters(QRectF(left + contentW - 620 * u, top + 6 * u, 620 * u, 68 * u));

        const qreal bodyTop = top + 110 * u, bodyBottom = height - 110 * u;
        QRectF panel(left, bodyTop, contentW * 0.46, bodyBottom - bodyTop);
        speciesPanel(panel, false);

        QRectF listArea(panel.right() + 40 * u, bodyTop, left + contentW - panel.right() - 40 * u, bodyBottom - bodyTop);
        const qreal rowGap = 12 * u;
        const qreal rowH = (listArea.height() - rowGap * (kDexRowCount - 1)) / kDexRowCount;
        for (int i = 0; i < kDexRowCount; i++)
        {
            const u8* row = scr + kDexRows + i * kDexRowSize;
            if (!row[5])
                continue;
            int species = ReadU16(row), number = ReadU16(row + 2), state = row[4];
            bool sel = i == kDexRowCount / 2;
            QRectF r(listArea.left(), listArea.top() + i * (rowH + rowGap), listArea.width(), rowH);
            QRectF box = sel ? r.adjusted(-8 * u, -4 * u, 8 * u, 4 * u) : r;
            QPainterPath shape;
            shape.addRoundedRect(box, box.height() / 2, box.height() / 2);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QColor(255, 255, 255) : QColor(255, 255, 255, state ? 215 : 120));
            p.drawPath(shape);
            if (sel)
            {
                p.setPen(QPen(QColor(255, 196, 60), 6 * u));
                p.setBrush(Qt::NoBrush);
                p.drawPath(shape);
            }
            font.setPixelSize((int)(rowH * 0.36));
            font.setBold(true);
            p.setFont(font);
            p.setPen(state ? red : QColor(150, 140, 146));
            p.drawText(QRectF(box.left() + rowH * 0.5, box.top(), 110 * u, box.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       QString("%1").arg(number, 3, 10, QChar('0')));
            if (state && species)
            {
                const QImage& icon = pokemonIcon(species, false);
                if (!icon.isNull())
                {
                    int frame = sel ? (screen.animFrame / 8) % 2 : 0;
                    drawImage(QRectF(box.left() + rowH * 0.5 + 100 * u, box.center().y() - rowH * 0.62, rowH * 1.24, rowH * 1.24), icon.copy(0, frame * 32, 32, 32));
                }
            }
            p.setPen(state ? QColor(32, 40, 56) : QColor(150, 140, 146));
            p.drawText(QRectF(box.left() + rowH * 0.5 + 110 * u + rowH * 1.3, box.top(), box.width() - 400 * u, box.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       state ? DecodeGameText((const u16*)(scr + kDexNames + i * kDexNameLen * 2), kDexNameLen).simplified() : QString::fromUtf8("—"));
            if (state == 2)
                pokeball(QPointF(box.right() - rowH * 0.6, box.center().y()), rowH * 0.24, true);
        }
        qreal hy = height - 80 * u;
        DrawButtonHint(p, QPointF(left, hy), 46 * u, QString::fromUtf8("▲▼"), "Choisir", QColor(90, 110, 140), Qt::white);
        DrawButtonHint(p, QPointF(left + 280 * u, hy), 46 * u, QString::fromUtf8("◀▶"), QString::fromUtf8("Défiler vite"), QColor(90, 110, 140), Qt::white);
        DrawButtonHint(p, QPointF(left + 620 * u, hy), 46 * u, "A", QString::fromUtf8("Fiche"), QColor(80, 170, 90), Qt::white);
        DrawButtonHint(p, QPointF(left + 860 * u, hy), 46 * u, "Y", "Rechercher", QColor(120, 120, 140), Qt::white);
        DrawButtonHint(p, QPointF(left + 1180 * u, hy), 46 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    }
    else
    {
        // Entry
        font.setPixelSize((int)(50 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, dexName);
        // Page tabs, like the game: Zone, Taille, Formes, Retour
        const int subPage = std::min<int>(scr[kDexSubPage], 3);
        const char* tabNames[] = { "Zone", "Taille", "Formes", "Retour" };
        for (int i = 0; i < 4; i++)
        {
            QRectF tab(left + contentW - (4 - i) * 210 * u, top + 8 * u, 190 * u, 64 * u);
            bool current = i == subPage;
            p.setPen(Qt::NoPen);
            p.setBrush(current ? Qt::white : QColor(255, 255, 255, 50));
            p.drawRoundedRect(tab, 32 * u, 32 * u);
            font.setPixelSize((int)(30 * u));
            font.setBold(current);
            p.setFont(font);
            p.setPen(current ? red : Qt::white);
            p.drawText(tab, Qt::AlignCenter, QString::fromUtf8(tabNames[i]));
        }

        const QRectF body(left, top + 110 * u, contentW, height - top - 110 * u - 110 * u);
        const int species = ReadU16(scr + kDexSpecies);
        if (subPage == 0)
        {
            // Area: the Pokémon on the left, where it lives on the map on the right
            QRectF panel(body.left(), body.top(), body.width() * 0.36, body.height());
            speciesPanel(panel, false);
            QRectF mapArea(panel.right() + 40 * u, body.top(), body.right() - panel.right() - 40 * u, body.height());
            if (gearMap.isNull())
                gearMap = QImage(HoLympeAssetsDir() + "/gear_map.png");
            qreal scale = std::min(mapArea.width() / 376.0, (mapArea.height() - 90 * u) / 160.0);
            QRectF mapRect(mapArea.center().x() - 188 * scale, mapArea.top() + 20 * u, 376 * scale, 160 * scale);
            QPainterPath frame;
            frame.addRoundedRect(mapRect.adjusted(-10 * u, -10 * u, 10 * u, 10 * u), 30 * u, 30 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 235));
            p.drawPath(frame);
            QPainterPath clip;
            clip.addRoundedRect(mapRect, 22 * u, 22 * u);
            p.save();
            p.setClipPath(clip);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            if (!gearMap.isNull())
                p.drawImage(mapRect, gearMap);
            p.fillRect(mapRect, QColor(0, 0, 0, 70)); // dim the map so the areas stand out
            const qreal cell = 8 * scale;
            qreal pulse = 0.5 + 0.5 * std::sin(screen.animFrame * 0.2);
            int count = std::min<int>(scr[kDexAreaCount], 40);
            for (int i = 0; i < count; i++)
            {
                const u8* a = scr + kDexAreas + i * 4;
                QRectF r(mapRect.left() + a[0] * cell, mapRect.top() + a[1] * cell, std::max(1, (int)a[2]) * cell, std::max(1, (int)a[3]) * cell);
                p.setPen(QPen(QColor(255, 220, 80), 3 * u));
                p.setBrush(QColor(255, 200, 40, 110 + (int)(70 * pulse)));
                p.drawRoundedRect(r.adjusted(1 * u, 1 * u, -1 * u, -1 * u), 6 * u, 6 * u);
            }
            p.restore();
            font.setPixelSize((int)(32 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(QRectF(mapArea.left(), mapRect.bottom() + 30 * u, mapArea.width(), 50 * u), Qt::AlignCenter,
                       count > 0 ? QString::fromUtf8("Zones où vit ce Pokémon à cette heure de la journée")
                                 : QString::fromUtf8("Zone d'habitat inconnue"));
        }
        else if (subPage == 1)
        {
            // Size: the Pokémon next to the player (the game compares with 1.5 m and 40 kg)
            QPainterPath shape;
            shape.addRoundedRect(body, 40 * u, 40 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(250, 251, 255));
            p.drawPath(shape);
            double monH = read32(kDexHeight) / 10.0, monW = read32(kDexWeight) / 10.0;
            const double playerH = 1.5, playerW = 40.0;
            QRectF stage(body.left() + 60 * u, body.top() + 60 * u, body.width() * 0.55, body.height() - 200 * u);
            double tallest = std::max(monH, playerH);
            qreal ground = stage.bottom();
            p.setPen(QPen(QColor(210, 214, 224), 3 * u));
            p.drawLine(QPointF(stage.left(), ground), QPointF(stage.right(), ground));
            // Player silhouette
            qreal ph = stage.height() * playerH / tallest;
            QRectF person(stage.left() + stage.width() * 0.62, ground - ph, ph * 0.38, ph);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(60, 66, 80));
            p.drawEllipse(QRectF(person.center().x() - ph * 0.09, person.top(), ph * 0.18, ph * 0.18));
            p.drawRoundedRect(QRectF(person.left() + person.width() * 0.18, person.top() + ph * 0.2, person.width() * 0.64, ph * 0.45), ph * 0.06, ph * 0.06);
            p.drawRoundedRect(QRectF(person.left() + person.width() * 0.22, person.top() + ph * 0.6, person.width() * 0.24, ph * 0.4), ph * 0.04, ph * 0.04);
            p.drawRoundedRect(QRectF(person.left() + person.width() * 0.54, person.top() + ph * 0.6, person.width() * 0.24, ph * 0.4), ph * 0.04, ph * 0.04);
            // Pokémon silhouette (the sprite filled with one color)
            const QImage& sprite = pokemonSprite(species);
            if (!sprite.isNull() && monH > 0)
            {
                int frames = sprite.width() >= sprite.height() * 2 ? 2 : 1;
                QImage img = sprite.copy(0, 0, sprite.width() / frames, sprite.height()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                // Trim the transparent rows so the height matches the real size
                int topRow = img.height(), bottomRow = 0;
                for (int y = 0; y < img.height(); y++)
                    for (int x = 0; x < img.width(); x++)
                        if (qAlpha(img.pixel(x, y)) > 0)
                        {
                            topRow = std::min(topRow, y);
                            bottomRow = std::max(bottomRow, y);
                        }
                if (bottomRow >= topRow)
                    img = img.copy(0, topRow, img.width(), bottomRow - topRow + 1);
                QPainter ip(&img);
                ip.setCompositionMode(QPainter::CompositionMode_SourceIn);
                ip.fillRect(img.rect(), QColor(206, 46, 52));
                ip.end();
                qreal mh = stage.height() * monH / tallest;
                qreal mw = mh * img.width() / std::max(1, img.height());
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(QRectF(stage.left() + stage.width() * 0.28 - mw / 2, ground - mh, mw, mh), img);
                p.restore();
            }
            // Numbers
            QRectF info(stage.right() + 60 * u, body.top() + 80 * u, body.right() - stage.right() - 120 * u, body.height() - 160 * u);
            auto row = [&](qreal y, const QString& label, double mon, double player, const QString& unit) {
                font.setPixelSize((int)(32 * u));
                font.setBold(true);
                p.setFont(font);
                p.setPen(QColor(100, 110, 128));
                p.drawText(QRectF(info.left(), y, info.width(), 44 * u), Qt::AlignVCenter | Qt::AlignLeft, label);
                const QString names[2] = { DecodeGameText((const u16*)(scr + kDexNames + (kDexRowCount / 2) * kDexNameLen * 2), kDexNameLen).simplified(),
                                           QString::fromUtf8("Toi") };
                const double values[2] = { mon, player };
                const QColor colors[2] = { red, QColor(60, 66, 80) };
                double maxValue = std::max(mon, player);
                for (int i = 0; i < 2; i++)
                {
                    qreal yy = y + 60 * u + i * 70 * u;
                    font.setPixelSize((int)(28 * u));
                    font.setBold(false);
                    p.setFont(font);
                    p.setPen(QColor(50, 58, 74));
                    p.drawText(QRectF(info.left(), yy, 200 * u, 50 * u), Qt::AlignVCenter | Qt::AlignLeft, names[i]);
                    QRectF bar(info.left() + 210 * u, yy + 15 * u, info.width() - 400 * u, 20 * u);
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(226, 230, 238));
                    p.drawRoundedRect(bar, 10 * u, 10 * u);
                    p.setBrush(colors[i]);
                    p.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max<qreal>(bar.height(), bar.width() * values[i] / std::max(0.1, maxValue)), bar.height()), 10 * u, 10 * u);
                    font.setBold(true);
                    p.setFont(font);
                    p.setPen(QColor(32, 40, 56));
                    p.drawText(QRectF(bar.right() + 10 * u, yy, 180 * u, 50 * u), Qt::AlignVCenter | Qt::AlignRight,
                               QString("%1 %2").arg(values[i], 0, 'f', 1).arg(unit));
                }
            };
            row(info.top(), QString::fromUtf8("Taille"), monH, playerH, "m");
            row(info.top() + 260 * u, QString::fromUtf8("Poids"), monW, playerW, "kg");
        }
        else if (subPage == 2)
        {
            // Forms
            QRectF panel(body.left(), body.top(), body.width() * 0.5, body.height());
            speciesPanel(panel, false);
            QRectF side(panel.right() + 40 * u, body.top(), body.right() - panel.right() - 40 * u, body.height());
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(250, 251, 255));
            p.drawRoundedRect(side, 40 * u, 40 * u);
            font.setPixelSize((int)(110 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(red);
            p.drawText(QRectF(side.left(), side.top() + 80 * u, side.width(), 140 * u), Qt::AlignCenter, QString::number(std::max<int>(1, scr[kDexNumForms])));
            font.setPixelSize((int)(36 * u));
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(60, 66, 80));
            p.drawText(QRectF(side.left(), side.top() + 230 * u, side.width(), 60 * u), Qt::AlignCenter,
                       scr[kDexNumForms] > 1 ? QString::fromUtf8("formes déjà vues") : QString::fromUtf8("forme déjà vue"));
        }
        else
        {
            // Back to the list
            QRectF card(width / 2.0 - 500 * u, body.center().y() - 160 * u, 1000 * u, 320 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(250, 251, 255));
            p.drawRoundedRect(card, 40 * u, 40 * u);
            font.setPixelSize((int)(52 * u));
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(32, 40, 56));
            p.drawText(QRectF(card.left(), card.top() + 60 * u, card.width(), 80 * u), Qt::AlignCenter, QString::fromUtf8("Retourner à la liste ?"));
            DrawButtonHint(p, QPointF(card.left() + 300 * u, card.bottom() - 110 * u), 50 * u, "A", "Oui", QColor(80, 170, 90));
            DrawButtonHint(p, QPointF(card.left() + 560 * u, card.bottom() - 110 * u), 50 * u, QString::fromUtf8("◀"), "Non", QColor(90, 110, 140));
        }
        qreal hy = height - 80 * u;
        DrawButtonHint(p, QPointF(left, hy), 46 * u, QString::fromUtf8("◀▶"), QString::fromUtf8("Pages"), QColor(90, 110, 140), Qt::white);
        DrawButtonHint(p, QPointF(left + 300 * u, hy), 46 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Pokégear phone (CoopUiPhoneScreen)
// ---------------------------------------------------------------------------

enum
{
    kPhInTabs = 2,
    kPhSelected = 3,
    kPhCount = 4,
    kPhMenuCount = 5,
    kPhMenuCursor = 6,
    kPhInCall = 7,
    kPhHour = 12,
    kPhMinute = 13,
    kPhTabCount = 14,
    kPhTabCursor = 15,
    kPhTabs = 16,
    kPhRowValid = 22,
    kPhNames = 30,       // u16[7][12]
    kPhClasses = 198,    // u16[7][16]
    kPhMenuItems = 422,  // u16[6][12]
    kPhMessage = 566,    // u16[200]
    kPhRows = 7,
    kPhNameLen = 12,
    kPhClassLen = 16,
};

void GameUi::drawPhone(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) {
        QString t = DecodeGameText((const u16*)(scr + offset), len).simplified();
        return t;
    };
    const QColor accent(40, 150, 220);

    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(28, 60, 120));
    bg.setColorAt(1, QColor(10, 20, 48));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 10));
    for (qreal x = -height; x < width; x += 140 * u)
    {
        QPolygonF stripe;
        stripe << QPointF(x, height) << QPointF(x + 60 * u, height) << QPointF(x + 60 * u + height, 0) << QPointF(x + height, 0);
        p.drawPolygon(stripe);
    }

    const qreal contentW = std::min<qreal>(width - 160 * u, 1500 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 50 * u;

    // Header: Pokégear and the clock
    font.setPixelSize((int)(50 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Pokégear  ·  Téléphone"));
    font.setPixelSize((int)(64 * u));
    p.setFont(font);
    p.setPen(QColor(255, 210, 90));
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignRight,
               QString("%1:%2").arg(scr[kPhHour], 2, 10, QChar('0')).arg(scr[kPhMinute], 2, 10, QChar('0')));

    // Contacts
    const bool inTabs = scr[kPhInTabs] != 0;
    const qreal listTop = top + 120 * u;
    const qreal tabsH = 110 * u;
    const qreal listBottom = height - tabsH - 80 * u;
    const qreal gap = 14 * u;
    const qreal rowH = (listBottom - listTop - gap * (kPhRows - 1)) / kPhRows;
    const qreal listW = contentW * 0.62;
    QRectF selectedRow;
    for (int i = 0; i < kPhRows; i++)
    {
        if (!scr[kPhRowValid + i])
            continue;
        bool sel = i == kPhRows / 2 && !inTabs;
        QRectF r(left, listTop + i * (rowH + gap), listW, rowH);
        QRectF box = sel ? r.adjusted(-6 * u, -4 * u, 6 * u, 4 * u) : r;
        if (i == kPhRows / 2)
            selectedRow = box;
        QPainterPath shape;
        shape.addRoundedRect(box, 26 * u, 26 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? QColor(255, 255, 255) : QColor(255, 255, 255, 210));
        p.drawPath(shape);
        if (sel)
        {
            p.setPen(QPen(QColor(255, 196, 60), 6 * u));
            p.setBrush(Qt::NoBrush);
            p.drawPath(shape);
        }
        // Phone icon
        QRectF icon(box.left() + 24 * u, box.center().y() - rowH * 0.32, rowH * 0.64, rowH * 0.64);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(icon);
        font.setPixelSize((int)(rowH * 0.34));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(icon, Qt::AlignCenter, text(kPhNames + i * kPhNameLen * 2, kPhNameLen).left(1));
        font.setPixelSize((int)(rowH * 0.36));
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(icon.right() + 24 * u, box.top(), box.width() * 0.5, box.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   text(kPhNames + i * kPhNameLen * 2, kPhNameLen));
        font.setPixelSize((int)(rowH * 0.28));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(100, 110, 128));
        p.drawText(QRectF(box.left(), box.top(), box.width() - 30 * u, box.height()), Qt::AlignVCenter | Qt::AlignRight,
                   text(kPhClasses + i * kPhClassLen * 2, kPhClassLen));
    }
    // Count on the right
    QRectF side(left + listW + 50 * u, listTop, contentW - listW - 50 * u, listBottom - listTop);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 30));
    p.drawRoundedRect(side, 36 * u, 36 * u);
    font.setPixelSize((int)(110 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(side.left(), side.top() + 60 * u, side.width(), 140 * u), Qt::AlignCenter, QString::number(scr[kPhCount]));
    font.setPixelSize((int)(34 * u));
    font.setBold(false);
    p.setFont(font);
    p.drawText(QRectF(side.left(), side.top() + 200 * u, side.width(), 50 * u), Qt::AlignCenter, scr[kPhCount] == 1 ? "contact" : "contacts");

    // Context menu next to the selected contact
    int menuCount = scr[kPhMenuCount];
    if (menuCount > 0 && !selectedRow.isNull())
    {
        qreal itemH = 78 * u;
        QRectF menu(side.left(), std::min(selectedRow.top(), listBottom - menuCount * itemH - 30 * u), side.width(), menuCount * itemH + 30 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawRoundedRect(menu.translated(0, 10 * u), 30 * u, 30 * u);
        p.setBrush(QColor(250, 252, 255));
        p.drawRoundedRect(menu, 30 * u, 30 * u);
        for (int i = 0; i < menuCount; i++)
        {
            QRectF item(menu.left() + 15 * u, menu.top() + 15 * u + i * itemH, menu.width() - 30 * u, itemH);
            bool sel = i == scr[kPhMenuCursor];
            if (sel)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(accent);
                p.drawRoundedRect(item.adjusted(0, 4 * u, 0, -4 * u), 24 * u, 24 * u);
            }
            QString label = text(kPhMenuItems + i * kPhNameLen * 2, kPhNameLen);
            label = label.left(1).toUpper() + label.mid(1).toLower();
            font.setPixelSize((int)(34 * u));
            font.setBold(sel);
            p.setFont(font);
            p.setPen(sel ? Qt::white : QColor(50, 58, 74));
            p.drawText(item, Qt::AlignCenter, label);
        }
    }

    // Apps bar
    static const char* kAppNames[] = { "Réglages", "Radio", "Carte", "Téléphone", "Quitter" };
    int tabCount = scr[kPhTabCount];
    if (tabCount > 0)
    {
        qreal tw = std::min<qreal>(260 * u, (contentW - 20 * u * (tabCount - 1)) / tabCount);
        qreal tx = left + (contentW - (tw * tabCount + 20 * u * (tabCount - 1))) / 2;
        for (int i = 0; i < tabCount; i++)
        {
            int app = scr[kPhTabs + i];
            QRectF tab(tx + i * (tw + 20 * u), height - tabsH - 30 * u, tw, tabsH * 0.8);
            bool current = app == 3;
            bool sel = inTabs && i == scr[kPhTabCursor];
            QRectF box = sel ? tab.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : tab;
            p.setPen(sel ? QPen(QColor(255, 196, 60), 5 * u) : QPen(Qt::NoPen));
            p.setBrush(current ? accent : app == 4 ? QColor(200, 70, 80) : QColor(255, 255, 255, 40));
            p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
            font.setPixelSize((int)(30 * u));
            font.setBold(current || sel);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(box, Qt::AlignCenter, QString::fromUtf8(app < 5 ? kAppNames[app] : ""));
        }
    }

    // Call in progress: the conversation over everything
    if (scr[kPhInCall])
    {
        p.fillRect(QRectF(0, 0, width, height), QColor(5, 10, 25, 170));
        QRectF card(width / 2.0 - 700 * u, height / 2.0 - 280 * u, 1400 * u, 560 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(250, 252, 255));
        p.drawRoundedRect(card, 44 * u, 44 * u);
        QRectF avatar(card.left() + 60 * u, card.top() + 60 * u, 150 * u, 150 * u);
        p.setBrush(accent);
        p.drawEllipse(avatar);
        QString caller = text(kPhNames + (kPhRows / 2) * kPhNameLen * 2, kPhNameLen);
        font.setPixelSize((int)(70 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(avatar, Qt::AlignCenter, caller.left(1));
        font.setPixelSize((int)(52 * u));
        p.setFont(font);
        p.setPen(QColor(32, 40, 56));
        p.drawText(QRectF(avatar.right() + 40 * u, avatar.top(), card.width() * 0.6, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, caller);
        font.setPixelSize((int)(32 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(60, 160, 90));
        p.drawText(QRectF(avatar.right() + 40 * u, avatar.top() + 80 * u, card.width() * 0.6, 50 * u), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromUtf8("●  En ligne"));
        font.setPixelSize((int)(40 * u));
        p.setFont(font);
        p.setPen(QColor(40, 48, 64));
        p.drawText(QRectF(card.left() + 70 * u, avatar.bottom() + 40 * u, card.width() - 140 * u, card.bottom() - avatar.bottom() - 80 * u),
                   Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, text(kPhMessage, 200));
    }
    else
    {
        qreal hy = height - tabsH - 30 * u - 70 * u;
        Q_UNUSED(hy);
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Pokégear map and fly map (CoopUiGearMapScreen)
// ---------------------------------------------------------------------------

enum
{
    kGmMapType = 1,
    kGmMenuCount = 4,
    kGmMenuCursor = 5,
    kGmInTabs = 6,
    kGmTabCursor = 7,
    kGmCursorX = 12,
    kGmCursorY = 14,
    kGmPlayerX = 16,
    kGmPlayerY = 18,
    kGmHour = 20,
    kGmMinute = 21,
    kGmTabCount = 22,
    kGmTabs = 24,
    kGmRegion = 32,      // u16[16]
    kGmName = 64,        // u16[24]
    kGmFlavor = 112,     // u16[100]
    kGmMenuItems = 312,  // u16[4][24]
    kGmUsedCount = 504,  // Nuzlocke: places whose encounter was used
    kGmUsed = 508,       // u8[64][4]: x, y, width, height in cells
    kGmNameLen = 24,
};

// The Pokégear apps bar (phone, map...), shared by the Pokégear screens
static void DrawGearTabs(QPainter& p, const QRectF& area, const u8* tabs, int count, int cursor, bool inTabs, int currentApp, qreal u)
{
    static const char* kAppNames[] = { "Réglages", "Radio", "Carte", "Téléphone", "Quitter" };
    if (count <= 0)
        return;
    QFont font("Segoe UI");
    qreal tw = std::min<qreal>(260 * u, (area.width() - 20 * u * (count - 1)) / count);
    qreal tx = area.left() + (area.width() - (tw * count + 20 * u * (count - 1))) / 2;
    for (int i = 0; i < count; i++)
    {
        int app = tabs[i];
        QRectF tab(tx + i * (tw + 20 * u), area.top(), tw, area.height());
        bool current = app == currentApp;
        bool sel = inTabs && i == cursor;
        QRectF box = sel ? tab.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : tab;
        p.setPen(sel ? QPen(QColor(255, 196, 60), 5 * u) : QPen(Qt::NoPen));
        p.setBrush(current ? QColor(40, 150, 220) : app == 4 ? QColor(200, 70, 80) : QColor(255, 255, 255, 40));
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        font.setPixelSize((int)(30 * u));
        font.setBold(current || sel);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, QString::fromUtf8(app < 5 ? kAppNames[app] : ""));
    }
}

void GameUi::drawGearMap(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    const int mapType = scr[kGmMapType];
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) { return DecodeGameText((const u16*)(scr + offset), len).simplified(); };
    auto readS16 = [&](int o) { return (int)(s16)ReadU16(scr + o); };

    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(28, 60, 120));
    bg.setColorAt(1, QColor(10, 20, 48));
    p.fillRect(QRectF(0, 0, width, height), bg);

    const qreal contentW = std::min<qreal>(width - 120 * u, 1680 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 40 * u;

    // Header
    font.setPixelSize((int)(48 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, top, contentW, 70 * u), Qt::AlignVCenter | Qt::AlignLeft,
               mapType == 1 ? QString::fromUtf8("Où voler ?") : mapType == 2 ? QString::fromUtf8("Carte de la région")
                            : QString::fromUtf8("Pokégear  ·  Carte"));
    if (mapType == 0)
    {
        font.setPixelSize((int)(60 * u));
        p.setFont(font);
        p.setPen(QColor(255, 210, 90));
        p.drawText(QRectF(left, top, contentW, 70 * u), Qt::AlignVCenter | Qt::AlignRight,
                   QString("%1:%2").arg(scr[kGmHour], 2, 10, QChar('0')).arg(scr[kGmMinute], 2, 10, QChar('0')));
    }

    // The map, as large as possible (47 x 20 cells of 8 pixels)
    if (gearMap.isNull())
        gearMap = QImage(HoLympeAssetsDir() + "/gear_map.png");
    const bool hasTabs = scr[kGmTabCount] > 0;
    const qreal infoH = 200 * u;
    const qreal tabsH = hasTabs ? 100 * u : 0;
    const qreal availH = height - (top + 90 * u) - infoH - tabsH - 60 * u;
    qreal scale = std::min(contentW / 376.0, availH / 160.0);
    QRectF mapRect(width / 2.0 - 188 * scale, top + 90 * u, 376 * scale, 160 * scale);
    QPainterPath frame;
    frame.addRoundedRect(mapRect.adjusted(-10 * u, -10 * u, 10 * u, 10 * u), 30 * u, 30 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 235));
    p.drawPath(frame);
    if (!gearMap.isNull())
    {
        QPainterPath clip;
        clip.addRoundedRect(mapRect, 22 * u, 22 * u);
        p.save();
        p.setClipPath(clip);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(mapRect, gearMap);
        p.restore();
    }
    const qreal cell = 8 * scale;
    // Nuzlocke: the places whose encounter was used, tinted in red (their badge comes later, on top)
    QList<QRectF> usedAreas;
    bool cursorOnUsed = false;
    const int curX = readS16(kGmCursorX), curY = readS16(kGmCursorY);
    for (int i = 0; i < std::min<int>(scr[kGmUsedCount], 64); i++)
    {
        const u8* r = scr + kGmUsed + i * 4;
        QRectF area(mapRect.left() + r[0] * cell, mapRect.top() + r[1] * cell, r[2] * cell, r[3] * cell);
        usedAreas.append(area);
        if (curX >= r[0] && curY >= r[1] && curX < r[0] + r[2] && curY < r[1] + r[3])
            cursorOnUsed = true;
        p.setPen(QPen(QColor(200, 40, 55, 230), 2.5 * u));
        p.setBrush(QColor(220, 50, 60, 110));
        p.drawRoundedRect(area.adjusted(1.5 * u, 1.5 * u, -1.5 * u, -1.5 * u), 5 * u, 5 * u);
    }
    // Player
    int px = readS16(kGmPlayerX), py = readS16(kGmPlayerY);
    if (px >= 0 && py >= 0 && px < 47 && py < 20)
    {
        QPointF c(mapRect.left() + (px + 0.5) * cell, mapRect.top() + (py + 0.5) * cell);
        qreal r = cell * 0.55;
        p.setPen(QPen(Qt::white, 4 * u));
        p.setBrush(QColor(230, 60, 60));
        p.drawEllipse(c, r, r);
        font.setPixelSize((int)(r * 0.9));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::fromUtf8("●"));
    }
    // Friends online: a dot of their color with their initial (side by side when they share a cell)
    QStringList friendsHere;
    int cx = readS16(kGmCursorX), cy = readS16(kGmCursorY);
    {
        QMap<int, int> perCell;
        for (const CoopPlayer& f : coopPlayers)
        {
            if (f.gearX < 0 || f.gearY < 0 || f.gearX >= 47 || f.gearY >= 20)
                continue;
            int key = f.gearY * 47 + f.gearX;
            int n = perCell.value(key, 0);
            perCell[key] = n + 1;
            if (f.gearX == cx && f.gearY == cy)
                friendsHere << f.name;
            qreal r = cell * 0.66;
            QPointF c(mapRect.left() + (f.gearX + 0.5) * cell + (n - 0.5) * r * 1.5, mapRect.top() + (f.gearY + 0.5) * cell - r * 1.6);
            p.setPen(QPen(Qt::white, 3 * u));
            p.setBrush(PlayerColor(f.slot));
            p.drawEllipse(c, r, r);
            font.setPixelSize((int)(r * 1.1));
            font.setBold(true);
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, f.name.left(1).toUpper());
        }
    }

    // Cursor: pulsing square on its cell
    if (cx >= 0 && cy >= 0 && cx < 47 && cy < 20)
    {
        qreal pulse = 0.5 + 0.5 * std::sin(screen.animFrame * 0.25);
        QRectF c(mapRect.left() + cx * cell, mapRect.top() + cy * cell, cell, cell);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 196, 60), (4 + 2 * pulse) * u));
        p.drawRoundedRect(c.adjusted(-6 * u, -6 * u, 6 * u, 6 * u), 10 * u, 10 * u);
    }

    // Used places: a small ✗ badge on their top right corner
    for (const QRectF& area : usedAreas)
    {
        qreal r = cell * 0.42;
        QPointF c(area.right() - r * 0.3, area.top() + r * 0.3);
        p.setPen(QPen(Qt::white, 2 * u));
        p.setBrush(QColor(190, 35, 50));
        p.drawEllipse(c, r, r);
        font.setPixelSize((int)(r * 1.3));
        font.setBold(true);
        p.setFont(font);
        p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::fromUtf8("✗"));
    }

    // The place under the cursor
    QRectF info(left, mapRect.bottom() + 40 * u, contentW, infoH);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255));
    p.drawRoundedRect(info, 30 * u, 30 * u);
    font.setPixelSize((int)(28 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(QColor(40, 150, 220));
    p.drawText(QRectF(info.left() + 50 * u, info.top() + 20 * u, 400 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft, text(kGmRegion, 16));
    if (cursorOnUsed && friendsHere.isEmpty())
    {
        p.setPen(QColor(200, 50, 60));
        p.drawText(QRectF(info.left() + 450 * u, info.top() + 20 * u, info.width() - 500 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight,
                   QString::fromUtf8("✗  Rencontre déjà faite ici"));
    }
    if (!friendsHere.isEmpty())
    {
        // Who is at the place under the cursor
        p.setPen(QColor(150, 110, 220));
        p.drawText(QRectF(info.left() + 450 * u, info.top() + 20 * u, info.width() - 500 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight,
                   (friendsHere.size() == 1 ? QString::fromUtf8("%1 est ici") : QString::fromUtf8("%1 sont ici")).arg(friendsHere.join(", ")));
    }
    font.setPixelSize((int)(46 * u));
    p.setFont(font);
    p.setPen(QColor(32, 40, 56));
    p.drawText(QRectF(info.left() + 50 * u, info.top() + 60 * u, info.width() - 100 * u, 60 * u), Qt::AlignVCenter | Qt::AlignLeft, text(kGmName, kGmNameLen));
    font.setPixelSize((int)(30 * u));
    font.setBold(false);
    p.setFont(font);
    p.setPen(QColor(80, 90, 108));
    p.drawText(QRectF(info.left() + 50 * u, info.top() + 125 * u, info.width() - 100 * u, info.height() - 135 * u),
               Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, text(kGmFlavor, 100));

    // Fly menu
    int menuCount = scr[kGmMenuCount];
    if (menuCount > 0)
    {
        qreal itemH = 80 * u;
        QRectF menu(info.right() - 420 * u, info.top() - menuCount * itemH - 50 * u, 400 * u, menuCount * itemH + 30 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 80));
        p.drawRoundedRect(menu.translated(0, 10 * u), 30 * u, 30 * u);
        p.setBrush(QColor(250, 252, 255));
        p.drawRoundedRect(menu, 30 * u, 30 * u);
        for (int i = 0; i < menuCount; i++)
        {
            QRectF item(menu.left() + 15 * u, menu.top() + 15 * u + i * itemH, menu.width() - 30 * u, itemH);
            bool sel = i == scr[kGmMenuCursor];
            if (sel)
            {
                p.setBrush(QColor(40, 150, 220));
                p.drawRoundedRect(item.adjusted(0, 4 * u, 0, -4 * u), 24 * u, 24 * u);
            }
            font.setPixelSize((int)(32 * u));
            font.setBold(sel);
            p.setFont(font);
            p.setPen(sel ? Qt::white : QColor(50, 58, 74));
            p.drawText(item, Qt::AlignCenter, text(kGmMenuItems + i * kGmNameLen * 2, kGmNameLen));
        }
    }

    if (hasTabs)
        DrawGearTabs(p, QRectF(left, height - tabsH - 20 * u, contentW, tabsH * 0.8), scr + kGmTabs, scr[kGmTabCount], scr[kGmTabCursor],
                     scr[kGmInTabs] != 0, 2, u);
    else
    {
        DrawButtonHint(p, QPointF(left, height - 80 * u), 46 * u, "A", "Voler ici", QColor(80, 170, 90), Qt::white);
        DrawButtonHint(p, QPointF(left + 320 * u, height - 80 * u), 46 * u, "B", "Retour", QColor(220, 80, 80), Qt::white);
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Pokégear radio (CoopUiRadioScreen)
// ---------------------------------------------------------------------------

enum
{
    kRdStation = 1,
    kRdButton = 2,
    kRdSignal = 3,
    kRdInTabs = 4,
    kRdTabCursor = 5,
    kRdTabCount = 6,
    kRdCursorX = 12,
    kRdCursorY = 14,
    kRdHour = 16,
    kRdMinute = 17,
    kRdTabs = 18,
    kRdTitle = 24,  // u16[24]
    kRdHost = 72,   // u16[16]
    kRdLines = 104, // u16[2][64]
    kRdLineLen = 64,
};

void GameUi::drawRadio(QPainter& p, int width, int height)
{
    p.save();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) { return DecodeGameText((const u16*)(scr + offset), len).simplified(); };
    const QColor accent(40, 150, 220);
    const bool tuned = scr[kRdStation] != 0xFF;

    QLinearGradient bg(0, 0, width, height);
    bg.setColorAt(0, QColor(28, 60, 120));
    bg.setColorAt(1, QColor(10, 20, 48));
    p.fillRect(QRectF(0, 0, width, height), bg);

    const qreal contentW = std::min<qreal>(width - 160 * u, 1600 * u);
    const qreal left = (width - contentW) / 2;
    const qreal top = 50 * u;
    font.setPixelSize((int)(50 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("Pokégear  ·  Radio"));
    font.setPixelSize((int)(64 * u));
    p.setFont(font);
    p.setPen(QColor(255, 210, 90));
    p.drawText(QRectF(left, top, contentW, 80 * u), Qt::AlignVCenter | Qt::AlignRight,
               QString("%1:%2").arg(scr[kRdHour], 2, 10, QChar('0')).arg(scr[kRdMinute], 2, 10, QChar('0')));

    // Tuner: the game's globe (circle of radius 52 around 128, 92 on the DS screen)
    const qreal bodyTop = top + 130 * u;
    const qreal bodyH = height - bodyTop - 200 * u;
    const qreal globeR = std::min(bodyH * 0.40, contentW * 0.14);
    const QPointF center(left + contentW * 0.26, bodyTop + bodyH / 2);
    QRadialGradient globe(center, globeR);
    globe.setColorAt(0, QColor(20, 40, 80));
    globe.setColorAt(1, QColor(6, 14, 32));
    p.setPen(QPen(Qt::white, 6 * u));
    p.setBrush(globe);
    p.drawEllipse(center, globeR, globeR);
    p.setPen(QPen(QColor(80, 150, 230, 160), 2 * u));
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 3; i++)
    {
        qreal k = i / 4.0;
        p.drawEllipse(center, globeR * k, globeR);
        p.drawEllipse(center, globeR, globeR * k);
    }
    p.drawLine(QPointF(center.x() - globeR, center.y()), QPointF(center.x() + globeR, center.y()));
    p.drawLine(QPointF(center.x(), center.y() - globeR), QPointF(center.x(), center.y() + globeR));
    qreal cx = center.x() + ((s16)ReadU16(scr + kRdCursorX) - 128) / 52.0 * globeR;
    qreal cy = center.y() + ((s16)ReadU16(scr + kRdCursorY) - 92) / 52.0 * globeR;
    if (tuned)
    {
        qreal pulse = 0.5 + 0.5 * std::sin(screen.animFrame * 0.3);
        p.setPen(QPen(QColor(255, 255, 255, (int)(160 * (1 - pulse))), 4 * u));
        p.drawEllipse(QPointF(cx, cy), 30 * u + 30 * u * pulse, 30 * u + 30 * u * pulse);
    }
    p.setPen(QPen(Qt::white, 5 * u));
    p.setBrush(tuned ? QColor(255, 196, 60) : QColor(150, 160, 180));
    p.drawEllipse(QPointF(cx, cy), 18 * u, 18 * u);

    // Preset buttons: two on each side of the globe
    for (int i = 0; i < 4; i++)
    {
        qreal bx = i < 2 ? center.x() - globeR - 140 * u : center.x() + globeR + 40 * u;
        qreal by = center.y() + (i % 2 == 0 ? -globeR * 0.55 : globeR * 0.35);
        QRectF b(bx, by, 100 * u, 70 * u);
        bool sel = scr[kRdButton] == i;
        p.setPen(sel ? QPen(QColor(255, 196, 60), 5 * u) : QPen(Qt::NoPen));
        p.setBrush(sel ? accent : QColor(255, 255, 255, 40));
        p.drawRoundedRect(b, 35 * u, 35 * u);
        font.setPixelSize((int)(30 * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(b, Qt::AlignCenter, QString::number(i + 1));
    }

    // The show
    QRectF panel(left + contentW * 0.56, bodyTop, contentW * 0.44, bodyH);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(250, 252, 255));
    p.drawRoundedRect(panel, 36 * u, 36 * u);
    font.setPixelSize((int)(30 * u));
    font.setBold(true);
    p.setFont(font);
    p.setPen(accent);
    p.drawText(QRectF(panel.left() + 50 * u, panel.top() + 40 * u, panel.width() - 100 * u, 44 * u), Qt::AlignVCenter | Qt::AlignLeft,
               tuned ? text(kRdHost, 16) : QString());
    // Signal bars
    int signal = tuned ? scr[kRdSignal] + 1 : 0;
    for (int i = 0; i < 4; i++)
    {
        QRectF bar(panel.right() - 60 * u - (3 - i) * 26 * u, panel.top() + 76 * u - (i + 1) * 9 * u, 18 * u, (i + 1) * 9 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(i < signal ? QColor(60, 170, 90) : QColor(210, 214, 224));
        p.drawRoundedRect(bar, 4 * u, 4 * u);
    }
    font.setPixelSize((int)(52 * u));
    p.setFont(font);
    p.setPen(QColor(32, 40, 56));
    p.drawText(QRectF(panel.left() + 50 * u, panel.top() + 90 * u, panel.width() - 100 * u, 80 * u), Qt::AlignVCenter | Qt::AlignLeft,
               tuned ? text(kRdTitle, 24) : QString::fromUtf8("Aucune station"));
    font.setPixelSize((int)(36 * u));
    font.setBold(false);
    p.setFont(font);
    if (tuned)
    {
        // The two last lines of the show, the newest one darker
        for (int i = 0; i < 2; i++)
        {
            p.setPen(i == 1 ? QColor(40, 48, 64) : QColor(130, 138, 154));
            p.drawText(QRectF(panel.left() + 50 * u, panel.top() + 210 * u + i * 60 * u, panel.width() - 100 * u, 56 * u),
                       Qt::AlignVCenter | Qt::AlignLeft, text(kRdLines + i * kRdLineLen * 2, kRdLineLen));
        }
    }
    else
    {
        p.setPen(QColor(120, 128, 144));
        p.drawText(QRectF(panel.left() + 50 * u, panel.top() + 210 * u, panel.width() - 100 * u, 200 * u), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                   QString::fromUtf8("Déplace le curseur sur le globe pour capter une station."));
    }

    DrawGearTabs(p, QRectF(left, height - 110 * u, contentW, 80 * u), scr + kRdTabs, scr[kRdTabCount], scr[kRdTabCursor], scr[kRdInTabs] != 0, 1, u);
    p.restore();
}

// ---------------------------------------------------------------------------
// Main menu (CoopUiMainMenuScreen): custom HeartGold Coop look
// ---------------------------------------------------------------------------

enum
{
    kMmCursor = 1,
    kMmMode = 2,
    kMmNewGameCursor = 3,
    kMmGender = 4,
    kMmHasPokedex = 5,
    kMmBadges = 6,
    kMmOptionCount = 7,
    kMmHours = 12,
    kMmMinutes = 14,
    kMmDexCount = 16,
    kMmOptions = 18,
    kMmName = 28,
    kMmLabels = 44,      // u16[9][20]
    kMmLabelLen = 20,
    kMmMessage = 404,    // u16[200]
    kMmChoices = 804,    // u16[2][24]
};

// ---------------------------------------------------------------------------
// Starter choice (Professor Elm's lab). Nuzlocke: three closed Poke Balls, a first press
// opens one (like a card pack), a second takes it. Classic: the three are shown right away.
// ---------------------------------------------------------------------------

enum
{
    kStHidden = 1,
    kStChosen = 2,
    kStSpecies = 12,
    kStTypes = 18,
    kStNames = 24,
    kStStats = 96,
};

void GameUi::updateStarter(NDS* nds, u8* mailbox, u32 newPressed)
{
    if (screen.type != kScreenStarter)
    {
        starter.active = false;
        return;
    }
    u8* scr = mailbox + kScrOffset;
    if (!starter.active)
    {
        starter = StarterState();
        starter.active = true;
        starter.hidden = scr[kStHidden] != 0;
        for (int i = 0; i < 3; i++)
            starter.reveal[i] = starter.hidden ? -1.0f : 1.0f;
    }
    nds->SetKeyMask(0xFFF); // the choice has the controls
    starter.frame++;
    dirty = true; // the Poke Balls breathe, the cards animate

    for (int i = 0; i < 3; i++)
        if (starter.reveal[i] >= 0.0f && starter.reveal[i] < 1.0f)
            starter.reveal[i] = std::min(1.0f, starter.reveal[i] + 1.0f / 50.0f);
    if (starter.chosen >= 0)
    {
        starter.chosenAnim = std::min(3.0f, starter.chosenAnim + 1.0f / 30.0f);
        if (starter.chosenAnim >= 3.0f && (s8)scr[kStChosen] < 0)
            scr[kStChosen] = (u8)starter.chosen; // the game gives the Pokemon
        return;
    }

    auto act = [&](int i) {
        starter.cursor = i;
        if (starter.reveal[i] < 0.0f)
            starter.reveal[i] = 0.0f;            // open the Poke Ball
        else if (starter.reveal[i] >= 1.0f)
            starter.chosen = i;                  // take this one
    };
    if (pendingClick >= 100 && pendingClick < 103)
    {
        act(pendingClick - 100);
        pendingClick = 0;
    }
    if (newPressed & kKeyLeft)
        starter.cursor = (starter.cursor + 2) % 3;
    else if (newPressed & kKeyRight)
        starter.cursor = (starter.cursor + 1) % 3;
    else if (newPressed & kKeyA)
        act(starter.cursor);
}

void GameUi::drawStarter(QPainter& p, int width, int height)
{
    p.save();
    clickTargets.clear();
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    float t = starter.frame / 60.0f;

    // Background: deep blue with a warm light from above
    QLinearGradient bg(0, 0, 0, height);
    bg.setColorAt(0, QColor(28, 42, 86));
    bg.setColorAt(1, QColor(10, 14, 34));
    p.fillRect(QRectF(0, 0, width, height), bg);
    QRadialGradient light(QPointF(width / 2.0, -height * 0.1), height * 0.9);
    light.setColorAt(0, QColor(255, 214, 120, 70));
    light.setColorAt(1, QColor(255, 214, 120, 0));
    p.fillRect(QRectF(0, 0, width, height), light);

    QFont font("Segoe UI");
    font.setBold(true);
    font.setPixelSize((int)(58 * u));
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 50 * u, width, 80 * u), Qt::AlignCenter, QString::fromUtf8("Choisis ton partenaire"));
    font.setBold(false);
    font.setPixelSize((int)(28 * u));
    p.setFont(font);
    p.setPen(QColor(200, 210, 235));
    bool allOpen = starter.reveal[0] >= 1.0f && starter.reveal[1] >= 1.0f && starter.reveal[2] >= 1.0f;
    QString sub = starter.chosen >= 0 ? QString()
        : starter.hidden && !allOpen ? QString::fromUtf8("Ouvre les Poké Balls pour découvrir les Pokémon de ta tentative")
        : QString::fromUtf8("Le Professeur Orme te confie l'un de ces Pokémon");
    p.drawText(QRectF(0, 128 * u, width, 44 * u), Qt::AlignCenter, sub);

    const qreal cardW = 480 * u, cardH = 740 * u, gap = 60 * u;
    const qreal left = (width - 3 * cardW - 2 * gap) / 2, top = 200 * u;
    for (int i = 0; i < 3; i++)
    {
        bool selected = i == starter.cursor && starter.chosen < 0;
        float reveal = starter.reveal[i];
        float chosenT = std::min(1.0f, starter.chosenAnim);
        float fade = starter.chosen >= 0 && starter.chosen != i ? 1.0f - chosenT : 1.0f;
        qreal lift = selected ? (14 + 6 * std::sin(t * 4)) * u : 0;
        QRectF card(left + i * (cardW + gap), top - lift, cardW, cardH);
        if (starter.chosen == i)
            card = card.adjusted(-20 * u * chosenT, -20 * u * chosenT, 20 * u * chosenT, 20 * u * chosenT);
        clickTargets.push_back({ (float)(card.left() / width), (float)(card.top() / height), (float)(card.right() / width), (float)(card.bottom() / height), 100 + i });
        p.setOpacity(fade);

        // Card: glass, gold border when selected
        if (selected || starter.chosen == i)
        {
            QRadialGradient glow(card.center(), cardW * 0.9);
            glow.setColorAt(0.5, QColor(255, 200, 80, 90));
            glow.setColorAt(1.0, QColor(255, 200, 80, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(card.center(), cardW * 0.9, cardH * 0.7);
        }
        QLinearGradient cg(card.topLeft(), card.bottomLeft());
        cg.setColorAt(0, QColor(250, 252, 255, 245));
        cg.setColorAt(1, QColor(226, 234, 246, 245));
        p.setPen(selected || starter.chosen == i ? QPen(QColor(255, 196, 60), 6 * u) : QPen(QColor(255, 255, 255, 120), 2 * u));
        p.setBrush(cg);
        p.drawRoundedRect(card, 40 * u, 40 * u);

        QPointF ballCenter(card.center().x(), card.top() + 220 * u);
        if (reveal < 1.0f)
        {
            // A closed Poke Ball, breathing; it shakes, then bursts open
            float open = reveal < 0.0f ? 0.0f : reveal;
            float shake = open < 0.5f ? std::sin(open * 60.0f) * 12.0f * (open / 0.5f) : 0.0f;
            qreal r = (110 + (reveal < 0.0f ? 4 * std::sin(t * 3 + i) : 0)) * u;
            p.save();
            p.translate(ballCenter);
            p.rotate(shake);
            if (open < 0.6f)
            {
                QRectF ball(-r, -r, 2 * r, 2 * r);
                QRadialGradient red(QPointF(-r * 0.3, -r * 0.4), r * 1.6);
                red.setColorAt(0, QColor(255, 110, 100));
                red.setColorAt(1, QColor(200, 30, 40));
                p.setPen(QPen(QColor(40, 40, 50), 8 * u));
                p.setBrush(Qt::white);
                p.drawEllipse(ball);
                p.setBrush(red);
                p.drawChord(ball, 0, 180 * 16);
                p.drawLine(QPointF(-r, 0), QPointF(r, 0));
                p.setBrush(Qt::white);
                p.drawEllipse(QPointF(0, 0), r * 0.28, r * 0.28);
                p.setBrush(QColor(230, 230, 236));
                p.drawEllipse(QPointF(0, 0), r * 0.14, r * 0.14);
            }
            p.restore();
            if (open > 0.45f)
            {
                // White burst
                float b = (open - 0.45f) / 0.55f;
                QRadialGradient burst(ballCenter, r * (1 + 3 * b));
                burst.setColorAt(0, QColor(255, 255, 255, (int)(255 * (1 - b))));
                burst.setColorAt(1, QColor(255, 240, 200, 0));
                p.setPen(Qt::NoPen);
                p.setBrush(burst);
                p.drawEllipse(ballCenter, r * (1 + 3 * b), r * (1 + 3 * b));
            }
            if (reveal < 0.0f)
            {
                font.setBold(true);
                font.setPixelSize((int)(34 * u));
                p.setFont(font);
                p.setPen(QColor(90, 100, 124));
                p.drawText(QRectF(card.left(), card.top() + 420 * u, cardW, 60 * u), Qt::AlignCenter, "?");
                font.setBold(false);
                font.setPixelSize((int)(26 * u));
                p.setFont(font);
                p.drawText(QRectF(card.left(), card.top() + 480 * u, cardW, 50 * u), Qt::AlignCenter, QString::fromUtf8("Appuie pour ouvrir"));
            }
        }
        float show = reveal >= 1.0f ? 1.0f : std::max(0.0f, (reveal - 0.6f) / 0.4f);
        if (show > 0.0f)
        {
            p.setOpacity(fade * show);
            int species = scr[kStSpecies + i * 2] | (scr[kStSpecies + i * 2 + 1] << 8);
            const QImage& sheet = pokemonSprite(species);
            if (!sheet.isNull())
            {
                int frame = ((starter.frame / 20) % 2) && selected ? 1 : 0;
                QImage sprite = sheet.copy(frame * sheet.height(), 0, sheet.height(), sheet.height());
                qreal s = (280 + 40 * (1 - show)) * u;
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(QRectF(ballCenter.x() - s / 2, ballCenter.y() - s / 2 - 10 * u, s, s), sprite);
                p.restore();
            }
            QString name = DecodeGameText((const u16*)(scr + kStNames + i * 24), 12);
            font.setBold(true);
            font.setPixelSize((int)(44 * u));
            p.setFont(font);
            p.setPen(QColor(30, 36, 52));
            p.drawText(QRectF(card.left(), card.top() + 380 * u, cardW, 56 * u), Qt::AlignCenter, name);
            int type1 = scr[kStTypes + i * 2], type2 = scr[kStTypes + i * 2 + 1];
            qreal bw = 150 * u, bh = 40 * u;
            if (type2 == type1)
                DrawTypeBadge(p, QRectF(card.center().x() - bw / 2, card.top() + 444 * u, bw, bh), type1, u);
            else
            {
                DrawTypeBadge(p, QRectF(card.center().x() - bw - 6 * u, card.top() + 444 * u, bw, bh), type1, u);
                DrawTypeBadge(p, QRectF(card.center().x() + 6 * u, card.top() + 444 * u, bw, bh), type2, u);
            }
            // Base stats
            static const char* kLabels[6] = { "PV", "Attaque", "Défense", "Vitesse", "Atq. Spé", "Déf. Spé" };
            font.setPixelSize((int)(22 * u));
            int total = 0;
            for (int s = 0; s < 6; s++)
            {
                int v = scr[kStStats + i * 6 + s];
                total += v;
                qreal y = card.top() + 512 * u + s * 34 * u;
                font.setBold(false);
                p.setFont(font);
                p.setPen(QColor(90, 100, 124));
                p.drawText(QRectF(card.left() + 40 * u, y, 120 * u, 26 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(kLabels[s]));
                QRectF track(card.left() + 170 * u, y + 8 * u, cardW - 260 * u, 10 * u);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(214, 222, 236));
                p.drawRoundedRect(track, 5 * u, 5 * u);
                p.setBrush(v >= 90 ? QColor(80, 190, 110) : v >= 60 ? QColor(240, 180, 60) : QColor(230, 110, 80));
                p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * std::min(1.0, v / 150.0), track.height()), 5 * u, 5 * u);
                font.setBold(true);
                p.setFont(font);
                p.setPen(QColor(40, 48, 66));
                p.drawText(QRectF(track.right() + 10 * u, y, 50 * u, 26 * u), Qt::AlignVCenter | Qt::AlignRight, QString::number(v));
            }
            Q_UNUSED(total);
        }
        p.setOpacity(1);
    }

    // After the choice: who joins the team
    if (starter.chosen >= 0)
    {
        QString name = DecodeGameText((const u16*)(scr + kStNames + starter.chosen * 24), 12);
        font.setBold(true);
        font.setPixelSize((int)(40 * u));
        p.setFont(font);
        p.setOpacity(std::min(1.0f, starter.chosenAnim));
        p.setPen(QColor(255, 214, 90));
        p.drawText(QRectF(0, top + cardH + 40 * u, width, 60 * u), Qt::AlignCenter, QString::fromUtf8("%1 rejoint ton équipe !").arg(name));
        p.setOpacity(1);
    }
    else
    {
        bool opened = starter.reveal[starter.cursor] >= 1.0f;
        DrawButtonHint(p, QPointF(width / 2.0 - 260 * u, top + cardH + 40 * u), 46 * u, "A",
                       opened ? QString::fromUtf8("Prendre ce Pokémon") : QString::fromUtf8("Ouvrir la Poké Ball"), QColor(80, 170, 90), QColor(220, 228, 245));
        DrawButtonHint(p, QPointF(width / 2.0 + 120 * u, top + cardH + 40 * u), 46 * u, QString::fromUtf8("◀"), QString::fromUtf8("▶  Changer"), QColor(90, 110, 160), QColor(220, 228, 245));
    }
    p.restore();
}

bool GameUi::HandleClick(float x, float y)
{
    QMutexLocker locker(&mutex);
    if (screen.type == 0)
        return false;
    for (const ClickTarget& t : clickTargets)
    {
        if (x >= t.x0 && x <= t.x1 && y >= t.y0 && y <= t.y1)
        {
            pendingClick = t.code;
            return true;
        }
    }
    return false;
}

void GameUi::drawMainMenu(QPainter& p, int width, int height)
{
    p.save();
    clickTargets.clear();
    auto clickable = [&](const QRectF& r, int code) {
        clickTargets.push_back({ (float)(r.left() / width), (float)(r.top() / height), (float)(r.right() / width), (float)(r.bottom() / height), code });
    };
    const qreal u = std::min(height / 1080.0, width / 1920.0);
    const u8* scr = screen.raw;
    QFont font("Segoe UI");
    auto text = [&](int offset, int len) {
        QString t = DecodeGameText((const u16*)(scr + offset), len).simplified();
        return t.isEmpty() ? t : t.left(1).toUpper() + t.mid(1).toLower();
    };
    const QColor gold(246, 196, 70), goldDark(176, 110, 20);

    // Background: HeartGold sunset with light rays
    QRadialGradient bg(QPointF(width * 0.72, height * 0.38), width * 0.8);
    bg.setColorAt(0, QColor(255, 214, 120));
    bg.setColorAt(0.35, QColor(214, 120, 40));
    bg.setColorAt(1, QColor(70, 22, 18));
    p.fillRect(QRectF(0, 0, width, height), bg);
    p.save();
    p.translate(width * 0.72, height * 0.38);
    p.rotate(screen.animFrame * 0.05);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 240, 200, 26));
    for (int i = 0; i < 12; i++)
    {
        QPolygonF ray;
        ray << QPointF(0, 0) << QPointF(width * 1.2, -60 * u) << QPointF(width * 1.2, 60 * u);
        p.drawPolygon(ray);
        p.rotate(30);
    }
    p.restore();

    // Ho-Oh, the legendary of HeartGold
    const QImage& hooh = pokemonSprite(250);
    if (!hooh.isNull())
    {
        int frames = hooh.width() >= hooh.height() * 2 ? 2 : 1;
        QImage img = hooh.copy(((screen.animFrame / 30) % frames) * (hooh.width() / frames), 0, hooh.width() / frames, hooh.height());
        qreal size = std::min(height * 0.78, width * 0.42);
        qreal bob = std::sin(screen.animFrame * 0.04) * 12 * u;
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(QRectF(width * 0.72 - size / 2, height * 0.42 - size / 2 + bob, size, size), img);
        p.restore();
    }

    // Title
    const qreal left = std::max<qreal>(80 * u, width * 0.06);
    font.setPixelSize((int)(30 * u));
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 6 * u);
    p.setFont(font);
    p.setPen(QColor(255, 236, 190));
    p.drawText(QRectF(left, 50 * u, 900 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("POKÉMON"));
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    font.setPixelSize((int)(96 * u));
    font.setItalic(true);
    p.setFont(font);
    p.setPen(QColor(90, 30, 10, 160));
    p.drawText(QRectF(left + 5 * u, 90 * u + 5 * u, 1000 * u, 120 * u), Qt::AlignVCenter | Qt::AlignLeft, "HeartGold");
    QLinearGradient titleGrad(0, 90 * u, 0, 210 * u);
    titleGrad.setColorAt(0, QColor(255, 244, 190));
    titleGrad.setColorAt(1, gold);
    p.setPen(QPen(QBrush(titleGrad), 1));
    p.drawText(QRectF(left, 90 * u, 1000 * u, 120 * u), Qt::AlignVCenter | Qt::AlignLeft, "HeartGold");
    font.setItalic(false);
    QRectF coop(left + 470 * u, 115 * u, 170 * u, 64 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(200, 40, 50));
    p.drawRoundedRect(coop, 32 * u, 32 * u);
    font.setPixelSize((int)(38 * u));
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(coop, Qt::AlignCenter, "Coop");

    // Buttons
    const int count = std::min<int>(scr[kMmOptionCount], 9);
    const int cursor = scr[kMmCursor];
    const qreal colW = std::min<qreal>(760 * u, width * 0.42);
    qreal y = 250 * u;
    for (int n = 0; n < count; n++)
    {
        int option = scr[kMmOptions + n];
        bool sel = option == cursor && scr[kMmMode] == 0;
        bool isContinue = option == 0;
        qreal h = isContinue ? 300 * u : 96 * u;
        if (y + h > height - 120 * u)
            break;
        QRectF card(left, y, colW, h);
        if (scr[kMmMode] == 0)
            clickable(card, option + 1); // click = choose this button
        QRectF box = sel ? card.adjusted(-6 * u, -6 * u, 6 * u, 6 * u) : card;
        QPainterPath shape;
        shape.addRoundedRect(box, 30 * u, 30 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawRoundedRect(box.translated(0, 8 * u), 30 * u, 30 * u);
        p.setBrush(sel ? QColor(255, 252, 244) : QColor(255, 248, 236, 200));
        p.drawPath(shape);
        if (sel)
        {
            p.setPen(QPen(gold, 6 * u));
            p.setBrush(Qt::NoBrush);
            p.drawPath(shape);
        }
        QString label = text(kMmLabels + option * kMmLabelLen * 2, kMmLabelLen);
        font.setPixelSize((int)((isContinue ? 44 : 36) * u));
        font.setBold(true);
        p.setFont(font);
        p.setPen(sel ? goldDark : QColor(90, 60, 40));
        p.drawText(QRectF(box.left() + 40 * u, box.top() + (isContinue ? 24 * u : 0), box.width() - 80 * u, isContinue ? 60 * u : box.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, label);
        if (isContinue)
        {
            // Save file: player, time, badges, Pokédex
            const bool female = scr[kMmGender] == 1;
            QRectF avatar(box.left() + 40 * u, box.top() + 110 * u, 150 * u, 150 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(female ? QColor(220, 90, 130) : QColor(60, 120, 210));
            p.drawEllipse(avatar);
            QString name = DecodeGameText((const u16*)(scr + kMmName), 8).simplified();
            font.setPixelSize((int)(70 * u));
            p.setFont(font);
            p.setPen(Qt::white);
            p.drawText(avatar, Qt::AlignCenter, name.left(1).toUpper());
            auto info = [&](qreal yy, const QString& k, const QString& v) {
                font.setPixelSize((int)(30 * u));
                font.setBold(false);
                p.setFont(font);
                p.setPen(QColor(130, 100, 80));
                p.drawText(QRectF(avatar.right() + 40 * u, yy, 260 * u, 40 * u), Qt::AlignVCenter | Qt::AlignLeft, k);
                font.setBold(true);
                p.setFont(font);
                p.setPen(QColor(60, 40, 30));
                p.drawText(QRectF(avatar.right() + 40 * u, yy, box.right() - avatar.right() - 80 * u, 40 * u), Qt::AlignVCenter | Qt::AlignRight, v);
            };
            qreal iy = avatar.top() - 6 * u;
            info(iy, "Joueur", name); iy += 42 * u;
            info(iy, "Temps de jeu", QString("%1:%2").arg(ReadU16(scr + kMmHours)).arg(scr[kMmMinutes], 2, 10, QChar('0'))); iy += 42 * u;
            info(iy, "Badges", QString::number(scr[kMmBadges])); iy += 42 * u;
            if (scr[kMmHasPokedex])
                info(iy, QString::fromUtf8("Pokédex"), QString::number(ReadU16(scr + kMmDexCount)));
        }
        y += h + 22 * u;
    }

    DrawButtonHint(p, QPointF(left, height - 90 * u), 48 * u, QString::fromUtf8("▲▼"), "Choisir", QColor(120, 60, 30), QColor(255, 240, 220));
    DrawButtonHint(p, QPointF(left + 280 * u, height - 90 * u), 48 * u, "A", "Valider", QColor(80, 170, 90), QColor(255, 240, 220));

    // Messages over the menu (new game warning, mystery gift...)
    const int mode = scr[kMmMode];
    if (mode != 0)
    {
        if (mode == 1)
            clickable(QRectF(0, 0, width, height), 0x80); // click anywhere = OK
        p.fillRect(QRectF(0, 0, width, height), QColor(20, 8, 4, 160));
        QRectF card(width / 2.0 - 720 * u, height / 2.0 - 220 * u, 1440 * u, mode == 2 ? 440 * u : 360 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 250, 240));
        p.drawRoundedRect(card, 40 * u, 40 * u);
        font.setPixelSize((int)(36 * u));
        font.setBold(false);
        p.setFont(font);
        p.setPen(QColor(60, 40, 30));
        p.drawText(card.adjusted(70 * u, 50 * u, -70 * u, mode == 2 ? -180 * u : -80 * u), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap,
                   DecodeGameText((const u16*)(scr + kMmMessage), 200).simplified());
        if (mode == 2)
        {
            for (int i = 0; i < 2; i++)
            {
                QRectF b(card.left() + 120 * u + i * (card.width() - 240 * u) / 2, card.bottom() - 150 * u, (card.width() - 240 * u) / 2 - 30 * u, 100 * u);
                bool sel = scr[kMmNewGameCursor] == i;
                clickable(b, 0x40 | i); // click = this choice
                QRectF bb = sel ? b.adjusted(-5 * u, -5 * u, 5 * u, 5 * u) : b;
                p.setPen(sel ? QPen(Qt::white, 5 * u) : QPen(Qt::NoPen));
                p.setBrush(i == 0 ? QColor(60, 160, 90) : QColor(200, 70, 70));
                if (!sel)
                    p.setBrush(i == 0 ? QColor(60, 160, 90).darker(140) : QColor(200, 70, 70).darker(140));
                p.drawRoundedRect(bb, bb.height() / 2, bb.height() / 2);
                font.setPixelSize((int)(34 * u));
                font.setBold(true);
                p.setFont(font);
                p.setPen(Qt::white);
                p.drawText(bb, Qt::AlignCenter, DecodeGameText((const u16*)(scr + kMmChoices + i * 48), 24).simplified());
            }
        }
        else
            DrawButtonHint(p, QPointF(card.center().x() - 80 * u, card.bottom() - 90 * u), 48 * u, "A", "OK", QColor(80, 170, 90));
    }
    p.restore();
}
