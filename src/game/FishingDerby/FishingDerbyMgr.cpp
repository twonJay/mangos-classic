/*
 * Stranglethorn Fishing Extravaganza derby - see FishingDerbyMgr.h for the rationale.
 */

#include "FishingDerby/FishingDerbyMgr.h"

#include "Chat/Chat.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "GameEvents/GameEventMgr.h"
#include "Log/Log.h"
#include "Maps/GridMap.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "Maps/SpawnManager.h"
#include "Util/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

INSTANTIATE_SINGLETON_1(FishingDerbyMgr);

namespace
{
    const uint16 EVENT_FISHING          = 15;       // Sun 2-4pm: pools up, Tastyfish catchable
    const uint16 EVENT_TURNIN           = 34;       // Sun 2-5pm: Riggle takes turn-ins
    const uint32 NPC_RIGGLE_BASSBAIT    = 15077;
    const uint32 NPC_RIVAL_ANGLER       = 190000;
    const uint32 GO_SCHOOL_OF_TASTYFISH = 180248;
    const uint32 ZONE_STRANGLETHORN     = 33;
    const uint32 ZONE_BOOTY_BAY         = 35;
    const uint32 CATCHES_TO_WIN         = 40;

    const char* RIVAL_NAMES[] =
    {
        "Zeke Wobblegear", "Marlin Saltbeard", "Tilly Sprocketwrench", "Old Bessa",
        "Jom'bo", "Fizzle Brasspole", "Durgan Wake", "Sallie Hook",
        "Nix Quickline", "Tarl Redd", "Mokka", "Wixxle Greaseline",
        "Harlan Doyle", "Zin'jala", "Petra Grange", "Skeezle Bentbobber",
    };
    const size_t RIVAL_NAME_COUNT = sizeof(RIVAL_NAMES) / sizeof(RIVAL_NAMES[0]);
}

void FishingDerbyMgr::Initialize()
{
    m_enabled     = sConfig.GetBoolDefault("FishingDerby.Enable", true);
    m_rivalCount  = sConfig.GetIntDefault("FishingDerby.Rivals", 6);
    m_paceMinSecs = sConfig.GetIntDefault("FishingDerby.WinnerPaceMinMinutes", 18) * 60;
    m_paceMaxSecs = sConfig.GetIntDefault("FishingDerby.WinnerPaceMaxMinutes", 30) * 60;
    m_chatterSecs = sConfig.GetIntDefault("FishingDerby.ChatterIntervalSeconds", 240);
    m_anglerCount = sConfig.GetIntDefault("FishingDerby.Anglers", 10);
    m_anglerCatchMinSecs = sConfig.GetIntDefault("FishingDerby.AnglerCatchMinSeconds", 15);
    m_anglerCatchMaxSecs = sConfig.GetIntDefault("FishingDerby.AnglerCatchMaxSeconds", 30);
    m_minActivePools = sConfig.GetIntDefault("FishingDerby.MinActivePools", 25);
    m_drainRate   = sConfig.GetFloatDefault("FishingDerby.PoolDrainRate", 0.35f);

    if (!m_enabled)
    {
        sLog.outString("FishingDerbyMgr: disabled (FishingDerby.Enable = 0).");
        return;
    }

    m_rivalCount  = std::max(1u, std::min(m_rivalCount, uint32(RIVAL_NAME_COUNT)));
    m_paceMinSecs = std::max(120u, m_paceMinSecs);
    m_paceMaxSecs = std::max(m_paceMinSecs + 60, m_paceMaxSecs);
    m_chatterSecs = std::max(30u, m_chatterSecs);
    m_anglerCatchMinSecs = std::max(5u, m_anglerCatchMinSecs);
    m_anglerCatchMaxSecs = std::max(m_anglerCatchMinSecs + 1, m_anglerCatchMaxSecs);
    m_drainRate   = std::max(0.f, std::min(m_drainRate, 3.f));

    if (auto result = WorldDatabase.PQuery(
            "SELECT guid, position_x, position_y FROM gameobject WHERE id = %u", GO_SCHOOL_OF_TASTYFISH))
    {
        do
        {
            Field* f = result->Fetch();
            PoolSpot spot;
            spot.dbGuid = f[0].GetUInt32();
            spot.x      = f[1].GetFloat();
            spot.y      = f[2].GetFloat();
            m_pools.push_back(spot);
        } while (result->NextRow());
    }

    if (auto result = WorldDatabase.PQuery(
            "SELECT guid FROM creature WHERE id = %u LIMIT 1", NPC_RIGGLE_BASSBAIT))
        m_riggleDbGuid = result->Fetch()[0].GetUInt32();

    sLog.outString("FishingDerbyMgr: %u rivals, winner pace %u-%u min, %zu Tastyfish pools, %u shore anglers.",
                   m_rivalCount, m_paceMinSecs / 60, m_paceMaxSecs / 60, m_pools.size(), m_anglerCount);
}

