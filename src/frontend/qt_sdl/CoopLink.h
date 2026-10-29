/*
    HeartGold Coop / Ho-Lympe - network link with the other players (up to 6 on a server).

    The patched game keeps a "mailbox" struct in main RAM (see pokeheartgold include/coop.h).
    Every frame we send our player's state to the relay (tools/server/hgcoop_server.cpp) and
    write each other player's state, movements and following Pokemon back into the mailbox.
*/

#ifndef COOPLINK_H
#define COOPLINK_H

#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <QString>
#include "types.h"

namespace melonDS { class NDS; }

class CoopLink
{
public:
    CoopLink();
    ~CoopLink();

    // Call once per emulated frame, after NDS::RunFrame()
    void Update(melonDS::NDS* nds);

    // The game's shared memory block, or nullptr if not found (yet)
    melonDS::u8* Mailbox(melonDS::NDS* nds) const;
    bool GameSaving(melonDS::NDS* nds) const; // the DS writes its save: the emulation runs fast meanwhile

    static constexpr int kOffUiMenu = 328;         // CoopUiMenu, see pokeheartgold include/coop.h
    static constexpr int kOffFieldFrame = 16 + 20; // local CoopPlayerState.frame: advances while the map runs
    static constexpr int kOffNames = 1376;         // CoopNames
    static constexpr int kOffFollowers = 1460;     // CoopFollowers (local one first)
    static constexpr int kOffRemotes = 1476;       // CoopRemote[5]
    static constexpr int kRemoteSize = 180;
    static constexpr int kMaxRemotes = 5;
    static constexpr int kOffInfo = 2376;          // CoopInfo
    static constexpr int kInfoSize = 72;
    static constexpr int kOffNuzlocke = 2448;      // CoopNuzlocke: seed (u32), rules (u8)
    static constexpr int kOffNuzState = 2456;      // CoopNuzlockeState
    static constexpr int kNuzKeep = 64 + 192;      // areas + dead: kept with the save (nuzlocke.dat)
    static constexpr int kNuzDeathCount = 256, kNuzDeaths = 260, kNuzDeathSize = 36,
                         kNuzBattleCatch = 404, kNuzGameOver = 405, kNuzLoaded = 406, kNuzBlockReason = 407, kNuzBallBlocked = 408;
    static constexpr int kOffGift = 2868;          // CoopGift
    static constexpr int kOffTrade = 7160;         // CoopTrade
    static constexpr int kTradeDetailSize = 164, kTradePartnerName = 984;
    static constexpr int kOffLook = 8160;          // CoopLook: gender, count, from[32], to[32]
    static constexpr int kLookColors = 32, kLookSize = 4 + 4 * kLookColors;
    static constexpr int kOffGhostLooks = kOffLook + kLookSize, kGhostLookSize = kLookSize; // CoopGhostLook[5]
    static constexpr int kOffSaving = kOffGhostLooks + 5 * kGhostLookSize; // u16: the game is writing its save
    static constexpr int kOffSpeed = kOffSaving + 2;       // u8: 1 = "Jeu accéléré (x1,5)" (the game slows its music)
    static constexpr int kOffHeartbeat = kOffSaving + 4;   // u32: +1 each turn of the game's main loop
    static constexpr int kMailboxSize = kOffHeartbeat + 4;
    // CoopGift fields
    static constexpr int kGiftRequest = 0, kGiftDone = 4, kGiftCommand = 8, kGiftResult = 9, kGiftItemCount = 10,
                         kGiftMonCount = 11, kGiftMoney = 12, kGiftArg = 16, kGiftArg2 = 20, kGiftMons = 24,
                         kGiftItems = 216, kGiftMon = 4056, kGiftEntrySize = 32, kMonSize = 236;
    enum GiftCommand : melonDS::u8 { kGiftCatalog = 1, kGiftTakeItem, kGiftTakeMoney, kGiftTakeMon, kGiftRoomItem,
                                     kGiftRoomMoney, kGiftRoomMon, kGiftGiveItem, kGiftGiveMoney, kGiftGiveMon,
                                     kGiftPeekMon, kGiftTradeMon };

