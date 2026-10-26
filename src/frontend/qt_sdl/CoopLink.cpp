/*
    HeartGold Coop / Ho-Lympe - network link (see CoopLink.h).

    Configuration (environment variables, set by the launcher):
      HGCOOP_MODE  = join          (unset = no network: solo)
      HGCOOP_ADDR  = relay address
      HGCOOP_PORT  = UDP port      (7777 Classique, 7778 Nuzlocke)
      HGCOOP_TOKEN = launcher session (the relay checks it); HGCOOP_NAME = our name
*/

#include "HGCoop.h"
#include <QTextStream>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "CoopLink.h"
#include "GameUi.h"
#include "NDS.h"
#include "Platform.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

static u32 ReadU32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void WriteU32(u8* p, u32 v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = v >> 24;
}

CoopLink::CoopLink()
{
    // Our name: from the launcher, or the last one chosen (coop.ini)
    QString name = QString::fromUtf8(getenv("HGCOOP_NAME") ? getenv("HGCOOP_NAME") : "");
    if (name.isEmpty())
        name = QSettings(HoLympeDir() + "/coop.ini", QSettings::IniFormat).value("lastName").toString();
    name = name.trimmed().left(kNameLength);
    if (!name.isEmpty() && EncodeGameText(name, localNameCode, kNameLength))
    {
        localName = name.toStdString();
        haveLocalName = true;
    }
    GameUi::Get().SetLocalName(name);

    saveDir = QString::fromUtf8(getenv("HGCOOP_SAVE_DIR") ? getenv("HGCOOP_SAVE_DIR") : "");
    GameUi::Get().SetLook(QString::fromUtf8(getenv("HGCOOP_LOOK") ? getenv("HGCOOP_LOOK") : ""));

    // Nuzlocke: on the Nuzlocke server or a solo Nuzlocke game, with the run's seed
    QString world = QString::fromUtf8(getenv("HGCOOP_WORLD") ? getenv("HGCOOP_WORLD") : "");
    if (world.endsWith("nuzlocke"))
    {
        seed = (u32)QString::fromUtf8(getenv("HGCOOP_SEED") ? getenv("HGCOOP_SEED") : "0").toULongLong();
        rules = 1 | ((getenv("HGCOOP_RULES") ? atoi(getenv("HGCOOP_RULES")) : 0) & 6);
    }

    const char* mode = getenv("HGCOOP_MODE");
    if (!mode)
        return;
    if (const char* t = getenv("HGCOOP_TOKEN"))
        token = t;

    const char* portStr = getenv("HGCOOP_PORT");
    enet_uint16 port = portStr ? (enet_uint16)atoi(portStr) : 7777;
    if (enet_initialize() != 0)
    {
        Log(LogLevel::Error, "[coop] enet_initialize failed\n");
        return;
    }
    const char* addrStr = getenv("HGCOOP_ADDR");
    host = enet_host_create(nullptr, 1, 2, 0, 0);
    enet_address_set_host(&serverAddr, addrStr ? addrStr : "127.0.0.1");
    serverAddr.port = port;
    if (!host)
    {
        Log(LogLevel::Error, "[coop] could not create network host\n");
        enet_deinitialize();
        return;
    }
    Log(LogLevel::Info, "[coop] joining %s:%d\n", addrStr ? addrStr : "127.0.0.1", port);
    enabled = true;

    if (const char* dir = getenv("HGCOOP_SHOTS"))
        debugLog = fopen((std::string(dir) + "/coop.log").c_str(), "w");
}

CoopLink::~CoopLink()
{
    if (!enabled)
        return;
    if (debugLog)
        fclose(debugLog);
    if (peer)
        enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(host);
    enet_deinitialize();
}

