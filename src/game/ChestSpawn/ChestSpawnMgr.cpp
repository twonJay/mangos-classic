/*
 * Open-world chest cycler - see ChestSpawnMgr.h for the rationale.
 */

#include "ChestSpawn/ChestSpawnMgr.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "Globals/SharedDefines.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Pools/PoolManager.h"
#include "Server/DBCStores.h"
#include "Util/Util.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

INSTANTIATE_SINGLETON_1(ChestSpawnMgr);

namespace
{
    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }

    // comma-separated -> trimmed, lowercased tokens
    std::vector<std::string> ParseNameList(const std::string& raw)
    {
        std::vector<std::string> tokens;
        size_t start = 0;
        while (start <= raw.size())
        {
            const size_t comma = raw.find(',', start);
            std::string token = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const size_t first = token.find_first_not_of(" \t");
            const size_t last = token.find_last_not_of(" \t");
            if (first != std::string::npos)
                tokens.push_back(ToLower(token.substr(first, last - first + 1)));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return tokens;
    }
}

void ChestSpawnMgr::Initialize()
{
    m_enabled           = sConfig.GetBoolDefault("ChestSpawn.Enable", true);
    m_upTimeSecs        = sConfig.GetIntDefault("ChestSpawn.UpTimeSeconds", 1800);
    m_respawnMinSecs    = sConfig.GetIntDefault("ChestSpawn.RespawnMinSeconds", 7200);
    m_respawnMaxSecs    = sConfig.GetIntDefault("ChestSpawn.RespawnMaxSeconds", 21600);
    m_checkIntervalSecs = sConfig.GetIntDefault("ChestSpawn.CheckIntervalSeconds", 60);
    m_playerGuardDist   = sConfig.GetFloatDefault("ChestSpawn.PlayerGuardDistance", 50.0f);

    if (m_upTimeSecs < 60)
        m_upTimeSecs = 60;
    if (m_checkIntervalSecs < 5)
        m_checkIntervalSecs = 5;
    if (m_respawnMaxSecs < m_respawnMinSecs)
        std::swap(m_respawnMinSecs, m_respawnMaxSecs);
    if (m_playerGuardDist < 0.0f)
        m_playerGuardDist = 0.0f;

    // Only real containers are managed. GAMEOBJECT_TYPE_CHEST also covers
    // open-lock world gatherables (Sack of Oats, Giant Clam, Bloodpetal Sprout,
    // Blood of Heroes, ...) which are not chests and often carry quest loot, so
    // an empty include list would be far too wide. Empty = allow all, by design,
    // but the default is deliberately restrictive.
    m_includeNames = ParseNameList(
        sConfig.GetStringDefault("ChestSpawn.IncludeNames", "Chest,Crate,Lockbox,Strongbox,Coffer,Cache"));

    // ...minus anything on the exclusion list (default: rogue lockpicking
    // footlockers, left on their stock behaviour)
    m_excludeNames = ParseNameList(sConfig.GetStringDefault("ChestSpawn.ExcludeNames", "Footlocker"));

    if (!m_enabled)
    {
        sLog.outString("ChestSpawnMgr: disabled (ChestSpawn.Enable = 0).");
        return;
    }

    LoadManagedChests();

    // Everything is seeded "up" by the world (pool init / fresh spawn), so start
    // from the steady-state distribution instead of leaving them all up: each
    // chest keeps its up-state with probability up/(up+avgDown) for a random
    // remainder of the up window; the rest flip down on the first update pass
    // and re-stagger through their randomized 2-6h down windows. Without this,
    // every restart opened a window where all chests in the world were up at once.
    const time_t now = time(nullptr);
    const uint32 avgDown = std::max(1u, (m_respawnMinSecs + m_respawnMaxSecs) / 2);
    for (ManagedChest& chest : m_chests)
    {
        chest.state = STATE_UP;
        if (urand(0, m_upTimeSecs + avgDown - 1) < m_upTimeSecs)
            chest.nextFlip = now + urand(m_checkIntervalSecs, m_upTimeSecs);
        else
            chest.nextFlip = now;                 // flips down on the first pass
    }

    sLog.outString("ChestSpawnMgr: managing %zu chests (up window %us, respawn %u-%us, check every %us).",
                   m_chests.size(), m_upTimeSecs, m_respawnMinSecs, m_respawnMaxSecs, m_checkIntervalSecs);
}

