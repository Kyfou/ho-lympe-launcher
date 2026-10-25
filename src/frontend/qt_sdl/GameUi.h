/*
    HeartGold Coop - modern HD menus drawn by the launcher over the game.

    The game publishes a menu in its shared memory block (CoopUiMenu, see
    pokeheartgold include/coop.h). GameUi takes over the controls while the menu
    is open, writes the chosen entry back, and renders the menu in HD in the style
    of Pokémon Brilliant Diamond / Shining Pearl.
*/

#ifndef GAMEUI_H
#define GAMEUI_H

#include <map>
#include <vector>
#include <QColor>
#include <QMap>
#include <QPair>
#include <vector>
#include <QImage>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include "types.h"

namespace melonDS { class NDS; }

class GameUi
{
public:
    static GameUi& Get();

    // Emulator thread, once per frame after the game's input has been set:
    // reads the menu from the mailbox and handles the controls while it is open
    void Update(melonDS::NDS* nds, melonDS::u8* mailbox);

    // Render thread: returns false when nothing is shown. Otherwise fills `image`
    // (width x height pixels) and sets `changed` when it was redrawn.
    bool Render(QImage& image, int width, int height, bool& changed);

    // UI thread: a click at (x, y) in 0..1 window coordinates. Returns true when it
    // hit a clickable part of an HD screen (the game then gets the choice).
    bool HandleClick(float x, float y);

    // Emulator thread (CoopLink): -1 no coop, 0 server not reached yet, 1 on the server alone, 2 friend here
    void SetNetworkStatus(int status) { networkStatus = status; }

    // Emulator thread (CoopLink): the other players on the server (relay slot, name, their game's info)
    struct CoopPlayer
    {
        int slot = -1;   // relay slot: their color
        int remote = -1; // index in the mailbox's CoopRemote array
        QString name, location;
        QList<int> team;
        int badges = 0;
        int gearX = -1, gearY = -1;
    };
    void SetCoopPlayers(const std::vector<CoopPlayer>& players, int mySlot);
    void SetLocalName(const QString& name);
    static QColor PlayerColor(int slot);

    // Nuzlocke (CoopLink): messages (a Pokemon fell, a ball refused), the end of the run,
    // the number of dead (from nuzlocke.dat, for the launcher's card)
    void NuzlockeMessage(const QString& text, const QColor& color);
    void SetNuzlockeOver();
    bool IsNuzlockeOver();
    void SetNuzlockeLost(const QByteArray& dead);

    // Chat (online): UI thread - a key while playing (T opens, Enter sends, Tab picks who, Escape
    // closes); returns true when the chat used it. Emulator thread (CoopLink) - messages in and out.
    bool ChatKey(class QKeyEvent* event);
    void ChatReceived(int slot, const QString& from, const QString& text, bool isPrivate);
    bool TakeChatOut(int& target, QString& text); // target -1 = everybody
    void ChatSay(const QString& text, const QString& to = QString()); // automated tests: sends (to a name, or everybody)
    void ChatShowInput(const QString& text);      // automated tests: the open chat box
    void NamingType(const QString& text);         // automated tests: typed in the naming box

    // Gifts (online): the gift screen (from the players screen) and the offers received.
    // Emulator thread (CoopLink) for all of these.
    struct GiftItem { int id = 0, quantity = 0; QString name; };
    struct GiftMon
    {
        int species = 0, heldItem = 0, level = 0;
        bool egg = false, shiny = false;
        int gender = 0;
        QString name;
        // Details for the trade screen
        melonDS::u32 personality = 0;
        int stats[6] {};
        QStringList moves;
        QString itemName, ability;
    };
    struct GiftChoice { int target = -1, kind = 0, id = 0, quantity = 0; QString label; int species = 0, level = 0; }; // kind 1 item, 2 money, 3 Pokemon (id = party slot)
    bool GiftCatalogWanted();
    void SetGiftCatalog(melonDS::u32 money, const QList<GiftItem>& items, const QList<GiftMon>& mons);
    bool TakeGiftChoice(GiftChoice& choice);
    void GiftOffered(int key, int slot, const QString& from, const QString& label, int kind, int itemId, int species, int quantity);
    bool TakeGiftAnswer(int& key, bool& accept);