void CoopLink::serviceNetwork()
{
    if (!peer && !rejected && --reconnectCooldown <= 0)
    {
        peer = enet_host_connect(host, &serverAddr, 2, 0);
        reconnectCooldown = 180;
    }

    ENetEvent event;
    while (enet_host_service(host, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
        {
            peer = event.peer;
            // Who we are: the relay checks the launcher session
            std::vector<u8> hello { kPacketHello, (u8)token.size() };
            hello.insert(hello.end(), token.begin(), token.end());
            hello.push_back((u8)localName.size());
            hello.insert(hello.end(), localName.begin(), localName.end());
            enet_peer_send(peer, 1, enet_packet_create(hello.data(), hello.size(), ENET_PACKET_FLAG_RELIABLE));
            Log(LogLevel::Info, "[coop] connected to the relay\n");
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE:
            onPacket(event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            if (event.peer == peer)
                peer = nullptr;
            mySlot = -1;
            for (Player& p : players)
                p = Player();
            publishPlayers();
            Log(LogLevel::Info, "[coop] disconnected from the relay\n");
            break;

        default:
            break;
        }
    }
}

void CoopLink::onPacket(const u8* data, size_t len)
{
    if (len < 2)
        return;
    switch (data[0])
    {
    case kPacketWelcome:
        mySlot = data[1] < kMaxSlots ? data[1] : -1;
        return;
    case kPacketReject:
        rejected = data[1];
        GameUi::Get().SetNetworkStatus(-2 - rejected); // shown in the status pill
        return;
    case kPacketRoster:
    {
        bool present[kMaxSlots] {};
        std::string names[kMaxSlots];
        size_t pos = 2;
        for (int n = 0; n < data[1] && pos + 2 <= len; n++)
        {
            int slot = data[pos];
            size_t nameLen = data[pos + 1];
            pos += 2;
            if (slot < kMaxSlots && pos + nameLen <= len)
            {
                present[slot] = true;
                names[slot] = std::string((const char*)data + pos, nameLen);
            }
            pos += nameLen;
        }
        for (int s = 0; s < kMaxSlots; s++)
        {
            if (!present[s])
                players[s] = Player();
            else
            {
                players[s].present = true;
                players[s].name = names[s];
            }
        }
        publishPlayers();
        return;
    }
    default:
        break;
    }

    // Data from another player: type, sender slot, payload
    int slot = data[1];
    if (slot >= kMaxSlots || slot == mySlot)
        return;
    Player& p = players[slot];
    const u8* payload = data + 2;
    size_t size = len - 2;
    if (data[0] == kPacketPlayerState && size == (size_t)kStateSize)
    {
        memcpy(p.state, payload, kStateSize);
        p.haveState = true;
        p.framesSinceState = 0;
    }
    else if (data[0] == kPacketMoveEvent && size == (size_t)kEventSize)
        p.events.insert(p.events.end(), payload, payload + kEventSize);
    else if (data[0] == kPacketFollower && size == (size_t)kFollowerSize)
        memcpy(p.follower, payload, kFollowerSize);
    else if (data[0] == kPacketChat && size >= 2 && p.present)
    {
        QString text = QString::fromUtf8((const char*)payload + 1, (int)std::min<size_t>(size - 1, 400));
        GameUi::Get().ChatReceived(slot, QString::fromStdString(p.name), text.left(120), payload[0] != 0xFF);
    }
    else if (data[0] == kPacketGift && size >= 6 && p.present)
        giftPacket(slot, payload, size);
    else if (data[0] == kPacketTrade && size >= 2 && p.present)
        tradePacket(slot, payload, size);
    else if (data[0] == kPacketLook && size >= 2 && p.present)
    {
        QString look = QString::fromUtf8((const char*)payload + 1, (int)std::min<size_t>(size - 1, 40));
        if (look != p.look)
        {
            p.look = look;
            p.lookSerial++;
        }
    }
    else if (data[0] == kPacketInfo && size == (size_t)kInfoSize)
    {
        memcpy(p.info, payload, kInfoSize);
        p.haveInfo = true;
        publishPlayers();
    }
}

// The Nuzlocke run state: loaded into the game once, written back to nuzlocke.dat when it
// changes; each death goes to the cemetery; refused balls and the end of the run are shown
void CoopLink::nuzlockeSync(u8* box)
{
    u8* nuz = box + kOffNuzState;
    QString statePath = saveDir + "/nuzlocke.dat";
    if (!nuz[kNuzLoaded])
    {
        QFile file(statePath);
        QByteArray data = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        memset(nuz, 0, kNuzKeep);
        memcpy(nuz, data.constData(), std::min<qsizetype>(data.size(), kNuzKeep));
        memcpy(nuzSaved, nuz, kNuzKeep);
        nuzDeaths = ReadU32(nuz + kNuzDeathCount);
        nuzBallBlocked = ReadU32(nuz + kNuzBallBlocked);
        nuz[kNuzLoaded] = 1;
        GameUi::Get().SetNuzlockeLost(QByteArray((const char*)nuz + 64, 192));
        return;
    }
    if (frameCounter % 30 == 0 && memcmp(nuz, nuzSaved, kNuzKeep) != 0)
    {
        memcpy(nuzSaved, nuz, kNuzKeep);
        QFile file(statePath);
        if (file.open(QIODevice::WriteOnly))
            file.write((const char*)nuzSaved, kNuzKeep);
        GameUi::Get().SetNuzlockeLost(QByteArray((const char*)nuzSaved + 64, 192));
    }
    u32 deaths = ReadU32(nuz + kNuzDeathCount);
    for (; nuzDeaths < deaths; nuzDeaths++)
    {
        const u8* d = nuz + kNuzDeaths + (nuzDeaths % 4) * kNuzDeathSize;
        QJsonObject entry {
            { "name", DecodeGameText((const u16*)(d + 12), 12) },
            { "species", d[4] | (d[5] << 8) },
            { "level", d[6] },
            { "killer", d[10] | (d[11] << 8) },
            { "location", GameUi::Get().LocationName() },
            { "date", QDateTime::currentSecsSinceEpoch() },
        };
        QFile file(saveDir + "/cemetery.json");
        QJsonArray cemetery = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).array() : QJsonArray();
        file.close();
        cemetery.append(entry);
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(cemetery).toJson(QJsonDocument::Compact));
        GameUi::Get().NuzlockeMessage(QString::fromUtf8("%1 est tombé…").arg(entry.value("name").toString()), QColor(200, 70, 70));
    }
    u32 blocked = ReadU32(nuz + kNuzBallBlocked);
    if (blocked != nuzBallBlocked)
    {
        nuzBallBlocked = blocked;
        int reason = nuz[kNuzBlockReason];
        // The level cap refuses experience after each battle: said once in a while
        bool quiet = reason == 3 && (long long)frameCounter - nuzCapToastFrame < 60 * 60;
        if (reason == 3 && !quiet)
            nuzCapToastFrame = frameCounter;
        QString text = reason == 2 ? QString::fromUtf8("Le Nuzlocke interdit les soins en combat")
                     : reason == 3 ? QString::fromUtf8("Plafond de niveau atteint : bats le prochain champion")
                                   : QString::fromUtf8("Le Nuzlocke t'interdit de capturer ici");
        if (!quiet)
            GameUi::Get().NuzlockeMessage(text, QColor(239, 106, 90));
    }
    if (nuz[kNuzGameOver] && !nuzOverSent)
    {
        nuzOverSent = true;
        GameUi::Get().SetNuzlockeOver();
    }
}

