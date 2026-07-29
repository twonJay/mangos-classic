/*
 * Gurubashi Arena Brawl manager - see ArenaBrawlMgr.h for the rationale.
 */

#include "ArenaBrawl/ArenaBrawlMgr.h"

#include "Config/Config.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "Util/Util.h"

#ifdef ENABLE_PLAYERBOTS
#include "playerbot/BotState.h"
#include "playerbot/PlayerbotAI.h"
#include "playerbot/RandomPlayerbotMgr.h"
#endif

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

INSTANTIATE_SINGLETON_1(ArenaBrawlMgr);

namespace
{
    const float ARENA_PI = 3.14159265358979f;
}

void ArenaBrawlMgr::Initialize()
{
    m_enabled           = sConfig.GetBoolDefault("ArenaBrawl.Enable", true);
    m_botCount          = sConfig.GetIntDefault("ArenaBrawl.BotCount", 10);
    m_botLevel          = sConfig.GetIntDefault("ArenaBrawl.BotLevel", 60);
    m_triggerRadius     = sConfig.GetFloatDefault("ArenaBrawl.TriggerRadius", 20.f);
    m_arenaRadius       = sConfig.GetFloatDefault("ArenaBrawl.ArenaRadius", 80.f);
    m_scatterRadius     = sConfig.GetFloatDefault("ArenaBrawl.ScatterRadius", 18.f);
    m_graceSecs         = sConfig.GetIntDefault("ArenaBrawl.NoPlayerGraceSeconds", 5);
    m_maxEventSecs      = sConfig.GetIntDefault("ArenaBrawl.MaxEventSeconds", 900);
    m_despawnOnAbandon  = sConfig.GetBoolDefault("ArenaBrawl.DespawnChestOnAbandon", true);
    m_factionMix        = sConfig.GetBoolDefault("ArenaBrawl.FactionMix", true);

    // Ramp barriers: semicolon-separated list of "entry:x:y:z:o" pieces.
    ParseBarrierConfig(sConfig.GetStringDefault("ArenaBrawl.Barriers",
        "180497:-13244.337:257.708:21.857:1.895;"
        "180497:-13194.510:231.316:21.857:0.261"));

    if (m_botCount > 40)     m_botCount = 40;
    if (m_triggerRadius < 1.f) m_triggerRadius = 1.f;
    if (m_arenaRadius < m_triggerRadius) m_arenaRadius = m_triggerRadius;

    if (!m_enabled)
    {
        sLog.outString("ArenaBrawlMgr: disabled (ArenaBrawl.Enable = 0).");
        return;
    }

    sLog.outString("ArenaBrawlMgr: enabled (%u bots, level %u, trigger %.0fy, grace %us, timeout %us, %zu barriers).",
                   m_botCount, m_botLevel, m_triggerRadius, m_graceSecs, m_maxEventSecs, m_barrierDefs.size());
}

void ArenaBrawlMgr::ParseBarrierConfig(const std::string& cfg)
{
    m_barrierDefs.clear();

    size_t pos = 0;
    while (pos < cfg.size())
    {
        size_t end = cfg.find(';', pos);
        std::string token = cfg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? cfg.size() : end + 1;

        if (token.empty())
            continue;

        BarrierDef d;
        d.scale = 1.f;                                          // optional 6th field; default keeps template size
        d.pitch = 0.f;                                          // optional 7th field (radians about Y; PI = upside down)
        d.roll  = 0.f;                                          // optional 8th field (radians about X; PI = upside down)
        if (sscanf(token.c_str(), "%u:%f:%f:%f:%f:%f:%f:%f", &d.entry, &d.x, &d.y, &d.z, &d.o, &d.scale, &d.pitch, &d.roll) >= 5 && d.entry)
            m_barrierDefs.push_back(d);
        else
            sLog.outError("ArenaBrawlMgr: bad barrier config token '%s' (want entry:x:y:z:o[:scale[:pitch[:roll]]]).", token.c_str());
    }
}

Map* ArenaBrawlMgr::GetArenaMap() const
{
    return sMapMgr.FindMap(m_mapId, 0);
}

GameObject* ArenaBrawlMgr::GetChest() const
{
    if (m_chestGuid.IsEmpty())
        return nullptr;
    Map* map = GetArenaMap();
    return map ? map->GetGameObject(m_chestGuid) : nullptr;
}

