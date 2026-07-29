/*
 * Rare spawn cycler - see RareSpawnMgr.h for the rationale.
 */

#include "RareSpawn/RareSpawnMgr.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Pools/PoolManager.h"
#include "Util/Util.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

INSTANTIATE_SINGLETON_1(RareSpawnMgr);

void RareSpawnMgr::Initialize()
{
    m_enabled           = sConfig.GetBoolDefault("RareSpawn.Enable", true);
    m_upTimeSecs        = sConfig.GetIntDefault("RareSpawn.UpTimeSeconds", 1800);
    m_checkIntervalSecs = sConfig.GetIntDefault("RareSpawn.CheckIntervalSeconds", 60);

    if (m_upTimeSecs < 60)
        m_upTimeSecs = 60;
    if (m_checkIntervalSecs < 5)
        m_checkIntervalSecs = 5;

    if (!m_enabled)
    {
        sLog.outString("RareSpawnMgr: disabled (RareSpawn.Enable = 0).");
        return;
    }

    LoadManagedRares();

    // Every rare is seeded "up" at startup (pool init / fresh spawn), so start
    // each one in the UP state and give it a jittered first window so the whole
    // population does not despawn in lockstep.
    const time_t now = time(nullptr);
    for (ManagedRare& rare : m_rares)
    {
        rare.state = STATE_UP;
        rare.nextFlip = now + urand(m_upTimeSecs / 2, m_upTimeSecs);
    }

    sLog.outString("RareSpawnMgr: managing %zu rares (up window %us, check every %us).",
                   m_rares.size(), m_upTimeSecs, m_checkIntervalSecs);
}

void RareSpawnMgr::LoadManagedRares()
{
    m_rares.clear();

    std::unordered_map<uint32, size_t> entryToIndex;   // pooled entry -> index in m_rares

    // --- pooled rares (silver-dragon rares are pooled by entry, max_limit 1) ---
    if (auto result = WorldDatabase.Query(
            "SELECT pct.pool_entry, ct.entry, MIN(c.map), MIN(c.spawntimesecsmin), MAX(c.spawntimesecsmax) "
            "FROM pool_creature_template pct "
            "JOIN creature_template ct ON ct.entry = pct.id "
            "JOIN creature c ON c.id = pct.id "
            "WHERE (ct.Rank IN (2,4) OR ct.entry = 2753) AND c.map IN (0,1) "
            "GROUP BY pct.pool_entry, ct.entry"))
    {
        do
        {
            Field* f = result->Fetch();
            ManagedRare rare;
            rare.pooled     = true;
            rare.poolId     = uint16(f[0].GetUInt32());
            rare.entry      = f[1].GetUInt32();
            rare.mapId      = f[2].GetUInt32();
            rare.respawnMin = f[3].GetUInt32();
            rare.respawnMax = f[4].GetUInt32();
            entryToIndex[rare.entry] = m_rares.size();
            m_rares.push_back(std::move(rare));
        } while (result->NextRow());
    }

    // member guids for pooled rares (used for the combat guard)
    if (!entryToIndex.empty())
    {
        if (auto result = WorldDatabase.Query(
                "SELECT c.id, c.guid FROM creature c "
                "JOIN pool_creature_template pct ON pct.id = c.id "
                "JOIN creature_template ct ON ct.entry = c.id "
                "WHERE (ct.Rank IN (2,4) OR ct.entry = 2753) AND c.map IN (0,1)"))
        {
            do
            {
                Field* f = result->Fetch();
                auto it = entryToIndex.find(f[0].GetUInt32());
                if (it != entryToIndex.end())
                    m_rares[it->second].members.push_back(f[1].GetUInt32());
            } while (result->NextRow());
        }
    }

    // --- non-pooled world rares (single spawns such as Setis) ---
    if (auto result = WorldDatabase.Query(
            "SELECT c.guid, c.id, c.map, c.spawntimesecsmin, c.spawntimesecsmax "
            "FROM creature c "
            "JOIN creature_template ct ON ct.entry = c.id "
            "LEFT JOIN pool_creature_template pct ON pct.id = c.id "
            "LEFT JOIN pool_creature pc ON pc.guid = c.guid "
            "WHERE ct.Rank IN (2,4) AND c.map IN (0,1) "
            "AND pct.id IS NULL AND pc.guid IS NULL"))
    {
        do
        {
            Field* f = result->Fetch();
            ManagedRare rare;
            rare.pooled     = false;
            rare.guid       = f[0].GetUInt32();
            rare.entry      = f[1].GetUInt32();
            rare.mapId      = f[2].GetUInt32();
            rare.respawnMin = f[3].GetUInt32();
            rare.respawnMax = f[4].GetUInt32();
            rare.members.push_back(rare.guid);
            m_rares.push_back(std::move(rare));
        } while (result->NextRow());
    }
}