// The players list for the HD screens (names, places, teams, notifications)
void CoopLink::publishPlayers()
{
    std::vector<GameUi::CoopPlayer> list;
    for (int s = 0; s < kMaxSlots; s++)
    {
        if (!players[s].present || s == mySlot)
            continue;
        GameUi::CoopPlayer p;
        p.slot = s;
        p.remote = mySlot >= 0 ? remoteIndex(s) : -1;
        p.name = QString::fromStdString(players[s].name);
        if (players[s].haveInfo)
        {
            p.location = DecodeGameText((const u16*)players[s].info, 24);
            for (int i = 0; i < 6; i++)
            {
                u16 species = players[s].info[48 + i * 2] | (players[s].info[49 + i * 2] << 8);
                if (species)
                    p.team.append(species);
            }
            p.badges = players[s].info[64];
            p.gearX = (s16)(players[s].info[66] | (players[s].info[67] << 8));
            p.gearY = (s16)(players[s].info[68] | (players[s].info[69] << 8));
        }
        list.push_back(p);
    }
    GameUi::Get().SetCoopPlayers(list, mySlot);
}

bool CoopLink::mailboxValid(NDS* nds) const
{
    if (!mailbox)
        return false;
    const u8* p = nds->MainRAM + mailbox;
    return ReadU32(p) == kMagic0 && ReadU32(p + 4) == kMagic1 && ReadU32(p + 8) == kVersion;
}

u32 CoopLink::findMailbox(NDS* nds) const
{
    const u8* ram = nds->MainRAM;
    u32 size = nds->MainRAMMask + 1;
    for (u32 off = 4; off + kMailboxSize <= size; off += 4)
    {
        if (ReadU32(ram + off) == kMagic0 && ReadU32(ram + off + 4) == kMagic1 && ReadU32(ram + off + 8) == kVersion)
            return off;
    }
    return 0;
}

u8* CoopLink::Mailbox(NDS* nds) const
{
    return mailboxValid(nds) ? nds->MainRAM + mailbox : nullptr;
}

// The game's main loop counts its turns (heartbeat): stopped for 5 s (not while it saves), the
// game is frozen. Where the ARM9 runs then goes to freeze-<date>.txt next to the launcher, with
// the open HD screen: the build's symbols tell which function is stuck.
void CoopLink::freezeWatch(NDS* nds, u8* box)
{
    // The last second's PCs, for a report asked with F12
    recentPcs.push_back(nds->ARM9.R[15]);
    if (recentPcs.size() > 60)
        recentPcs.erase(recentPcs.begin());
    if (GameUi::Get().TakeReportRequest())
    {
        writeReport(nds, box, "Ho-Lympe: report asked by the player (F12)", recentPcs);
        GameUi::Get().NuzlockeMessage(QString::fromUtf8("Rapport enregistré pour Claude"), QColor(90, 160, 220));
    }

    u32 beat = ReadU32(box + kOffHeartbeat);
    if (beat != lastHeartbeat || ReadU32(box + kOffSaving) != 0)
    {
        lastHeartbeat = beat;
        frozenFrames = 0;
        freezePcs.clear();
        return;
    }
    if (++frozenFrames > 300)
        return; // reported
    if (frozenFrames > 240)
        freezePcs.push_back(nds->ARM9.R[15]);
    if (frozenFrames < 300)
        return;
    writeReport(nds, box, "Ho-Lympe: the game froze (main loop stopped 5 s)", freezePcs);
    GameUi::Get().NuzlockeMessage(QString::fromUtf8("Le jeu s'est figé : rapport enregistré pour Claude"), QColor(200, 120, 60));
}