    // CoopRemote fields
    static constexpr int kRemState = 0, kRemEventCount = 24, kRemEvents = 28, kRemFollower = 156,
                         kRemActive = 164, kRemGhostVisible = 165, kRemGhostPos = 168;

private:
    static constexpr melonDS::u32 kMagic0 = 0x504F4F43; // "COOP"
    static constexpr melonDS::u32 kMagic1 = 0x32564748; // "HGV2"
    static constexpr melonDS::u32 kVersion = 8;
    static constexpr int kStateSize = 24;
    static constexpr int kOffLocal = 16;
    static constexpr int kOffLocalFrame = kOffLocal + 20;
    static constexpr int kOffLocalEventCount = 64;
    static constexpr int kOffLocalEvents = 72;
    static constexpr int kEventSize = 8;
    static constexpr int kEventCount = 16;
    static constexpr int kFollowerSize = 8;
    static constexpr int kNameLength = 7; // PLAYER_NAME_LENGTH
    static constexpr int kTimeoutFrames = 180;
    static constexpr int kMaxSlots = 6;

    // Packets (see the relay): data types are relayed as type, sender slot, payload
    static constexpr melonDS::u8 kPacketPlayerState = 1;
    static constexpr melonDS::u8 kPacketMoveEvent = 2;
    static constexpr melonDS::u8 kPacketFollower = 4;
    static constexpr melonDS::u8 kPacketInfo = 5;
    static constexpr melonDS::u8 kPacketChat = 6; // addressed: target slot (0xFF = all), UTF-8 text
    static constexpr melonDS::u8 kPacketGift = 7; // addressed: target, op (kGiftOp*), gift id (u32), ...
    enum GiftOp : melonDS::u8 { kGiftOffer = 1, kGiftAccept, kGiftDecline, kGiftData, kGiftCancel, kGiftReturn };
    static constexpr melonDS::u8 kPacketTrade = 8; // addressed: target, op (kTrade*), then the op's data
    static constexpr melonDS::u8 kPacketLook = 9;  // addressed (to all): our look, "gender:zones" (GameUi::LookString)
    enum TradeOp : melonDS::u8 { kTradeRequest = 1, kTradeOffer, kTradeConfirm, kTradeCancel, kTradeData };
    static constexpr melonDS::u8 kPacketHello = 0x10, kPacketWelcome = 0x11, kPacketRoster = 0x12, kPacketReject = 0x13;

    // What we know about each player on the server, by relay slot
    struct Player
    {
        bool present = false; // in the roster
        std::string name;
        melonDS::u8 state[kStateSize] {};
        bool haveState = false;
        int framesSinceState = kTimeoutFrames;
        std::vector<melonDS::u8> events; // received, not yet written to RAM
        melonDS::u8 follower[kFollowerSize] {};
        melonDS::u8 info[kInfoSize] {};
        bool haveInfo = false;
        QString look;              // their look string
        melonDS::u8 lookSerial = 0;
    };

    bool enabled = false;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    ENetAddress serverAddr {};
    int reconnectCooldown = 0;
    int mySlot = -1;   // given by the relay
    int rejected = 0;  // relay's refusal reason (1 session, 2 full, 3 kicked)
    Player players[kMaxSlots];
    std::string token, localName;
    melonDS::u16 localNameCode[kNameLength + 1] {};
    bool haveLocalName = false;
    melonDS::u32 seed = 0;   // Nuzlocke randomizer seed of this run (HGCOOP_SEED), 0 = classic
    melonDS::u8 rules = 0;   // COOP_RULE_* (HGCOOP_WORLD nuzlocke + HGCOOP_RULES options)

    melonDS::u32 mailbox = 0; // offset into main RAM, 0 = not found yet
    int scanCooldown = 0;
    melonDS::u32 lastSentFrame = 0xFFFFFFFF;
    melonDS::u32 lastSentEvent = 0;
    melonDS::u8 lastSentFollower[kFollowerSize] {};
    int followerCooldown = 0;
    int lookCooldown = 0;
    int infoCooldown = 0;