Map* FishingDerbyMgr::GetContinent() const
{
    return sMapMgr.FindMap(0, 0);   // Eastern Kingdoms
}

void FishingDerbyMgr::Update(uint32 diff)
{
    if (!m_enabled)
        return;

    m_sinceTickMs += diff;
    if (m_sinceTickMs < 5000)
        return;
    m_sinceTickMs = 0;

    const bool fishing = sGameEventMgr.IsActiveEvent(EVENT_FISHING);
    const bool turnin  = sGameEventMgr.IsActiveEvent(EVENT_TURNIN);
    const time_t now = time(nullptr);

    // Contest fully over (both windows closed): forget this week's winner.
    if (!fishing && !turnin)
    {
        if (m_racing || m_winnerDeclared)
            ResetAll();
        return;
    }

    // Fishing window opened (also covers a mid-event server restart).
    if (fishing && !m_racing && !m_winnerDeclared)
        StartRace(now);

    if (m_racing)
    {
        // 4 o'clock: the schools despawn with event 15; the shore empties out.
        // Rivals who already have their catch are still racing to Riggle, so
        // the win check keeps running until the turn-in window closes.
        if (!fishing && !m_anglers.empty())
            DespawnAnglers();

        TickRace(now);
    }
}

void FishingDerbyMgr::StartRace(time_t now)
{
    m_rivals.clear();

    // Deal distinct names by walking the list from a random start.
    uint32 nameIdx = urand(0, RIVAL_NAME_COUNT - 1);
    for (uint32 i = 0; i < m_rivalCount; ++i)
    {
        Rival rival;
        rival.name = RIVAL_NAMES[(nameIdx + i) % RIVAL_NAME_COUNT];
        rival.finishSecs = urand(m_paceMinSecs, m_paceMaxSecs);
        if (i > 0)  // everyone but the pace-setter drags further behind
            rival.finishSecs += urand(0, m_paceMaxSecs / 2);
        rival.curve = 0.85f + rand_norm_f() * 0.35f;    // neutral pack pacing
        rival.plateau = CATCHES_TO_WIN - 1;
        m_rivals.push_back(std::move(rival));
    }

    // The hidden finish times already decide the winner; shape the announced
    // race so the lead visibly changes hands every week: the eventual winner
    // starts cold and surges late, while one designated hare starts hot and
    // stalls in the low 30s. Purely random curves left ~40% of races with
    // every announcement naming the same rival.
    m_winnerIdx = 0;
    for (uint32 i = 1; i < m_rivals.size(); ++i)
        if (m_rivals[i].finishSecs < m_rivals[m_winnerIdx].finishSecs)
            m_winnerIdx = i;
    m_rivals[m_winnerIdx].curve = 1.2f + rand_norm_f() * 0.3f;
    if (m_rivals.size() > 1)
    {
        uint32 hareIdx = urand(0, m_rivals.size() - 2);
        if (hareIdx >= m_winnerIdx)
            ++hareIdx;
        m_rivals[hareIdx].curve = 0.6f + rand_norm_f() * 0.15f;
        m_rivals[hareIdx].plateau = urand(26, 32);
    }

    m_surgeCalled = false;
    m_lastAnnouncedName.clear();

    m_racing = true;
    m_winnerDeclared = false;
    m_raceStart = now;
    m_lastChatter = now;
    m_lastDrain = now;
    m_drainCarry = 0.f;

    AnnounceToZones("The Stranglethorn Fishing Extravaganza has begun! The first angler to bring 40 Speckled Tastyfish to Riggle Bassbait in Booty Bay becomes the Master Angler!");

    // Fresh coast at the opening bell: force every school up, wiping any
    // 15-30 min respawn timers left over from before the contest started.
    // The anglers place themselves over the following ticks (SpawnAnglers
    // tops up from TickRace once the schools actually exist).
    if (Map* map = GetContinent())
        for (const PoolSpot& spot : m_pools)
        {
            GameObject* go = map->GetGameObject(spot.dbGuid);
            if (!go || !go->IsSpawned())
                map->GetSpawnManager().RespawnGameObject(spot.dbGuid, 0);
        }

    sLog.outString("FishingDerbyMgr: race started, %u rivals, leader pace %u min.",
                   uint32(m_rivals.size()), m_rivals.empty() ? 0 : m_rivals[0].finishSecs / 60);
}