void CoopLink::writeReport(NDS* nds, u8* box, const char* title, const std::vector<u32>& pcs)
{
    QFile file(HoLympeDir() + "/freeze-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << title << "\n";
    out << "mailbox 0x" << QString::number(0x02000000 + mailbox, 16) << "  heartbeat " << ReadU32(box + kOffHeartbeat)
        << "  saving " << ReadU32(box + kOffSaving) << "\n";
    out << "keys 0x" << QString::number(nds->KeyInput, 16) << "\n";
    out << GameUi::Get().DebugState();
    out << "ui block:";
    for (int i = 0; i < 64; i++)
        out << " " << QString::number(box[kOffUiMenu + i], 16);
    out << "\nscreen block:";
    for (int i = 0; i < 32; i++)
        out << " " << QString::number(box[kOffUiMenu + 352 + i], 16);
    out << "\n";
    out << "ui type " << (int)box[kOffUiMenu + 0] << "  screen type " << (int)box[kOffUiMenu + 24] << "\n";
    out << "CPSR " << QString::number(nds->ARM9.CPSR, 16) << "\n";
    for (int i = 0; i < 16; i++)
        out << "R" << i << " " << QString::number(nds->ARM9.R[i], 16) << (i % 4 == 3 ? "\n" : "  ");
    // An exception (data abort...): the program's own SP / LR are kept aside by the emulator
    const bool abort = (nds->ARM9.CPSR & 0x1F) == 0x17 || (nds->ARM9.CPSR & 0x1F) == 0x1B;
    out << "banked: ABT " << QString::number(nds->ARM9.R_ABT[0], 16) << " " << QString::number(nds->ARM9.R_ABT[1], 16) << " " << QString::number(nds->ARM9.R_ABT[2], 16)
        << "  UND " << QString::number(nds->ARM9.R_UND[0], 16) << " " << QString::number(nds->ARM9.R_UND[1], 16)
        << "  SVC " << QString::number(nds->ARM9.R_SVC[0], 16) << " " << QString::number(nds->ARM9.R_SVC[1], 16)
        << "  IRQ " << QString::number(nds->ARM9.R_IRQ[0], 16) << " " << QString::number(nds->ARM9.R_IRQ[1], 16) << "\n";
    out << "PC samples:";
    for (u32 pc : pcs)
        out << " " << QString::number(pc, 16);
    out << "\nstack (from SP):\n";
    u32 sp = abort ? ((nds->ARM9.CPSR & 0x1F) == 0x17 ? nds->ARM9.R_ABT[0] : nds->ARM9.R_UND[0]) : nds->ARM9.R[13];
    for (int i = 0; i < 128; i++)
        out << QString::number(nds->ARM9Read32(sp + i * 4), 16) << (i % 8 == 7 ? "\n" : " ");
    file.close();
}

bool CoopLink::GameSaving(NDS* nds) const
{
    const u8* box = Mailbox(nds);
    return box && ReadU32(box + kOffSaving) != 0;
}

// ---------------------------------------------------------------------------
// Gifts between players
//   sender                         receiver
//   OFFER (what, label)    ->      asks the player; when accepted, checks there is room
//                          <-      ACCEPT (or DECLINE, reason 1 = no room)
//   takes it from the save
//   DATA (with the Pokemon) ->     puts it in the save
//   (CANCEL if it was gone) <-     RETURN when it could not be placed after all
// ---------------------------------------------------------------------------

static void PutU16(std::vector<u8>& v, u32 x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void PutU32(std::vector<u8>& v, u32 x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF); }

QString CoopLink::playerName(int slot) const
{
    return slot >= 0 && slot < kMaxSlots ? QString::fromStdString(players[slot].name) : QString();
}

void CoopLink::giftQueue(u8 command, u32 arg, u32 arg2, std::function<void(bool, const u8*)> done, const std::vector<u8>& mon)
{
    giftJobs.push_back({ command, arg, arg2, mon, std::move(done) });
}

// One command at a time: the game runs it when it can (changes only on the map, out of menus)
void CoopLink::giftRun(u8* box)
{
    u8* gift = box + kOffGift;
    if (!giftSynced)
    {
        giftRequest = ReadU32(gift + kGiftRequest);
        WriteU32(gift + kGiftDone, giftRequest);
        giftRunning = false;
        giftSynced = true;
    }
    if (giftRunning)
    {
        if (ReadU32(gift + kGiftDone) != giftRequest)
            return;
        giftRunning = false;
        GiftCommandJob job = std::move(giftJobs.front());
        giftJobs.pop_front();
        if (job.done)
            job.done(gift[kGiftResult] != 0, gift);
    }
    if (giftJobs.empty())
        return;
    const GiftCommandJob& job = giftJobs.front();
    WriteU32(gift + kGiftArg, job.arg);
    WriteU32(gift + kGiftArg2, job.arg2);
    if (!job.mon.empty())
        memcpy(gift + kGiftMon, job.mon.data(), std::min<size_t>(job.mon.size(), kMonSize));
    if (!job.partnerName.empty())
        memcpy(box + kOffTrade + kTradePartnerName, job.partnerName.data(), std::min<size_t>(job.partnerName.size(), 8) * 2);
    gift[kGiftCommand] = job.command;
    gift[kGiftResult] = 0;
    WriteU32(gift + kGiftRequest, ++giftRequest);
    giftRunning = true;
}

void CoopLink::giftSend(int target, u8 op, u32 id, const std::vector<u8>& extra)
{
    if (!peer || mySlot < 0)
        return;
    std::vector<u8> packet { kPacketGift, (u8)target, op };
    PutU32(packet, id);
    packet.insert(packet.end(), extra.begin(), extra.end());
    enet_peer_send(peer, 1, enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE));
}

// What the player did in the HD screens: the gift screen needs the bag, a gift was chosen,
// an offer was answered
void CoopLink::giftUpdate()
{
    GameUi& ui = GameUi::Get();
    if (ui.GiftCatalogWanted())
    {
        giftQueue(kGiftCatalog, 0, 0, [](bool, const u8* gift) {
            QList<GameUi::GiftItem> items;
            QList<GameUi::GiftMon> mons;
            for (int i = 0; i < std::min<int>(gift[kGiftItemCount], 120); i++)
            {
                const u8* e = gift + kGiftItems + i * kGiftEntrySize;
                items.append({ e[0] | (e[1] << 8), e[2] | (e[3] << 8), DecodeGameText((const u16*)(e + 4), 14) });
            }
            for (int i = 0; i < std::min<int>(gift[kGiftMonCount], 6); i++)
            {
                const u8* e = gift + kGiftMons + i * kGiftEntrySize;
                GameUi::GiftMon mon { e[0] | (e[1] << 8), e[2] | (e[3] << 8), e[4], e[5] != 0, e[6] != 0, e[7], DecodeGameText((const u16*)(e + 8), 12) };
                const u8* d = gift - kOffGift + kOffTrade + i * kTradeDetailSize;
                mon.personality = ReadU32(d);
                for (int s = 0; s < 6; s++)
                    mon.stats[s] = d[4 + s * 2] | (d[5 + s * 2] << 8);
                for (int m = 0; m < 4; m++)
                {
                    QString move = DecodeGameText((const u16*)(d + 16 + m * 24), 12);
                    if (!move.isEmpty())
                        mon.moves.append(move);
                }
                mon.itemName = DecodeGameText((const u16*)(d + 112), 14);
                mon.ability = DecodeGameText((const u16*)(d + 140), 12);
                mons.append(mon);
            }
            GameUi::Get().SetGiftCatalog(ReadU32(gift + kGiftMoney), items, mons);
        });
    }

    GameUi::GiftChoice choice;
    while (ui.TakeGiftChoice(choice))
    {
        GiftDeal deal;
        deal.slot = choice.target;
        deal.id = giftNextId++;
        deal.kind = choice.kind;
        deal.item = choice.id;
        deal.quantity = choice.quantity;
        deal.label = choice.label;
        std::vector<u8> extra { (u8)deal.kind };
        PutU16(extra, deal.item);
        PutU32(extra, deal.quantity);
        PutU16(extra, choice.species);
        extra.push_back((u8)choice.level);
        QByteArray label = deal.label.toUtf8().left(120);
        extra.push_back((u8)label.size());
        extra.insert(extra.end(), label.begin(), label.end());
        giftSend(deal.slot, kGiftOffer, deal.id, extra);
        ui.NuzlockeMessage(QString::fromUtf8("Cadeau proposé à %1…").arg(playerName(deal.slot)), QColor(240, 186, 40));
        giftsOut[deal.id] = deal;
    }

    int key;
    bool accept;
    while (ui.TakeGiftAnswer(key, accept))
    {
        auto it = giftsIn.find((u64)key);
        if (it == giftsIn.end())
            continue;
        GiftDeal deal = it->second;
        if (!accept)
        {
            giftSend(deal.slot, kGiftDecline, deal.id, { 0 });
            giftsIn.erase(it);
            continue;
        }
        u8 command = deal.kind == 1 ? kGiftRoomItem : deal.kind == 2 ? kGiftRoomMoney : kGiftRoomMon;
        u32 arg = deal.kind == 1 ? deal.item : deal.quantity;
        giftQueue(command, arg, deal.quantity, [this, key, deal](bool ok, const u8*) {
            if (ok)
            {
                giftSend(deal.slot, kGiftAccept, deal.id);
                return;
            }
            giftSend(deal.slot, kGiftDecline, deal.id, { 1 });
            giftsIn.erase((u64)key);
            GameUi::Get().NuzlockeMessage(QString::fromUtf8(deal.kind == 3 ? "Plus de place dans ton équipe ni ton PC"
                                                            : deal.kind == 2 ? "Tu as déjà trop d'argent pour ce cadeau"
                                                                             : "Ton sac est plein pour ce cadeau"),
                                          QColor(239, 106, 90));
        });
    }
}