bool ChestSpawnMgr::IsGatheringLock(uint32 lockId) const
{
    if (!lockId)
        return false;

    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock)
        return false;

    for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (lock->Type[i] != LOCK_KEY_SKILL)
            continue;
        if (lock->Index[i] == LOCKTYPE_HERBALISM || lock->Index[i] == LOCKTYPE_MINING)
            return true;                // herb node / ore vein - bots farm these already
    }
    return false;
}

bool ChestSpawnMgr::IsIncludedName(const std::string& name) const
{
    if (m_includeNames.empty())
        return true;                    // no whitelist configured -> allow all

    const std::string lowered = ToLower(name);
    for (const std::string& token : m_includeNames)
        if (!token.empty() && lowered.find(token) != std::string::npos)
            return true;
    return false;
}

bool ChestSpawnMgr::IsExcludedName(const std::string& name) const
{
    const std::string lowered = ToLower(name);
    for (const std::string& token : m_excludeNames)
        if (!token.empty() && lowered.find(token) != std::string::npos)
            return true;
    return false;
}

void ChestSpawnMgr::LoadManagedChests()
{
    m_chests.clear();

    std::unordered_map<uint16, size_t> poolToIndex;   // pool_entry -> index in m_chests

    // Candidate spawns: lootable open-world chests that are not quest objectives,
    // not tied to a seasonal game event, and not pinned to an always-up short timer
    // (those sub-minute respawns exist so a quest or script target is never missing).
    auto result = WorldDatabase.Query(
        "SELECT g.guid, g.id, g.map, gt.data0, gt.name, pg.pool_entry "
        "FROM gameobject g "
        "JOIN gameobject_template gt ON gt.entry = g.id "
        "LEFT JOIN pool_gameobject pg ON pg.guid = g.guid "
        "LEFT JOIN game_event_gameobject ge ON ge.guid = g.guid "
        "WHERE gt.type = 3 "                       // GAMEOBJECT_TYPE_CHEST
        "AND g.map IN (0,1) "                      // open world only, never dungeons
        "AND gt.data1 > 0 "                        // has a loot template
        "AND gt.data8 = 0 "                        // chest.questId - skip quest objectives
        "AND g.spawntimesecsmax >= 60 "
        "AND ge.guid IS NULL");

    if (!result)
    {
        sLog.outString("ChestSpawnMgr: no candidate chests found.");
        return;
    }

    uint32 skippedGathering = 0;
    uint32 skippedNotContainer = 0;
    uint32 skippedByName = 0;

    do
    {
        Field* f = result->Fetch();
        const uint32 guid   = f[0].GetUInt32();
        const uint32 entry  = f[1].GetUInt32();
        const uint32 mapId  = f[2].GetUInt32();
        const uint32 lockId = f[3].GetUInt32();
        const std::string name = f[4].GetCppString();
        const bool pooled   = !f[5].IsNULL();
        const uint16 poolId = pooled ? uint16(f[5].GetUInt32()) : uint16(0);

        if (IsGatheringLock(lockId))
        {
            ++skippedGathering;
            continue;
        }
        if (!IsIncludedName(name))
        {
            ++skippedNotContainer;
            continue;
        }
        if (IsExcludedName(name))
        {
            ++skippedByName;
            continue;
        }

        if (pooled)
        {
            // One managed unit per pool: the pool owns which spawn point is live,
            // so cycling the pool also rotates where the chest turns up.
            auto it = poolToIndex.find(poolId);
            if (it == poolToIndex.end())
            {
                ManagedChest chest;
                chest.pooled = true;
                chest.poolId = poolId;
                chest.entry  = entry;
                chest.mapId  = mapId;
                chest.members.push_back(guid);
                poolToIndex[poolId] = m_chests.size();
                m_chests.push_back(std::move(chest));
            }
            else
            {
                m_chests[it->second].members.push_back(guid);
            }
        }
        else
        {
            ManagedChest chest;
            chest.pooled = false;
            chest.guid   = guid;
            chest.entry  = entry;
            chest.mapId  = mapId;
            chest.members.push_back(guid);
            m_chests.push_back(std::move(chest));
        }
    }
    while (result->NextRow());

    sLog.outString("ChestSpawnMgr: skipped %u gathering nodes, %u non-containers, %u excluded by name.",
                   skippedGathering, skippedNotContainer, skippedByName);
}