void FishingDerbyMgr::ResetAll()
{
    DespawnAnglers();
    m_rivals.clear();
    m_racing = false;
    m_winnerDeclared = false;
    m_raceStart = 0;
    m_drainCarry = 0.f;
    m_winnerIdx = 0;
    m_surgeCalled = false;
    m_lastAnnouncedName.clear();
}

void FishingDerbyMgr::TickRace(time_t now)
{
    const uint32 elapsed = uint32(now - m_raceStart);

    // Did a rival land fish #40?
    for (Rival& rival : m_rivals)
    {
        if (elapsed >= rival.finishSecs)
        {
            DeclareWinner(rival.name);
            return;
        }
    }

    // Standings chatter: always the current leader. The curve/plateau shaping
    // from StartRace guarantees the announced lead changes hands (hare leads
    // early, stalls; winner surges late), and the scripted surge call below
    // guarantees the eventual winner is heard from before the declaration.
    // The hidden finish time still decides the real winner.
    if (now - m_lastChatter >= time_t(m_chatterSecs))
    {
        m_lastChatter = now;

        auto computedCatch = [elapsed](const Rival& rival) -> uint32
        {
            const float frac = std::min(1.f, float(elapsed) / float(rival.finishSecs));
            uint32 caught = std::min(CATCHES_TO_WIN - 1,
                                     uint32(float(CATCHES_TO_WIN) * std::pow(frac, rival.curve)));
            caught = std::min(caught, rival.plateau);    // the hare stalls out here
            return std::max(caught, rival.reported);     // never shrink a called-in count
        };

        Rival& winner = m_rivals[m_winnerIdx];
        if (!m_surgeCalled && elapsed + m_chatterSecs + 120 >= winner.finishSecs)
        {
            // scripted finale: with chatter every m_chatterSecs, exactly one
            // tick lands in this pre-finish window, so every race hears the
            // winner closing in before Riggle declares them
            uint32 bestOther = 0;
            for (uint32 i = 0; i < m_rivals.size(); ++i)
                if (i != m_winnerIdx)
                    bestOther = std::max(bestOther, computedCatch(m_rivals[i]));
            const uint32 caught = std::max(computedCatch(winner),
                                           std::min(CATCHES_TO_WIN - 1, bestOther + urand(1, 2)));
            winner.reported = caught;
            m_surgeCalled = true;
            m_lastAnnouncedName = winner.name;
            char buf[256];
            snprintf(buf, sizeof(buf), "Fishing Extravaganza: %s is on a tear - %u Speckled Tastyfish and closing fast!",
                     winner.name.c_str(), caught);
            AnnounceToZones(buf);
        }
        else
        {
            Rival* leader = nullptr;
            uint32 best = 0;
            for (Rival& rival : m_rivals)
            {
                const uint32 caught = computedCatch(rival);
                if (!leader || caught > best)
                {
                    best = caught;
                    leader = &rival;
                }
            }

            if (leader && best >= 3 && best > leader->reported)
            {
                leader->reported = best;
                char buf[256];
                if (!m_lastAnnouncedName.empty() && m_lastAnnouncedName != leader->name)
                    snprintf(buf, sizeof(buf), "Fishing Extravaganza: %s has taken the lead with %u Speckled Tastyfish!",
                             leader->name.c_str(), best);
                else
                    snprintf(buf, sizeof(buf), "Fishing Extravaganza: %s has caught %u Speckled Tastyfish!",
                             leader->name.c_str(), best);
                m_lastAnnouncedName = leader->name;
                AnnounceToZones(buf);
            }
        }
    }

    // Rival catches thin out the real schools while the pools are up.
    if (m_drainRate > 0.f && sGameEventMgr.IsActiveEvent(EVENT_FISHING))
    {
        float ratePerSec = 0.f;
        for (const Rival& rival : m_rivals)
            ratePerSec += float(CATCHES_TO_WIN) / float(rival.finishSecs);
        m_drainCarry += ratePerSec * m_drainRate * float(now - m_lastDrain);
        m_lastDrain = now;

        if (m_drainCarry >= 1.f)
        {
            const uint32 catches = uint32(m_drainCarry);
            m_drainCarry -= float(catches);
            DrainPools(catches);
        }
    }
    else
        m_lastDrain = now;

    // Visible anglers pull real fish out of their own schools, then anyone
    // whose school got fished out wanders to a fresh one.
    if (sGameEventMgr.IsActiveEvent(EVENT_FISHING))
    {
        AnglersFish(now);
        EnforcePoolFloor();     // top up first so relocating anglers have fresh schools
        RelocateIdleAnglers();
        if (m_anglers.size() < m_anglerCount)
            SpawnAnglers();     // initial placement and replacements, once schools are up
    }
}