void CoopLink::giftPacket(int slot, const u8* payload, size_t size)
{
    // payload: target, op, gift id, ...
    u8 op = payload[1];
    u32 id = ReadU32(payload + 2);
    const u8* rest = payload + 6;
    size_t restSize = size - 6;
    GameUi& ui = GameUi::Get();
    QString from = playerName(slot);
    u64 inKey = ((u64)slot << 32) | id;

    switch (op)
    {
    case kGiftOffer:
    {
        if (restSize < 11)
            return;
        GiftDeal deal;
        deal.slot = slot;
        deal.id = id;
        deal.kind = rest[0];
        deal.item = rest[1] | (rest[2] << 8);
        deal.quantity = ReadU32(rest + 3);
        int species = rest[7] | (rest[8] << 8);
        int labelLen = std::min<int>(rest[10], (int)restSize - 11);
        deal.label = QString::fromUtf8((const char*)rest + 11, std::max(0, labelLen));
        if (deal.kind < 1 || deal.kind > 3)
            return;
        // Small key for the UI: unique enough among the offers waiting
        int key = (int)(((u32)slot << 24) | (id & 0xFFFFFF));
        giftsIn[(u64)key] = deal;
        ui.GiftOffered(key, slot, from, deal.label, deal.kind, deal.item, species, deal.quantity);
        return;
    }
    case kGiftAccept:
    {
        auto it = giftsOut.find(id);
        if (it == giftsOut.end() || it->second.slot != slot)
            return;
        GiftDeal deal = it->second;
        u8 command = deal.kind == 1 ? kGiftTakeItem : deal.kind == 2 ? kGiftTakeMoney : kGiftTakeMon;
        u32 arg = deal.kind == 1 ? deal.item : deal.kind == 2 ? deal.quantity : deal.item;
        giftQueue(command, arg, deal.quantity, [this, deal, from](bool ok, const u8* gift) {
            giftsOut.erase(deal.id);
            if (!ok)
            {
                giftSend(deal.slot, kGiftCancel, deal.id);
                GameUi::Get().NuzlockeMessage(deal.kind == 3 ? QString::fromUtf8("Ce Pokémon ne peut pas partir (il faut en garder un pour combattre)")
                                                             : QString::fromUtf8("Tu n'as plus ce cadeau"), QColor(239, 106, 90));
                return;
            }
            std::vector<u8> extra { (u8)deal.kind };
            PutU16(extra, deal.item);
            PutU32(extra, deal.quantity);
            if (deal.kind == 3)
                extra.insert(extra.end(), gift + kGiftMon, gift + kGiftMon + kMonSize);
            giftSend(deal.slot, kGiftData, deal.id, extra);
            GameUi::Get().NuzlockeMessage(QString::fromUtf8("%1 a accepté : %2").arg(from, deal.label), QColor(76, 176, 120));
        });
        return;
    }
    case kGiftDecline:
    {
        auto it = giftsOut.find(id);
        if (it == giftsOut.end() || it->second.slot != slot)
            return;
        giftsOut.erase(it);
        ui.NuzlockeMessage(restSize >= 1 && rest[0] == 1 ? QString::fromUtf8("%1 n'a pas de place pour ton cadeau").arg(from)
                                                         : QString::fromUtf8("%1 a refusé ton cadeau").arg(from), QColor(239, 106, 90));
        return;
    }
    case kGiftData:
    case kGiftReturn:
    {
        if (restSize < 7)
            return;
        int kind = rest[0];
        u32 item = rest[1] | (rest[2] << 8);
        u32 quantity = ReadU32(rest + 3);
        std::vector<u8> mon;
        if (kind == 3)
        {
            if (restSize < 7 + (size_t)kMonSize)
                return;
            mon.assign(rest + 7, rest + 7 + kMonSize);
        }
        QString label;
        if (op == kGiftData)
        {
            int key = (int)(((u32)slot << 24) | (id & 0xFFFFFF));
            auto it = giftsIn.find((u64)key);
            if (it == giftsIn.end())
                return; // never accepted here
            label = it->second.label;
            giftsIn.erase(it);
        }
        u8 command = kind == 1 ? kGiftGiveItem : kind == 2 ? kGiftGiveMoney : kGiftGiveMon;
        u32 arg = kind == 1 ? item : quantity;
        bool returned = op == kGiftReturn;
        giftQueue(command, arg, quantity, [this, slot, id, kind, item, quantity, mon, label, from, returned](bool ok, const u8*) {
            if (returned)
            {
                GameUi::Get().NuzlockeMessage(QString::fromUtf8("Ton cadeau t'a été rendu"), QColor(240, 186, 40));
                return;
            }
            if (ok)
            {
                GameUi::Get().NuzlockeMessage(QString::fromUtf8("Cadeau de %1 : %2 !").arg(from, label), QColor(76, 176, 120));
                return;
            }
            // Could not be placed after all: back to the sender
            std::vector<u8> extra { (u8)kind };
            PutU16(extra, item);
            PutU32(extra, quantity);
            extra.insert(extra.end(), mon.begin(), mon.end());
            giftSend(slot, kGiftReturn, id, extra);
            GameUi::Get().NuzlockeMessage(QString::fromUtf8("Pas de place pour le cadeau : il retourne à %1").arg(from), QColor(239, 106, 90));
        }, mon);
        return;
    }
    case kGiftCancel:
    {
        int key = (int)(((u32)slot << 24) | (id & 0xFFFFFF));
        giftsIn.erase((u64)key);
        ui.NuzlockeMessage(QString::fromUtf8("%1 n'a plus ce cadeau").arg(from), QColor(239, 106, 90));
        return;
    }
    default:
        return;
    }
    (void)inKey;
}