uint32 ChestSpawnMgr::RollRespawnDelay() const
{
    if (m_respawnMaxSecs == 0)
        return 3600;                    // sane fallback if both bounds are cleared
    return urand(m_respawnMinSecs, m_respawnMaxSecs);
}

bool ChestSpawnMgr::IsBusy(const ManagedChest& chest) const
{
    MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(chest.mapId, 0);
    Map* map = state ? state->GetMap() : nullptr;
    if (!map)
        return false;                   // nothing loaded -> nobody near it

    for (uint32 memberGuid : chest.members)
    {
        GameObject* go = map->GetGameObject(memberGuid);
        if (!go)
            continue;                   // not spawned on this map / cold grid

        if (go->GetLootState() != GO_READY)
            return true;                // opened or being looted right now

        if (m_playerGuardDist <= 0.0f)
            continue;

        // A real player close enough to be walking up to it. Bots are ignored:
        // they never loot chests, so waiting on them would pin the chest up forever.
        for (const auto& ref : map->GetPlayers())
        {
            Player* player = ref.getSource();
            if (!player || !player->isRealPlayer())
                continue;
            if (go->IsWithinDist(player, m_playerGuardDist))
                return true;
        }
    }
    return false;
}

void ChestSpawnMgr::FlipDown(ManagedChest& chest, time_t now)
{
    const uint32 delay = RollRespawnDelay();

    if (chest.pooled)
    {
        // Clears the pool ledger and grid registration (and the live object if a
        // grid happens to be loaded). DespawnPoolInMaps does not call UpdatePool,
        // so no sibling is rolled in to replace it.
        sPoolMgr.DespawnPoolInMaps(chest.poolId);
    }
    else
    {
        MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(chest.mapId, 0);
        Map* map = state ? state->GetMap() : nullptr;
        GameObject* go = map ? map->GetGameObject(chest.guid) : nullptr;
        if (go)
            go->ForcedDespawn();
        if (state)
            state->SaveGORespawnTime(chest.guid, now + delay);
        else
        {
            chest.nextFlip = now + m_checkIntervalSecs;   // couldn't act, retry soon
            return;
        }
    }

    chest.state = STATE_DOWN;
    chest.nextFlip = now + delay;
    sLog.outDetail("ChestSpawnMgr: DOWN entry %u (%s %u) for %us.",
                   chest.entry, chest.pooled ? "pool" : "guid",
                   chest.pooled ? chest.poolId : chest.guid, delay);
}

void ChestSpawnMgr::FlipUp(ManagedChest& chest, time_t now)
{
    if (chest.pooled)
    {
        // Re-rolls which member is live, so the chest moves between its zone's
        // spawn points across cycles. Cold grid -> pure bookkeeping.
        sPoolMgr.SpawnPoolInMaps(chest.poolId, true);
    }
    else
    {
        MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(chest.mapId, 0);
        if (!state)
        {
            chest.nextFlip = now + m_checkIntervalSecs;   // retry soon
            return;
        }
        state->SaveGORespawnTime(chest.guid, now);        // due now -> spawns on next visit
    }

    chest.state = STATE_UP;
    chest.nextFlip = now + m_upTimeSecs;
    sLog.outDetail("ChestSpawnMgr: UP entry %u (%s %u) for %us.",
                   chest.entry, chest.pooled ? "pool" : "guid",
                   chest.pooled ? chest.poolId : chest.guid, m_upTimeSecs);
}

void ChestSpawnMgr::Update(uint32 diff)
{
    if (!m_enabled || m_chests.empty())
        return;

    m_sinceCheckMs += diff;
    if (m_sinceCheckMs < m_checkIntervalSecs * IN_MILLISECONDS)
        return;
    m_sinceCheckMs = 0;

    const time_t now = time(nullptr);
    for (ManagedChest& chest : m_chests)
    {
        if (now < chest.nextFlip)
            continue;

        if (chest.state == STATE_UP)
        {
            if (IsBusy(chest))
                continue;               // never yank a chest out from under a player
            FlipDown(chest, now);
        }
        else
        {
            FlipUp(chest, now);
        }
    }
}