void FishingDerbyMgr::DeclareWinner(const std::string& name)
{
    m_winnerDeclared = true;
    m_racing = false;
    DespawnAnglers();

    const std::string text = "We have a winner!  " + name + " is the Master Angler!";
    RiggleYell(text);
    AnnounceToZones("Fishing Extravaganza: " + text + " Tastyfish and rare fish turn-ins remain open until 5 o'clock.");

    sLog.outString("FishingDerbyMgr: winner declared - %s.", name.c_str());
}

void FishingDerbyMgr::OnPlayerWin(Player* player)
{
    if (!m_enabled || m_winnerDeclared || !player)
        return;

    DeclareWinner(player->GetName());
}

void FishingDerbyMgr::AnnounceToZones(const std::string& text) const
{
    Map* map = GetContinent();
    if (!map)
        return;

    for (const auto& ref : map->GetPlayers())
    {
        Player* player = ref.getSource();
        if (!player || !player->IsInWorld())
            continue;
        if (!player->isRealPlayer())
            continue;
        const uint32 zone = player->GetZoneId();
        if (zone == ZONE_STRANGLETHORN || zone == ZONE_BOOTY_BAY)
            ChatHandler(player).SendSysMessage(text.c_str());
    }
}

void FishingDerbyMgr::RiggleYell(const std::string& text) const
{
    if (!m_riggleDbGuid)
        return;

    Map* map = GetContinent();
    if (!map)
        return;

    if (Creature* riggle = map->GetCreature(m_riggleDbGuid))
        riggle->MonsterYell(text.c_str(), LANG_UNIVERSAL);
}