// ---------------------------------------------------------------------------
// Trades
//   A                               B
//   REQUEST (A's Pokemon)   ->      sees it, picks one of theirs
//                           <-      OFFER (B's Pokemon)
//   CONFIRM                 <->     CONFIRM
//   each one copies its Pokemon (checked unchanged) and sends DATA; with both, each game
//   runs the trade (animation, evolution) on its own party slot
// ---------------------------------------------------------------------------

static void PutString(std::vector<u8>& v, const QString& s)
{
    QByteArray utf8 = s.toUtf8().left(60);
    v.push_back((u8)utf8.size());
    v.insert(v.end(), utf8.begin(), utf8.end());
}

static std::vector<u8> TradeMonBytes(const GameUi::GiftMon& m)
{
    std::vector<u8> v;
    PutU16(v, m.species);
    v.push_back((u8)m.level);
    v.push_back(m.egg);
    v.push_back(m.shiny);
    v.push_back((u8)m.gender);
    PutU16(v, m.heldItem);
    PutU32(v, m.personality);
    for (int s = 0; s < 6; s++)
        PutU16(v, m.stats[s]);
    PutString(v, m.name);
    PutString(v, m.itemName);
    PutString(v, m.ability);
    v.push_back((u8)std::min<int>(m.moves.size(), 4));
    for (int i = 0; i < std::min<int>(m.moves.size(), 4); i++)
        PutString(v, m.moves[i]);
    return v;
}

static bool TradeMonFrom(const u8* p, size_t size, GameUi::GiftMon& m)
{
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= size; };
    auto str = [&](QString& out) {
        if (!need(1))
            return false;
        size_t len = p[pos++];
        if (!need(len))
            return false;
        out = QString::fromUtf8((const char*)p + pos, (int)len);
        pos += len;
        return true;
    };
    if (!need(24))
        return false;
    m.species = p[0] | (p[1] << 8);
    m.level = p[2];
    m.egg = p[3] != 0;
    m.shiny = p[4] != 0;
    m.gender = p[5];
    m.heldItem = p[6] | (p[7] << 8);
    m.personality = ReadU32(p + 8);
    for (int s = 0; s < 6; s++)
        m.stats[s] = p[12 + s * 2] | (p[13 + s * 2] << 8);
    pos = 24;
    if (!str(m.name) || !str(m.itemName) || !str(m.ability) || !need(1))
        return false;
    int moves = std::min<int>(p[pos++], 4);
    m.moves.clear();
    for (int i = 0; i < moves; i++)
    {
        QString move;
        if (!str(move))
            return false;
        m.moves.append(move);
    }
    return true;
}

void CoopLink::tradeSend(int target, u8 op, const std::vector<u8>& extra)
{
    if (!peer || mySlot < 0)
        return;
    std::vector<u8> packet { kPacketTrade, (u8)target, op };
    packet.insert(packet.end(), extra.begin(), extra.end());
    enet_peer_send(peer, 1, enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE));
}

// What the player did in the trade screen
void CoopLink::tradeUpdate()
{
    GameUi& ui = GameUi::Get();
    GameUi::TradeAction action;
    while (ui.TakeTradeAction(action))
    {
        switch (action.kind)
        {
        case GameUi::TradeAction::Request:
            if (trade.active)
                break;
            trade = TradeSession();
            trade.active = true;
            trade.slot = action.target;
            trade.initiator = true;
            trade.myIndex = action.index;
            trade.myPersonality = action.mon.personality;
            tradeSend(trade.slot, kTradeRequest, TradeMonBytes(action.mon));
            break;
        case GameUi::TradeAction::Offer:
            if (!trade.active || trade.initiator)
                break;
            trade.myIndex = action.index;
            trade.myPersonality = action.mon.personality;
            tradeSend(trade.slot, kTradeOffer, TradeMonBytes(action.mon));
            break;
        case GameUi::TradeAction::Confirm:
            if (!trade.active || trade.myIndex < 0)
                break;
            trade.myConfirm = true;
            tradeSend(trade.slot, kTradeConfirm);
            tradeTry();
            break;
        case GameUi::TradeAction::Cancel:
            if (trade.active && !trade.executing)
            {
                tradeSend(trade.slot, kTradeCancel, { 0 });
                trade = TradeSession();
            }
            else if (!trade.active && action.target >= 0)
                tradeSend(action.target, kTradeCancel, { 0 }); // a request refused
            break;
        }
    }
    // The friend left
    if (trade.active && !trade.executing && (trade.slot < 0 || !players[trade.slot].present))
    {
        trade = TradeSession();
        ui.TradeCancelled(QString::fromUtf8("L'échange est annulé : ton ami s'est déconnecté"));
    }
}