int ArenaBrawlMgr::CountRealPlayersNear(float radius) const
{
    Map* map = GetArenaMap();
    if (!map)
        return 0;

    int count = 0;
    for (const auto& ref : map->GetPlayers())
    {
        Player* p = ref.getSource();
        if (!p || !p->IsInWorld())
            continue;
        if (p->GetPlayerbotAI())            // bots are not "real" players
            continue;
        if (!p->IsAlive())                  // a corpse/ghost does not hold the chest
            continue;
        if (p->GetDistance2d(m_chestX, m_chestY) <= radius)
            ++count;
    }
    return count;
}

bool ArenaBrawlMgr::AnyRealPlayerWithin(float radius) const
{
    return CountRealPlayersNear(radius) > 0;
}

void ArenaBrawlMgr::ReserveBot(ObjectGuid guid, uint32 seconds)
{
#ifdef ENABLE_PLAYERBOTS
    const uint32 low = guid.GetCounter();
    // Push out the two scheduled actions that would otherwise yank an idle bot
    // out of the brawl: the grind re-teleport and the strategy reset. (These
    // are the only auto-management actions that can fire while a bot is idle;
    // randomize/logout use long timers and are extremely unlikely to elapse
    // inside a short brawl.)
    sRandomPlayerbotMgr.ScheduleTeleport(low, seconds);
    sRandomPlayerbotMgr.ScheduleChangeStrategy(low, seconds);
#endif
}

void ArenaBrawlMgr::ReleaseBot(const BrawlBot& b)
{
#ifdef ENABLE_PLAYERBOTS
    if (Player* bot = ObjectAccessor::FindPlayer(b.guid))
    {
        if (bot->IsInWorld())
        {
            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
            }
            if (PlayerbotAI* ai = bot->GetPlayerbotAI())
            {
                ai->ChangeStrategy("-pvp", BotState::BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("-pvp", BotState::BOT_STATE_COMBAT);
            }
            bot->CombatStop(true);
            bot->TeleportTo(b.originMap, b.originX, b.originY, b.originZ, b.originO);
        }
    }

    // Release the reservation: let RandomPlayerbotMgr send the bot back to a
    // level-appropriate grind spot and re-roll its strategy again shortly.
    const uint32 low = b.guid.GetCounter();
    sRandomPlayerbotMgr.ScheduleTeleport(low, 5);
    sRandomPlayerbotMgr.ScheduleChangeStrategy(low, 5);
#endif
}

void ArenaBrawlMgr::SpawnBarriers()
{
    DespawnBarriers();                 // never double-spawn

    Map* map = GetArenaMap();
    if (!map)
        return;

    for (const BarrierDef& d : m_barrierDefs)
    {
        GameObject* go = new GameObject;
        const uint32 lowGuid = map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT);

        // Build a quaternion from yaw (o), pitch (about Y) and roll (about X) so a
        // barrier can be flipped upside down (pitch or roll = PI) - e.g. a portcullis
        // buried in the ground with only its spikes protruding. Standard ZYX Euler.
        const float cy = cosf(d.o * 0.5f), sy = sinf(d.o * 0.5f);
        const float cp = cosf(d.pitch * 0.5f), sp = sinf(d.pitch * 0.5f);
        const float cr = cosf(d.roll * 0.5f), sr = sinf(d.roll * 0.5f);
        const float rot0 = sr * cp * cy - cr * sp * sy;   // x
        const float rot1 = cr * sp * cy + sr * cp * sy;   // y
        const float rot2 = cr * cp * sy - sr * sp * cy;   // z
        const float rot3 = cr * cp * cy + sr * sp * sy;   // w

        // GO_STATE_READY = closed/solid, which gives the barrier its collision.
        if (!go->Create(lowGuid, lowGuid, d.entry, map, d.x, d.y, d.z, d.o, rot0, rot1, rot2, rot3))
        {
            delete go;
            sLog.outError("ArenaBrawlMgr: failed to create barrier entry %u.", d.entry);
            continue;
        }
        // NOTE: do NOT call SetRespawnTime here. It sets m_respawnDelay!=0 with
        // m_spawnedByDefault=true, which makes GameObject::IsSpawned() return false,
        // hiding the barrier from every non-GM player. We Delete() barriers in
        // Cleanup() ourselves, so no respawn timer is needed.
        if (d.scale > 0.f)
            go->SetObjectScale(d.scale);            // shrink oversized models before the spawn packet goes out
        map->Add(go);
        go->AIM_Initialize();
        // Force the CLOSED state (visible + solid). Doors with startOpen=1 (e.g. the
        // Zul'Gurub forcefield 180497) otherwise spawn open/invisible; READY seals them.
        go->SetGoState(GO_STATE_READY);
        m_barrierGuids.push_back(go->GetObjectGuid());
    }

    if (!m_barrierGuids.empty())
        sLog.outString("ArenaBrawlMgr: sealed the arena with %zu barriers.", m_barrierGuids.size());
}

