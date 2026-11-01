/*
 * Gathering node competition - see NodeCompetitionMgr.h for the rationale.
 */

#include "NodeCompetition/NodeCompetitionMgr.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Globals/SharedDefines.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Pools/PoolManager.h"
#include "Server/DBCStores.h"
#include "Util/Util.h"

#include <algorithm>
#include <cmath>
#include <sstream>

INSTANTIATE_SINGLETON_1(NodeCompetitionMgr);

namespace
{
    struct Settings
    {
        float availability;            // steady-state share of nodes up, 0.01 - 0.99
        uint32 checkIntervalSecs;
        float playerGuardDist;
    };

    // Numbers are read on every check, so `reload config` retunes a running server.
    Settings ReadSettings()
    {
        Settings s;
        s.availability      = std::min(99, std::max(1, sConfig.GetIntDefault("NodeCompetition.Availability", 55))) / 100.0f;
        s.checkIntervalSecs = std::max(10, sConfig.GetIntDefault("NodeCompetition.CheckIntervalSeconds", 60));
        s.playerGuardDist   = std::max(0.0f, sConfig.GetFloatDefault("NodeCompetition.PlayerGuardDistance", 40.0f));
        return s;
    }

    struct MapContext
    {
        MapPersistentState* state = nullptr;
        Map* map = nullptr;
        std::vector<Player*> realPlayers;
    };
}

void NodeCompetitionMgr::Initialize()
{
    m_enabled = sConfig.GetBoolDefault("NodeCompetition.Enable", true);
    if (!m_enabled)
    {
        sLog.outString("NodeCompetitionMgr: disabled (NodeCompetition.Enable = 0).");
        return;
    }

    // default: Black Lotus (unpooled anyway, listed so it stays out if that changes)
    std::stringstream excluded(sConfig.GetStringDefault("NodeCompetition.ExcludeEntries", "176589"));
    std::string token;
    while (std::getline(excluded, token, ','))
        if (uint32 entry = std::strtoul(token.c_str(), nullptr, 10))
            m_excludedEntries.insert(entry);

    LoadManagedNodes();

    Settings const s = ReadSettings();
    sLog.outString("NodeCompetitionMgr: managing %zu pooled herb/ore nodes (availability %u%%, check every %us, player guard %.0fy, %zu entries excluded).",
                   m_nodes.size(), uint32(s.availability * 100.0f + 0.5f), s.checkIntervalSecs, s.playerGuardDist, m_excludedEntries.size());
}

bool NodeCompetitionMgr::IsGatheringLock(uint32 lockId) const
{
    LockEntry const* lock = lockId ? sLockStore.LookupEntry(lockId) : nullptr;
    if (!lock)
        return false;

    for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
        if (lock->Type[i] == LOCK_KEY_SKILL && (lock->Index[i] == LOCKTYPE_HERBALISM || lock->Index[i] == LOCKTYPE_MINING))
            return true;
    return false;
}

void NodeCompetitionMgr::LoadManagedNodes()
{
    m_nodes.clear();

    // Pooled open-world chests that are not seasonal game event spawns.
    auto result = WorldDatabase.Query(
        "SELECT g.guid, g.id, g.map, gt.data0, pg.pool_entry, g.spawntimesecsmin, g.spawntimesecsmax "
        "FROM gameobject g "
        "JOIN gameobject_template gt ON gt.entry = g.id "
        "JOIN pool_gameobject pg ON pg.guid = g.guid "
        "LEFT JOIN game_event_gameobject ge ON ge.guid = g.guid "
        "WHERE gt.type = 3 "                       // GAMEOBJECT_TYPE_CHEST (herbs and veins are chests)
        "AND g.map IN (0,1) "
        "AND ge.guid IS NULL");

    if (!result)
    {
        sLog.outString("NodeCompetitionMgr: no pooled gathering nodes found.");
        return;
    }

    uint32 multiNodeSpots = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const lockId = f[3].GetUInt32();
        if (!IsGatheringLock(lockId))
            continue;

        ManagedNode node;
        node.guid  = f[0].GetUInt32();
        node.entry = f[1].GetUInt32();
        node.mapId = f[2].GetUInt32();
        node.poolId = uint16(f[4].GetUInt32());
        if (m_excludedEntries.count(node.entry))
            continue;

        int32 const respawnMin = f[5].GetInt32();
        int32 const respawnMax = f[6].GetInt32();
        node.respawnSecs = uint32(std::max(60, (std::max(0, respawnMin) + std::max(0, respawnMax)) / 2));

        // Ore spot pools (one vein type per spot) sit inside a zone pool that decides
        // which spots are live. A spot that holds a single node is taken and returned
        // at that level, so the vein reappears at another spot in the zone.
        if (uint16 mother = sPoolMgr.IsPartOfAPool<Pool>(node.poolId))
        {
            if (sPoolMgr.GetPoolTemplate(node.poolId).MaxLimit == 1)
                node.motherPoolId = mother;
            else
                ++multiNodeSpots;       // spot holds several nodes: cycle them one by one inside it
        }

        m_nodes.push_back(node);
    }
    while (result->NextRow());

    if (multiNodeSpots)
        sLog.outString("NodeCompetitionMgr: %u nodes in multi-node spot pools are cycled per node inside their spot.", multiNodeSpots);
}