    // Trades (online), emulator thread (CoopLink)
    struct TradeAction
    {
        enum Kind { Request, Offer, Confirm, Cancel } kind = Cancel;
        int target = -1, index = -1;
        GiftMon mon;
    };
    bool TakeTradeAction(TradeAction& action);
    void TradeRequested(int slot, const QString& from, const GiftMon& theirs);
    void TradeOffered(const GiftMon& theirs);
    void TradeTheyConfirmed();
    void TradeCancelled(const QString& why);
    void TradeExecuting();

    // The player's look (Ethan / Lyra and colors), as "gender:z1.z2.z3.z4.z5" (preset index per zone).
    // UI thread at start (HGCOOP_LOOK), then the look screen changes it.
    void SetLook(const QString& look);
    QString LookString();
    std::vector<std::pair<melonDS::u16, melonDS::u16>> LookPairsFor(const QString& look); // a friend's colors
    static void WriteLookPairs(melonDS::u8* l, const std::vector<std::pair<melonDS::u16, melonDS::u16>>& pairs);

    // Ho-Lympe (any thread): where the player is and a summary of the game, sent to the server
    QString LocationName();
    QJsonObject SaveSummary();

    // UI thread: where the top screen is drawn, as the 2x3 matrix mapping its 256x192 pixels
    // to the widget (widget size in the same units). nullptr when the top screen is hidden.
    void SetTopScreenTransform(const float* mtx, int widgetWidth, int widgetHeight);

private:
    GameUi() = default;

    struct MenuState
    {
        int type = 0;
        std::vector<melonDS::u8> entries; // StartMenuAction values, in the game's order
        int cursor = 0;                   // index in entries
        melonDS::u32 money = 0;
        float open = 0.0f;                // 0 = hidden, 1 = fully shown
        bool closing = false;
    };

    // Full screens mirrored from the game (CoopUiScreen, e.g. the party)
    static constexpr int kScreenSize = 1024; // COOP_SCREEN_SIZE
    struct ScreenState
    {
        int type = 0;
        melonDS::u8 raw[kScreenSize] {}; // copy of CoopUiScreen
        melonDS::u32 lastFrame = 0;
        int staleFrames = 0;
        int animFrame = 0;
    };

    QMutex mutex;
    MenuState state;
    ScreenState screen;
    std::map<int, QImage> iconCache;
    std::map<int, QImage> itemIconCache;
    std::map<int, QImage> spriteCache;
    QImage gearMap; // Pokégear map picture (assets/gear_map.png)
    int requestId = -1;

    // Clickable parts of the HD screen (window coordinates in 0..1), filled when drawing
    struct ClickTarget
    {
        float x0, y0, x1, y1;
        int code; // written to the mailbox (CoopUiMenu.click)
    };
    std::vector<ClickTarget> clickTargets;
    int pendingClick = 0;

    // Black cover hiding the game's own screens while it switches between an HD
    // screen and the map (app fade-ins / fade-outs drawn with the old graphics)
    int coverFrames = 0;      // frames left before the cover gives up
    int coverArmed = 0;       // an app was chosen: cover as soon as the map stops (frames left)
    float coverAlpha = 0.0f;
    melonDS::u32 lastFieldFrame = 0;
    int fieldIdleFrames = 100; // frames since the map last ticked (it ticks every other frame)
    int lastScreenType = 0;
    bool started = false;

    // Coop: connection notifications
    bool coopMode = false;     // playing with a friend (HGCOOP_MODE set)
    bool friendConnected = false;
    int networkStatus = -1;
    int lastNetworkStatus = -2;
    QString toastText;
    int toastFrames = 0;       // frames left to show the toast
    bool dirty = true;
    int lastWidth = 0, lastHeight = 0;