uint32 RareSpawnMgr::RollRespawnDelay(const ManagedRare& rare) const
{
    uint32 lo = rare.respawnMin;
    uint32 hi = rare.respawnMax;
    if (hi < lo)
        std::swap(lo, hi);
    if (hi == 0)
        return 3600;                    // sane fallback if the spawn has no timer
    return urand(lo, hi);
}

bool RareSpawnMgr::AnyMemberInCombat(const ManagedRare& rare) const
{
    MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(rare.mapId, 0);
    Map* map = state ? state->GetMap() : nullptr;
    if (!map)
        return false;                   // nothing loaded -> nobody fighting it

    for (uint32 memberGuid : rare.members)
    {
        Creature* creature = map->GetCreature(memberGuid);
        if (creature && creature->IsAlive() && creature->IsInCombat())
            return true;
    }
    return false;
}

void RareSpawnMgr::FlipDown(ManagedRare& rare, time_t now)
{
    const uint32 delay = RollRespawnDelay(rare);

    if (rare.pooled)
    {
        // Removes the chosen member from the pool ledger and grid registration
        // (and the live creature if a grid happens to be loaded), so it will not
        // reappear on the next visit until we re-seed it.
        sPoolMgr.DespawnPoolInMaps(rare.poolId);
    }
    else
    {
        MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(rare.mapId, 0);
        Map* map = state ? state->GetMap() : nullptr;
        Creature* creature = map ? map->GetCreature(rare.guid) : nullptr;
        if (creature && creature->IsAlive())
            creature->ForcedDespawn();              // clean kill+respawn schedule
        else if (state)
            state->SaveCreatureRespawnTime(rare.guid, now + delay);
        else
        {
            rare.nextFlip = now + m_checkIntervalSecs;   // couldn't act, retry soon
            return;
        }
    }

    rare.state = STATE_DOWN;
    rare.nextFlip = now + delay;
    sLog.outDetail("RareSpawnMgr: DOWN entry %u (%s %u) for %us.",
                   rare.entry, rare.pooled ? "pool" : "guid",
                   rare.pooled ? rare.poolId : rare.guid, delay);
}

void RareSpawnMgr::FlipUp(ManagedRare& rare, time_t now)
{
    if (rare.pooled)
    {
        // Re-roll a member (rotates spawn point) and mark it available. On a cold
        // grid this is pure bookkeeping; if a player is present it spawns at once.
        sPoolMgr.SpawnPoolInMaps(rare.poolId, true);
    }
    else
    {
        MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(rare.mapId, 0);
        if (!state)
        {
            rare.nextFlip = now + m_checkIntervalSecs;   // retry soon
            return;
        }
        state->SaveCreatureRespawnTime(rare.guid, now);  // due now -> spawns on next visit
    }

    rare.state = STATE_UP;
    rare.nextFlip = now + m_upTimeSecs;
    sLog.outDetail("RareSpawnMgr: UP entry %u (%s %u) for %us.",
                   rare.entry, rare.pooled ? "pool" : "guid",
                   rare.pooled ? rare.poolId : rare.guid, m_upTimeSecs);
}

void RareSpawnMgr::Update(uint32 diff)
{
    if (!m_enabled || m_rares.empty())
        return;

    m_sinceCheckMs += diff;
    if (m_sinceCheckMs < m_checkIntervalSecs * IN_MILLISECONDS)
        return;
    m_sinceCheckMs = 0;

    const time_t now = time(nullptr);
    for (ManagedRare& rare : m_rares)
    {
        if (now < rare.nextFlip)
            continue;

        if (rare.state == STATE_UP)
        {
            if (AnyMemberInCombat(rare))
                continue;               // never yank a rare from a fighting player
            FlipDown(rare, now);
        }
        else
        {
            FlipUp(rare, now);
        }
    }
}