void CoopLink::tradeTry()
{
    if (!trade.active || !trade.myConfirm || !trade.theirConfirm || trade.executing)
        return;
    if (!trade.peeked && !trade.peeking)
    {
        // Our Pokemon, as it is now (it must still be the one shown)
        trade.peeking = true;
        giftQueue(kGiftPeekMon, trade.myIndex, 0, [this](bool ok, const u8* gift) {
            trade.peeking = false;
            if (!trade.active)
                return;
            if (!ok || ReadU32(gift + kGiftMon) != trade.myPersonality)
            {
                tradeSend(trade.slot, kTradeCancel, { 1 });
                trade = TradeSession();
                GameUi::Get().TradeCancelled(QString::fromUtf8("L'échange est annulé : ton équipe a changé"));
                return;
            }
            trade.peeked = true;
            std::vector<u8> data(gift + kGiftMon, gift + kGiftMon + kMonSize);
            tradeSend(trade.slot, kTradeData, data);
            tradeTry();
        });
        return;
    }
    if (!trade.peeked || trade.theirData.size() != (size_t)kMonSize)
        return;
    trade.executing = true;
    QString partner = playerName(trade.slot);
    GameUi::Get().TradeExecuting();
    std::vector<u16> name(8, 0xFFFF);
    EncodeGameText(partner.left(7), name.data(), 7);
    GiftCommandJob job;
    job.command = kGiftTradeMon;
    job.arg = trade.myIndex;
    job.mon = trade.theirData;
    job.partnerName = name;
    job.done = [this, partner](bool ok, const u8*) {
        trade = TradeSession();
        GameUi::Get().NuzlockeMessage(ok ? QString::fromUtf8("Échange réussi avec %1 !").arg(partner)
                                         : QString::fromUtf8("L'échange n'a pas pu se faire"), ok ? QColor(76, 176, 120) : QColor(239, 106, 90));
    };
    giftJobs.push_back(std::move(job));
}

void CoopLink::tradePacket(int slot, const u8* payload, size_t size)
{
    u8 op = payload[1];
    const u8* rest = payload + 2;
    size_t restSize = size - 2;
    GameUi& ui = GameUi::Get();
    QString from = playerName(slot);
    switch (op)
    {
    case kTradeRequest:
    {
        GameUi::GiftMon theirs;
        if (!TradeMonFrom(rest, restSize, theirs))
            return;
        if (trade.active)
        {
            tradeSend(slot, kTradeCancel, { 2 }); // busy with another trade
            return;
        }
        trade = TradeSession();
        trade.active = true;
        trade.slot = slot;
        trade.initiator = false;
        ui.TradeRequested(slot, from, theirs);
        return;
    }
    case kTradeOffer:
    {
        GameUi::GiftMon theirs;
        if (!trade.active || trade.slot != slot || !trade.initiator || !TradeMonFrom(rest, restSize, theirs))
            return;
        ui.TradeOffered(theirs);
        return;
    }
    case kTradeConfirm:
        if (!trade.active || trade.slot != slot)
            return;
        trade.theirConfirm = true;
        ui.TradeTheyConfirmed();
        tradeTry();
        return;
    case kTradeData:
        if (!trade.active || trade.slot != slot || restSize != (size_t)kMonSize)
            return;
        trade.theirData.assign(rest, rest + kMonSize);
        tradeTry();
        return;
    case kTradeCancel:
    {
        int reason = restSize >= 1 ? rest[0] : 0;
        if (trade.active && trade.slot == slot && !trade.executing)
            trade = TradeSession();
        ui.TradeCancelled(reason == 2 ? QString::fromUtf8("%1 est déjà en plein échange").arg(from)
                          : reason == 1 ? QString::fromUtf8("L'échange est annulé : l'équipe de %1 a changé").arg(from)
                                        : QString::fromUtf8("%1 a annulé l'échange").arg(from));
        return;
    }
    default:
        return;
    }
}