void NodeCompetitionMgr::Refill(time_t now)
{
    auto due = std::partition(m_pending.begin(), m_pending.end(),
                              [now](PendingRefill const& refill) { return refill.due > now; });

    for (auto itr = due; itr != m_pending.end(); ++itr)
    {
        MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(itr->mapId, 0);
        if (!state)
            continue;

        // Instantly: in a loaded grid the node appears now; in a cold grid this is
        // only the ledger and grid registration, and it spawns when the grid loads.
        if (sPoolMgr.GetPoolPools(itr->poolId).isEmpty())
            sPoolMgr.SpawnOneInPool<GameObject>(*state, itr->poolId, true);
        else
            sPoolMgr.SpawnOneInPool<Pool>(*state, itr->poolId, true);
    }

    m_pending.erase(due, m_pending.end());
}

bool NodeCompetitionMgr::Take(ManagedNode const& node, time_t now, bool seeding)
{
    MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(node.mapId, 0);
    if (!state)
        return false;

    PendingRefill refill;
    refill.mapId = node.mapId;
    // seeding spreads the returns over one respawn so the world does not refill in a wave
    refill.due = now + (seeding ? urand(1, node.respawnSecs) : node.respawnSecs);

    if (node.motherPoolId)
    {
        sPoolMgr.DespawnOneFromPool<Pool>(*state, node.motherPoolId, node.poolId);
        refill.poolId = node.motherPoolId;
    }
    else
    {
        sPoolMgr.DespawnOneFromPool<GameObject>(*state, node.poolId, node.guid);
        refill.poolId = node.poolId;
    }

    m_pending.push_back(refill);
    return true;
}

void NodeCompetitionMgr::Update(uint32 diff)
{
    if (!m_enabled || m_nodes.empty())
        return;

    Settings const s = ReadSettings();

    m_sinceCheckMs += diff;
    if (m_sinceCheckMs < s.checkIntervalSecs * IN_MILLISECONDS)
        return;
    uint32 const elapsedSecs = m_sinceCheckMs / IN_MILLISECONDS;
    m_sinceCheckMs = 0;

    time_t const now = time(nullptr);
    Refill(now);

    MapContext contexts[2];            // maps 0 and 1
    for (uint32 mapId = 0; mapId < 2; ++mapId)
    {
        MapContext& ctx = contexts[mapId];
        ctx.state = sMapPersistentStateMgr.GetPersistentState(mapId, 0);
        ctx.map = ctx.state ? ctx.state->GetMap() : nullptr;
        if (!ctx.map)
            continue;
        for (auto const& ref : ctx.map->GetPlayers())
            if (Player* player = ref.getSource())
                if (player->isRealPlayer())
                    ctx.realPlayers.push_back(player);
    }

    uint32 up = 0, taken = 0;
    for (ManagedNode const& node : m_nodes)
    {
        MapContext& ctx = contexts[node.mapId];
        if (!ctx.state || !ctx.state->GetSpawnedPoolData().IsSpawnedObject<GameObject>(node.guid))
            continue;                  // not the live member of its pool right now

        if (GameObject* go = ctx.map ? ctx.map->GetGameObject(node.guid) : nullptr)
        {
            // on a real gather's respawn timer, or being gathered this moment
            if (!go->IsSpawned() || go->GetLootState() != GO_READY)
                continue;

            if (s.playerGuardDist > 0.0f &&
                std::any_of(ctx.realPlayers.begin(), ctx.realPlayers.end(),
                            [go, &s](Player* player) { return go->IsWithinDist(player, s.playerGuardDist); }))
            {
                ++up;
                continue;
            }
        }
        ++up;

        // First pass: the world starts with every node up, so take the share that
        // would be down at steady state. Afterwards: chance that a node which lasts
        // U seconds on average is taken within this interval.
        float chance;
        if (!m_seeded)
            chance = 1.0f - s.availability;
        else
        {
            float const meanUpSecs = s.availability / (1.0f - s.availability) * node.respawnSecs;
            chance = 1.0f - std::exp(-float(elapsedSecs) / meanUpSecs);
        }

        if (rand_norm_f() < chance && Take(node, now, !m_seeded))
        {
            ++taken;
            --up;
        }
    }

    sLog.outDetail("NodeCompetitionMgr: %u nodes up, %u taken, %zu waiting to return.", up, taken, m_pending.size());

    // a line every 15 minutes to judge the setting by: up / (up + taken and not back yet)
    m_sinceReportSecs += elapsedSecs;
    if (!m_seeded || m_sinceReportSecs >= 15 * MINUTE)
    {
        m_sinceReportSecs = 0;
        size_t const slots = up + m_pending.size();
        sLog.outString("NodeCompetitionMgr: %u of %zu node slots up (%.1f%%, target %u%%), %zu waiting to return.",
                       up, slots, slots ? 100.0f * up / slots : 0.0f, uint32(s.availability * 100.0f + 0.5f), m_pending.size());
    }
    m_seeded = true;
}
