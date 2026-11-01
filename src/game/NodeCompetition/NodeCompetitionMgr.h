/*
 * Gathering node competition.
 *
 * On a real realm herb and ore nodes rarely stay up: players and farmers take
 * them, the node sits out its respawn timer, and its pool rolls where it comes
 * back. Here the only competition is a bot that happens to pass within
 * gathering range with the right skill, so nearly every node stays up and never
 * moves. This manager simulates the missing gatherers.
 *
 * Every check it "gathers" a share of the spawned nodes on both continents and
 * brings each back after that node's own respawn time. The share is set so that,
 * at steady state, a configured fraction of nodes is up at any moment:
 * with respawn time R, a node survives on average U = A/(1-A) * R before it is
 * taken, so it is up U/(U+R) = A of the time.
 *
 * It works on the pool ledger rather than on live objects, the same way the
 * chest and rare cyclers do: taking a node removes it from its pool (and from the
 * map if the grid is loaded), bringing it back re-spawns the pool instantly, which
 * re-rolls the spawn point. That needs no GameObject::Update tick - those never
 * run in cells only bots stand in - and no database writes. Nodes being gathered
 * or near a real player are left alone, so nothing vanishes in front of anyone.
 *
 * Only pooled nodes are managed. Unpooled spawns (every Black Lotus among them)
 * keep their stock behaviour.
 */

#ifndef NODE_COMPETITION_MGR_H
#define NODE_COMPETITION_MGR_H

#include "Common.h"
#include "Policies/Singleton.h"

#include <ctime>
#include <set>
#include <vector>

struct ManagedNode
{
    uint32 guid = 0;                   // spawn db guid
    uint32 entry = 0;                  // gameobject_template entry
    uint32 mapId = 0;
    uint16 poolId = 0;                 // pool the spawn belongs to
    uint16 motherPoolId = 0;           // pool that poolId belongs to (ore spot pools), else 0
    uint32 respawnSecs = 0;            // average of spawntimesecsmin/max
};

struct PendingRefill
{
    uint16 poolId = 0;                 // pool to top up again
    uint32 mapId = 0;
    time_t due = 0;
};

class NodeCompetitionMgr
{
    public:
        NodeCompetitionMgr() = default;
        ~NodeCompetitionMgr() = default;

        void Initialize();             // read config + load the managed node list
        void Update(uint32 diff);      // called every World::Update tick

    private:
        void LoadManagedNodes();
        bool IsGatheringLock(uint32 lockId) const;
        void Refill(time_t now);
        bool Take(ManagedNode const& node, time_t now, bool seeding);

        bool m_enabled = false;
        bool m_seeded = false;         // first pass brings the world to steady state
        uint32 m_sinceCheckMs = 0;
        uint32 m_sinceReportSecs = 0;  // periodic summary in the server log

        std::set<uint32> m_excludedEntries;
        std::vector<ManagedNode> m_nodes;
        std::vector<PendingRefill> m_pending;
};

#define sNodeCompetitionMgr MaNGOS::Singleton<NodeCompetitionMgr>::Instance()

#endif