bool FishingDerbyMgr::DrainPoolOnce(Map* map, uint32 dbGuid) const
{
    // One catch taken from the school; after its normal 3-5 opens it despawns
    // and comes back on its usual respawn timer, exactly as if fished out.
    GameObject* go = map->GetGameObject(dbGuid);
    if (!go || !go->IsSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE)
        return false;

    go->AddUse();
    if (go->GetUseCount() >= urand(go->GetGOInfo()->fishinghole.minSuccessOpens,
                                   go->GetGOInfo()->fishinghole.maxSuccessOpens))
        go->SetLootState(GO_JUST_DEACTIVATED);
    return true;
}

void FishingDerbyMgr::DrainPools(uint32 catches)
{
    Map* map = GetContinent();
    if (!map || m_pools.empty())
        return;

    // The unseen rest of the field hits random schools along the coast.
    for (uint32 i = 0; i < catches; ++i)
        for (uint32 attempt = 0; attempt < 8; ++attempt)
            if (DrainPoolOnce(map, m_pools[urand(0, m_pools.size() - 1)].dbGuid))
                break;
}

void FishingDerbyMgr::AnglersFish(time_t now)
{
    Map* map = GetContinent();
    if (!map)
        return;

    for (Angler& entry : m_anglers)
    {
        if (now < entry.nextCatch)
            continue;
        entry.nextCatch = now + urand(m_anglerCatchMinSecs, m_anglerCatchMaxSecs);
        DrainPoolOnce(map, m_pools[entry.poolIdx].dbGuid);
    }
}

void FishingDerbyMgr::EnforcePoolFloor()
{
    if (!m_minActivePools)
        return;

    Map* map = GetContinent();
    if (!map)
        return;

    // However hard the field fishes, the coast never empties below the floor:
    // a random fished-out school bubbles straight back up for each one missing.
    // Under the dynguid spawning system a fished-out school's object is
    // destroyed outright and only a pending entry in the map's SpawnManager
    // remains (GetGameObject returns nullptr for it), so early respawns go
    // through the SpawnManager rather than the (dead) object.
    uint32 active = 0;
    std::vector<uint32> dormant;
    for (const PoolSpot& spot : m_pools)
    {
        GameObject* go = map->GetGameObject(spot.dbGuid);
        if (go && go->IsSpawned())
            ++active;
        else
            dormant.push_back(spot.dbGuid);
    }

    while (active < m_minActivePools && !dormant.empty())
    {
        const uint32 idx = urand(0, dormant.size() - 1);
        map->GetSpawnManager().RespawnGameObject(dormant[idx], 0);
        dormant.erase(dormant.begin() + idx);
        ++active;
    }
}

void FishingDerbyMgr::SpawnAnglers()
{
    // Top-up: fills open angler slots at spawned, unworked schools. Called
    // every race tick until the roster is full, so placement simply waits
    // out schools that have not spawned yet.
    if (!m_anglerCount || m_pools.empty())
        return;

    Map* map = GetContinent();
    if (!map)
        return;

    std::vector<uint32> order(m_pools.size());
    for (uint32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::shuffle(order.begin(), order.end(), *GetRandomGenerator());

    for (uint32 poolIdx : order)
    {
        if (m_anglers.size() >= m_anglerCount)
            break;

        GameObject* pool = map->GetGameObject(m_pools[poolIdx].dbGuid);
        if (!pool || !pool->IsSpawned())
            continue;

        bool taken = false;
        for (const Angler& other : m_anglers)
        {
            if (other.poolIdx == poolIdx)
            {
                taken = true;
                break;
            }
        }
        if (taken)
            continue;

        float sx, sy, sz, so;
        if (!FindShorePoint(m_pools[poolIdx], sx, sy, sz, so))
            continue;

        // Active object: keeps its map cells ticking with no player around, so
        // its school's despawn processes on time and relocation isn't deferred
        // until someone rides into view.
        TempSpawnSettings settings(nullptr, NPC_RIVAL_ANGLER, sx, sy, sz, so, TEMPSPAWN_MANUAL_DESPAWN, 0, true);
        if (Creature* angler = WorldObject::SummonCreature(settings, map))
        {
            angler->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_FISHING);
            Angler entry;
            entry.guid = angler->GetObjectGuid();
            entry.poolIdx = poolIdx;
            entry.nextCatch = time(nullptr) + urand(m_anglerCatchMinSecs, m_anglerCatchMaxSecs);
            m_anglers.push_back(entry);
        }
    }
}