void ArenaBrawlMgr::DespawnBarriers()
{
    if (m_barrierGuids.empty())
        return;

    Map* map = GetArenaMap();
    if (map)
    {
        for (const ObjectGuid& guid : m_barrierGuids)
            if (GameObject* go = map->GetGameObject(guid))
                go->Delete();
    }
    m_barrierGuids.clear();
}

void ArenaBrawlMgr::Deploy()
{
    m_bots.clear();

    SpawnBarriers();                   // seal the ramps as the fight begins

#ifdef ENABLE_PLAYERBOTS
    // Recruit eligible free bots, kept in two faction buckets so we can
    // interleave for a mixed brawl. Same-map (arena continent) bots first so
    // most teleports are cheap.
    std::vector<Player*> alliance;
    std::vector<Player*> horde;
    size_t freeBots = 0;

    // Enumerate every online player (authoritative list) and keep the free
    // random bots that are eligible. This does not depend on RandomPlayerbotMgr's
    // internal roster being populated by whichever login path was used.
    for (auto& itr : sObjectAccessor.GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld())
            continue;
        if (!sRandomPlayerbotMgr.IsFreeBot(bot))            // random/free bots only (excludes humans)
            continue;
        ++freeBots;
        if (!bot->IsAlive())
            continue;
        if (bot->GetLevel() != m_botLevel)
            continue;
        if (bot->GetMapId() != 0 && bot->GetMapId() != 1)   // continents only
            continue;
        if (bot->IsInCombat() || bot->InBattleGround())
            continue;
        if (bot->GetGroup())                                 // don't disband parties
            continue;

        std::vector<Player*>& bucket = (bot->GetTeam() == ALLIANCE) ? alliance : horde;
        if (bot->GetMapId() == m_mapId)
            bucket.insert(bucket.begin(), bot);              // prefer same-map
        else
            bucket.push_back(bot);
    }

    sLog.outString("ArenaBrawlMgr: recruit scan - %zu free bots online, eligible A:%zu H:%zu.",
                   freeBots, alliance.size(), horde.size());

    std::vector<Player*> picked;
    size_t ai = 0, hi = 0;
    bool takeAlliance = true;
    while (picked.size() < m_botCount && (ai < alliance.size() || hi < horde.size()))
    {
        if (m_factionMix)
        {
            if (takeAlliance && ai < alliance.size())
                picked.push_back(alliance[ai++]);
            else if (!takeAlliance && hi < horde.size())
                picked.push_back(horde[hi++]);
            else if (ai < alliance.size())
                picked.push_back(alliance[ai++]);
            else if (hi < horde.size())
                picked.push_back(horde[hi++]);
            takeAlliance = !takeAlliance;
        }
        else
        {
            if (ai < alliance.size())      picked.push_back(alliance[ai++]);
            else if (hi < horde.size())    picked.push_back(horde[hi++]);
        }
    }

    const uint32 reserveSecs = m_maxEventSecs + 120;
    uint32 i = 0;
    for (Player* bot : picked)
    {
        BrawlBot b;
        b.guid      = bot->GetObjectGuid();
        b.originMap = bot->GetMapId();
        b.originX   = bot->GetPositionX();
        b.originY   = bot->GetPositionY();
        b.originZ   = bot->GetPositionZ();
        b.originO   = bot->GetOrientation();

        ReserveBot(b.guid, reserveSecs);

        // scatter around the chest on a ring so they converge instead of
        // instantly gibbing whoever is on the chest
        const float angle = (2.f * ARENA_PI * i) / (picked.empty() ? 1 : picked.size());
        const float x = m_chestX + m_scatterRadius * cos(angle);
        const float y = m_chestY + m_scatterRadius * sin(angle);
        bot->TeleportTo(m_mapId, x, y, m_chestZ, angle + ARENA_PI);   // face the centre

        bot->SetPvPFreeForAll(true);        // area update reasserts this on arrival too
        if (PlayerbotAI* ai2 = bot->GetPlayerbotAI())
        {
            ai2->ChangeStrategy("+pvp", BotState::BOT_STATE_NON_COMBAT);
            ai2->ChangeStrategy("+pvp", BotState::BOT_STATE_COMBAT);
        }

        m_bots.push_back(b);
        ++i;
    }