    FILE* debugLog = nullptr; // HGCOOP_SHOTS/coop.log, for automated tests
    melonDS::u32 frameCounter = 0;

    // Nuzlocke: the run's state goes to <save folder>/nuzlocke.dat, deaths to cemetery.json
    QString saveDir;
    melonDS::u8 nuzSaved[kNuzKeep] {};
    melonDS::u32 nuzDeaths = 0, nuzBallBlocked = 0;
    bool nuzOverSent = false;
    long long nuzCapToastFrame = -100000; // the last "level cap" message
    void nuzlockeSync(melonDS::u8* box);

    // Gifts: commands run by the game on the save, one at a time (see CoopGift in coop.h)
    struct GiftCommandJob
    {
        melonDS::u8 command = 0;
        melonDS::u32 arg = 0, arg2 = 0;
        std::vector<melonDS::u8> mon;
        std::function<void(bool ok, const melonDS::u8* gift)> done;
        std::vector<melonDS::u16> partnerName; // trades: the friend's name, for the animation
    };
    std::deque<GiftCommandJob> giftJobs;
    bool giftRunning = false;
    bool giftSynced = false;
    melonDS::u32 giftRequest = 0;
    struct GiftDeal // what a gift is: item (id, quantity), money (quantity), Pokemon (id = party slot)
    {
        int slot = -1; // the other player
        melonDS::u32 id = 0;
        int kind = 0, item = 0;
        melonDS::u32 quantity = 0;
        QString label;
        std::vector<melonDS::u8> mon;
    };
    std::map<melonDS::u32, GiftDeal> giftsOut; // offered by us, by gift id
    std::map<melonDS::u64, GiftDeal> giftsIn;  // offered to us, by (slot, gift id)
    melonDS::u32 giftNextId = 1;
    void giftQueue(melonDS::u8 command, melonDS::u32 arg, melonDS::u32 arg2, std::function<void(bool, const melonDS::u8*)> done,
                   const std::vector<melonDS::u8>& mon = {});
    void giftRun(melonDS::u8* box);
    void giftUpdate();
    void giftSend(int target, melonDS::u8 op, melonDS::u32 id, const std::vector<melonDS::u8>& extra = {});
    void giftPacket(int slot, const melonDS::u8* payload, size_t size);
    QString playerName(int slot) const;

    // Trades (one at a time): both choose, both confirm, both send the Pokemon, then each game
    // plays the trade animation
    struct TradeSession
    {
        bool active = false;
        int slot = -1;
        bool initiator = false;
        int myIndex = -1;            // our party slot
        melonDS::u32 myPersonality = 0;
        bool myConfirm = false, theirConfirm = false;
        bool peeking = false, peeked = false, executing = false;
        std::vector<melonDS::u8> theirData;
    } trade;
    void tradeUpdate();
    void tradePacket(int slot, const melonDS::u8* payload, size_t size);
    void tradeSend(int target, melonDS::u8 op, const std::vector<melonDS::u8>& extra = {});
    void tradeTry();

    // A frozen game (its main loop stopped): a report next to the launcher, to find the cause
    melonDS::u32 lastHeartbeat = 0;
    int frozenFrames = 0;
    std::vector<melonDS::u32> freezePcs;
    void freezeWatch(melonDS::NDS* nds, melonDS::u8* box);
    void writeReport(melonDS::NDS* nds, melonDS::u8* box, const char* title, const std::vector<melonDS::u32>& pcs);
    std::vector<melonDS::u32> recentPcs;
    bool fastGame = getenv("HGCOOP_SPEED") != nullptr && atof(getenv("HGCOOP_SPEED")) > 1.0;

    void serviceNetwork();
    void onPacket(const melonDS::u8* data, size_t len);
    void publishPlayers();
    int remoteIndex(int slot) const { return slot < mySlot ? slot : slot - 1; }
    bool mailboxValid(melonDS::NDS* nds) const;
    melonDS::u32 findMailbox(melonDS::NDS* nds) const;
};

#endif // COOPLINK_H