    // Coop: names above the characters, in top screen pixels
    static constexpr int kMaxRemotePlayers = 5;
    static constexpr int kPlates = 1 + kMaxRemotePlayers;
    struct Nameplate
    {
        bool visible = false;
        float x = 0, y = 0; // the character's feet
        QString name;
        QColor color;
    };
    Nameplate plates[kPlates]; // us, then the other players (CoopRemote index + 1)
    QString localName;
    std::vector<CoopPlayer> coopPlayers;
    int coopMySlot = -1;
    bool rosterSeen = false;
    QList<QPair<QString, QColor>> toasts; // waiting notifications
    QColor toastColor;
    bool ghostSeen[kMaxRemotePlayers] {};
    QMap<QString, int> arrivedAt; // frame a player connected (no "is here" toast right away)
    int frameCount = 0;
    // Starter choice: which Poke Balls are open (-1 closed, 0..1 opening, 1 open), the choice
    struct StarterState
    {
        bool active = false;
        bool hidden = false;
        float reveal[3] = { -1.0f, -1.0f, -1.0f };
        int cursor = 1;
        int chosen = -1;
        float chosenAnim = 0.0f;
        int frame = 0;
    };
    StarterState starter;
    bool nuzActive = false;
    int nuzBattleCatch = 0; // COOP_CATCH_* of the current wild battle
    bool nuzOver = false;
    int nuzLost = 0;
    struct ChatLine
    {
        QString from, text, to; // to: empty = everybody
        int slot = -1;          // the writer's color
        bool isPrivate = false;
        int frame = 0;          // when it arrived (fades after a while)
    };
    QList<ChatLine> chatLines;
    QList<QPair<int, QString>> chatOut;
    bool chatOpen = false;
    QString chatInput;
    int chatTarget = -1; // relay slot, -1 = everybody
    int chatFadeCheck = 0;
    void chatAdd(const ChatLine& line);
    void chatSend();
    void drawChat(class QPainter& p, int width, int height);
    bool chatVisible() const;
    QString chatTargetName() const;
    // Players screen: what to do with the friend chosen (-1 nothing asked, 0 write, 1 gift)
    int playersAction = -1;
    // Gift screen
    bool giftOpen = false;
    int giftTarget = -1;          // relay slot of the friend
    int giftTab = 0;              // 0 items, 1 money, 2 Pokemon
    int giftCursor = 0, giftScroll = 0;
    int giftQuantity = 1;
    melonDS::u32 giftMoneyAmount = 100;
    bool giftConfirm = false;     // "Offer ... to ...?"
    bool giftCatalogAsked = false, giftHaveCatalog = false;
    melonDS::u32 giftMoney = 0;
    QList<GiftItem> giftItems;
    QList<GiftMon> giftMons;
    QList<GiftChoice> giftChoices;
    struct GiftOffer { int key = 0, slot = -1, kind = 0, itemId = 0, species = 0, quantity = 0; QString from, label; int frame = 0; };
    QList<GiftOffer> giftOffers;  // received, waiting for an answer (the first is shown)
    QList<QPair<int, bool>> giftAnswers;
    // Trade screen
    struct TradeUi
    {
        bool open = false;
        int slot = -1;
        QString partner;
        bool initiator = false;
        bool picking = false;
        int cursor = 0;
        bool haveMine = false, haveTheirs = false;
        GiftMon mine, theirs;
        bool myConfirm = false, theirConfirm = false;
    } tradeUi;
    struct TradeRequest { bool pending = false; int slot = -1; QString from; GiftMon theirs; int frame = 0; } tradeRequest;
    QList<TradeAction> tradeActions;
    bool closeMenuForTrade = false;
    void tradeOpenFor(int slot, bool initiator);
    void tradeInput(melonDS::u32 newPressed);
    void drawTrade(class QPainter& p, int width, int height);
    void drawTradeRequest(class QPainter& p, int width, int height);
    void drawTradeCard(class QPainter& p, const class QRectF& card, qreal u, const QString& title, const GiftMon* mon, const QString& waiting, bool confirmed, bool picking);
    void giftOpenFor(int slot);
    bool giftInput(melonDS::u32 newPressed);
    void drawGift(class QPainter& p, int width, int height);
    void drawGiftOffer(class QPainter& p, int width, int height);
    void drawPlayersAction(class QPainter& p, int width, int height);
    bool playersOpen = false; // the online players screen, over the menu
    bool cemeteryOpen = false; // the Nuzlocke cemetery, over the menu
    int cemeteryCursor = 0;
    QList<QJsonObject> cemetery;
    int playersCursor = 0;
    float topMtx[6] {};
    float topWidgetW = 0, topWidgetH = 0;
    bool topVisible = false;
    QString locationName;
    QJsonObject saveSummary;
    melonDS::u32 lastNamesFrame = 0;
    int namesIdleFrames = 100;