#endif

    m_state = STATE_ACTIVE;
    m_eventStart = time(nullptr);
    sLog.outString("ArenaBrawlMgr: brawl started - deployed %zu bots to the arena.", m_bots.size());
}

void ArenaBrawlMgr::Cleanup(bool despawnChest)
{
    for (const BrawlBot& b : m_bots)
        ReleaseBot(b);
    m_bots.clear();

    DespawnBarriers();                 // open the ramps again

    if (despawnChest)
    {
        if (GameObject* chest = GetChest())
            chest->Delete();
    }

    m_state = STATE_IDLE;
    m_chestGuid.Clear();
    m_chestX = m_chestY = m_chestZ = 0.f;
    m_eventStart = 0;
    m_graceUntil = 0;
    sLog.outString("ArenaBrawlMgr: brawl ended (chest %s).", despawnChest ? "despawned" : "resolved");
}

void ArenaBrawlMgr::OnChestSpawned(GameObject* chest)
{
    if (!m_enabled || !chest)
        return;

    // one arena at a time; tidy any stale brawl before arming a new chest
    if (m_state != STATE_IDLE)
        Cleanup(false);

    m_mapId     = chest->GetMapId();
    m_chestGuid = chest->GetObjectGuid();
    m_chestX    = chest->GetPositionX();
    m_chestY    = chest->GetPositionY();
    m_chestZ    = chest->GetPositionZ();
    m_state     = STATE_ARMED;

    sLog.outString("ArenaBrawlMgr: arena chest up on map %u (%.1f, %.1f) - armed.",
                   m_mapId, m_chestX, m_chestY);
}

void ArenaBrawlMgr::OnArenaEventEnd()
{
    if (m_state == STATE_IDLE)
        return;

    // Event window closed. If the chest is already gone, make sure the bots go
    // home; if it is still up we leave the brawl alone (the timeout will catch
    // a runaway) so contestants are not cut off prematurely.
    if (!GetChest())
        Cleanup(false);
}

void ArenaBrawlMgr::Update(uint32 diff)
{
    if (!m_enabled || m_state == STATE_IDLE)
        return;

    m_sinceCheckMs += diff;
    if (m_sinceCheckMs < IN_MILLISECONDS)       // ~1s cadence
        return;
    m_sinceCheckMs = 0;

    const time_t now = time(nullptr);

    switch (m_state)
    {
        case STATE_ARMED:
        {
            if (!GetChest())                    // chest vanished before anyone came
            {
                m_state = STATE_IDLE;
                m_chestGuid.Clear();
                return;
            }
            if (AnyRealPlayerWithin(m_triggerRadius))
                Deploy();                       // -> STATE_ACTIVE
            return;
        }
        case STATE_ACTIVE:
        {
            if (!GetChest())                    // a player looted it -> legit win
            {
                Cleanup(false);
                return;
            }
            if (now - m_eventStart >= (time_t)m_maxEventSecs)
            {
                Cleanup(true);                  // runaway -> despawn
                return;
            }
            if (!AnyRealPlayerWithin(m_arenaRadius))
            {
                m_state = STATE_GRACE;
                m_graceUntil = now + m_graceSecs;
            }
            return;
        }
        case STATE_GRACE:
        {
            if (!GetChest())
            {
                Cleanup(false);
                return;
            }
            if (now - m_eventStart >= (time_t)m_maxEventSecs)
            {
                Cleanup(true);
                return;
            }
            if (AnyRealPlayerWithin(m_arenaRadius))   // came back within grace
            {
                m_state = STATE_ACTIVE;
                return;
            }
            if (now >= m_graceUntil)
            {
                Cleanup(m_despawnOnAbandon);
                return;
            }
            return;
        }
        default:
            return;
    }
}