void FishingDerbyMgr::DespawnAnglers()
{
    if (m_anglers.empty())
        return;

    if (Map* map = GetContinent())
        for (const Angler& entry : m_anglers)
            if (Creature* angler = map->GetCreature(entry.guid))
                angler->ForcedDespawn();

    m_anglers.clear();
}

void FishingDerbyMgr::RelocateIdleAnglers()
{
    Map* map = GetContinent();
    if (!map)
        return;

    for (Angler& entry : m_anglers)
    {
        GameObject* pool = map->GetGameObject(m_pools[entry.poolIdx].dbGuid);
        if (pool && pool->IsSpawned())
            continue;   // school still up, keep fishing it

        // Pick a fresh spawned school nobody else is working and walk over
        // (teleport - nobody watches the whole coast at once, and pathing
        // 500 yards of shoreline is not worth the cost).
        for (uint32 attempt = 0; attempt < 10; ++attempt)
        {
            const uint32 poolIdx = urand(0, m_pools.size() - 1);

            bool taken = false;
            for (const Angler& other : m_anglers)
            {
                if (&other != &entry && other.poolIdx == poolIdx)
                {
                    taken = true;
                    break;
                }
            }
            if (taken)
                continue;

            GameObject* target = map->GetGameObject(m_pools[poolIdx].dbGuid);
            if (!target || !target->IsSpawned())
                continue;

            float sx, sy, sz, so;
            if (!FindShorePoint(m_pools[poolIdx], sx, sy, sz, so))
                continue;

            if (Creature* angler = map->GetCreature(entry.guid))
            {
                angler->NearTeleportTo(sx, sy, sz, so);
                angler->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_FISHING);
                entry.poolIdx = poolIdx;
                entry.nextCatch = time(nullptr) + urand(m_anglerCatchMinSecs, m_anglerCatchMaxSecs);
            }
            break;
        }
    }
}

bool FishingDerbyMgr::FindShorePoint(const PoolSpot& pool, float& sx, float& sy, float& sz, float& so) const
{
    Map* map = GetContinent();
    if (!map)
        return false;

    const TerrainInfo* terrain = map->GetTerrain();
    if (!terrain)
        return false;

    const float angleOffset = rand_norm_f() * 2.f * M_PI_F;
    for (float radius : { 7.f, 10.f, 14.f, 18.f, 23.f })
    {
        for (uint32 step = 0; step < 12; ++step)
        {
            const float angle = angleOffset + 2.f * M_PI_F * float(step) / 12.f;
            const float gx = pool.x + std::cos(angle) * radius;
            const float gy = pool.y + std::sin(angle) * radius;

            // Schools float on the sea (z ~0); dry land close above the
            // waterline that is not itself flooded makes a fishing spot.
            const float ground = terrain->GetHeightStatic(gx, gy, 5.f, true);
            if (ground < 0.3f || ground > 8.f)
                continue;
            if (terrain->IsSwimmable(gx, gy, ground + 0.5f))
                continue;

            sx = gx;
            sy = gy;
            sz = ground;
            so = MapManager::NormalizeOrientation(std::atan2(pool.y - gy, pool.x - gx));
            return true;
        }
    }
    return false;
}