void CoopLink::Update(NDS* nds)
{
    // Chat messages typed by the player
    {
        int target;
        QString text;
        while (GameUi::Get().TakeChatOut(target, text))
        {
            if (!peer || mySlot < 0)
                continue;
            QByteArray utf8 = text.toUtf8().left(360);
            std::vector<u8> packet(2 + utf8.size());
            packet[0] = kPacketChat;
            packet[1] = target < 0 ? 0xFF : (u8)target;
            memcpy(packet.data() + 2, utf8.constData(), utf8.size());
            enet_peer_send(peer, 1, enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE));
        }
    }
    // The mailbox is also used without the network (HD menus), so it is always looked for
    if (enabled)
    {
        serviceNetwork();
        for (Player& p : players)
            if (p.framesSinceState < kTimeoutFrames)
                p.framesSinceState++;
    }

    if (!mailboxValid(nds))
    {
        mailbox = 0;
        if (--scanCooldown > 0)
            return;
        scanCooldown = 60;
        mailbox = findMailbox(nds);
        if (!mailbox)
            return;
        Log(LogLevel::Info, "[coop] mailbox found at %08X\n", 0x02000000 + mailbox);
        giftSynced = false;
        // Don't resend movements made before we found the mailbox
        lastSentEvent = ReadU32(nds->MainRAM + mailbox + kOffLocalEventCount);
        for (Player& p : players)
            p.events.clear();
    }

    u8* box = nds->MainRAM + mailbox;
    if (haveLocalName)
        memcpy(box + kOffNames, localNameCode, sizeof(localNameCode)); // little-endian host, like the DS
    WriteU32(box + kOffNuzlocke, seed);
    box[kOffNuzlocke + 4] = rules;
    if (rules & 1)
        nuzlockeSync(box);
    giftUpdate();
    tradeUpdate();
    giftRun(box);
    freezeWatch(nds, box);

    if (!enabled)
        return;

    bool online = peer && peer->state == ENET_PEER_STATE_CONNECTED && mySlot >= 0;
    frameCounter++;

    // Movement actions: sent reliably and in order, so the ghosts replay every step
    u32 localEvents = ReadU32(box + kOffLocalEventCount);
    if (!online || localEvents - lastSentEvent > (u32)kEventCount)
        lastSentEvent = online ? localEvents - kEventCount : localEvents;
    for (; lastSentEvent != localEvents; lastSentEvent++)
    {
        u8 data[1 + kEventSize];
        data[0] = kPacketMoveEvent;
        memcpy(data + 1, box + kOffLocalEvents + (lastSentEvent % kEventCount) * kEventSize, kEventSize);
        if (debugLog)
            fprintf(debugLog, "%u event cmd=%u x=%u z=%u\n", frameCounter, data[1] | (data[2] << 8), data[5] | (data[6] << 8), data[7] | (data[8] << 8));
        enet_peer_send(peer, 1, enet_packet_create(data, sizeof(data), ENET_PACKET_FLAG_RELIABLE));
    }

    u32 frame = ReadU32(box + kOffLocalFrame);
    if (online && frame != lastSentFrame)
    {
        u8 data[1 + kStateSize];
        data[0] = kPacketPlayerState;
        memcpy(data + 1, box + kOffLocal, kStateSize);
        enet_peer_send(peer, 0, enet_packet_create(data, sizeof(data), ENET_PACKET_FLAG_UNSEQUENCED));
        lastSentFrame = frame;
    }

    // Our following Pokemon (on change and now and then) and our game's info (place, team...)
    const u8* follower = box + kOffFollowers;
    if (online && (--followerCooldown <= 0 || memcmp(follower, lastSentFollower, kFollowerSize) != 0))
    {
        u8 data[1 + kFollowerSize];
        data[0] = kPacketFollower;
        memcpy(data + 1, follower, kFollowerSize);
        enet_peer_send(peer, 1, enet_packet_create(data, sizeof(data), ENET_PACKET_FLAG_RELIABLE));
        memcpy(lastSentFollower, follower, kFollowerSize);
        followerCooldown = 60;
    }
    // Our look, now and then (a friend who just came gets it soon); theirs into the game
    if (online && --lookCooldown <= 0)
    {
        QByteArray look = GameUi::Get().LookString().toUtf8();
        std::vector<u8> packet { kPacketLook, 0xFF };
        packet.insert(packet.end(), look.begin(), look.end());
        enet_peer_send(peer, 1, enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE));
        lookCooldown = 120;
    }
    if (frameCounter % 30 == 0)
    {
        for (int s = 0; s < kMaxSlots; s++)
        {
            if (s == mySlot || mySlot < 0 || !players[s].present)
                continue;
            int r = remoteIndex(s);
            if (r < 0 || r >= kMaxRemotes)
                continue;
            u8* g = box + kOffGhostLooks + r * kGhostLookSize;
            auto pairs = GameUi::Get().LookPairsFor(players[s].look);
            g[0] = (u8)(players[s].lookSerial + s * 37); // differs per player too: a new friend in this slot
            GameUi::WriteLookPairs(g, pairs);
        }
    }
    if (online && --infoCooldown <= 0 && box[kOffInfo + 65]) // CoopInfo.valid
    {
        u8 data[1 + kInfoSize];
        data[0] = kPacketInfo;
        memcpy(data + 1, box + kOffInfo, kInfoSize);
        enet_peer_send(peer, 1, enet_packet_create(data, sizeof(data), ENET_PACKET_FLAG_RELIABLE));
        infoCooldown = 90;
    }
    if (peer)
        enet_host_flush(host);

    // The other players, into the mailbox
    int others = 0;
    for (int s = 0; s < kMaxSlots; s++)
    {
        if (s == mySlot || mySlot < 0)
            continue;
        int index = remoteIndex(s);
        if (index < 0 || index >= kMaxRemotes)
            continue;
        Player& p = players[s];
        u8* remote = box + kOffRemotes + index * kRemoteSize;
        bool active = p.present && p.haveState && p.framesSinceState < kTimeoutFrames;
        others += p.present;
        if (!p.present)
        {
            remote[kRemActive] = 0;
            memset(remote + kRemFollower, 0, kFollowerSize);
            continue;
        }
        if (p.haveState)
            memcpy(remote + kRemState, p.state, kStateSize);
        u32 count = ReadU32(remote + kRemEventCount);
        for (size_t i = 0; i + kEventSize <= p.events.size(); i += kEventSize)
        {
            memcpy(remote + kRemEvents + (count % kEventCount) * kEventSize, &p.events[i], kEventSize);
            count++;
        }
        p.events.clear();
        WriteU32(remote + kRemEventCount, count);
        memcpy(remote + kRemFollower, p.follower, kFollowerSize);
        remote[kRemActive] = active ? 1 : 0;
    }
    if (mySlot < 0)
        for (int i = 0; i < kMaxRemotes; i++)
            box[kOffRemotes + i * kRemoteSize + kRemActive] = 0;

    if (!rejected)
        GameUi::Get().SetNetworkStatus(!online ? 0 : others > 0 ? 2 : 1);
}