    melonDS::u32 prevPressed = 0;
    int repeatKey = 0;
    int repeatTimer = 0;

    std::vector<int> visibleEntries() const;
    void moveCursor(int dx, int dy);
    void draw(QImage& image, int width, int height);
    void drawStartMenu(class QPainter& p, int width, int height);
    void drawParty(class QPainter& p, int width, int height);
    void drawBag(class QPainter& p, int width, int height);
    void drawShop(class QPainter& p, int width, int height);
    void drawSummary(class QPainter& p, int width, int height);
    void drawBattle(class QPainter& p, int width, int height);
    void drawBattleBag(class QPainter& p, int width, int height);
    void drawBattleParty(class QPainter& p, int width, int height);
    void drawOptions(class QPainter& p, int width, int height);
    void drawTrainerCard(class QPainter& p, int width, int height);
    void drawPokedex(class QPainter& p, int width, int height);
    void drawPhone(class QPainter& p, int width, int height);
    void drawGearMap(class QPainter& p, int width, int height);
    void drawRadio(class QPainter& p, int width, int height);
    void drawMainMenu(class QPainter& p, int width, int height);
    void drawPlayers(class QPainter& p, int width, int height);
    void drawCatchBadge(class QPainter& p, int width, int height);
    void drawCemetery(class QPainter& p, int width, int height);
    void drawNuzlockeOver(class QPainter& p, int width, int height);
    void drawStarter(class QPainter& p, int width, int height);
    // Look screen (start of the adventure, mirror, stylist)
    static constexpr int kLookZones = 8; // the most a character has
    struct Look { int gender = 0; int zone[kLookZones] {}; };
    Look look, lookEdit;
    bool lookActive = false, lookDirty = true;
    int lookRow = 0;
    int lookFrame = 0;
    QImage lookSheet[2]; // assets/look/hero.png, heroine.png (indexed colors)
    std::vector<std::pair<melonDS::u16, melonDS::u16>> lookPairs(const Look& l) const;
    void writeLook(melonDS::u8* mailbox);
    void updateLook(melonDS::NDS* nds, melonDS::u8* mailbox, melonDS::u32 newPressed);
    void drawLook(class QPainter& p, int width, int height);
    // Naming screen (nicknames...): typed with the PC keyboard
    bool namingActive = false;
    QString namingInput;
    QString namingPending; // automated tests
    bool namingSubmit = false;
    bool namingKey(class QKeyEvent* event);
    void updateNaming(melonDS::NDS* nds, melonDS::u8* mailbox, melonDS::u32 newPressed);
    void drawNaming(class QPainter& p, int width, int height);
    void updateStarter(melonDS::NDS* nds, melonDS::u8* mailbox, melonDS::u32 newPressed);
    const QImage& pokemonSprite(int species);
    const QImage& itemIcon(int itemId);
    const QImage& pokemonIcon(int species, bool isEgg);
    void updateScreen(melonDS::u8* mailbox);
    void updateCover(melonDS::u8* mailbox);
    void updateCoopStatus(melonDS::u8* mailbox);
    void drawCoopStatus(class QPainter& p, int width, int height);
    void updateNameplates(melonDS::u8* mailbox);
    void updateInfo(melonDS::u8* mailbox);
    bool anyPlateVisible() const;
    void pushToast(const QString& text, const QColor& color);
    void drawNameplates(class QPainter& p, int width, int height);
};

// Converts text in the game's encoding (u16, 0xFFFF-terminated) to Unicode
QString DecodeGameText(const melonDS::u16* text, int maxLen);

// Converts text to the game's encoding: writes at most maxLen characters and a 0xFFFF
// terminator to out (maxLen + 1 entries). Returns false if a character has no game equivalent.
bool EncodeGameText(const QString& text, melonDS::u16* out, int maxLen);

#endif // GAMEUI_H
