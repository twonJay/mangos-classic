/*
 * Rare spawn cycler.
 *
 * On heavily-botted continents this core only ticks creature AI where a REAL
 * player is standing (bots merely load grids). A pooled world rare therefore
 * gets seeded by a passing bot and then sits "up" forever, because nothing ever
 * kills it. This manager makes rares rare again by enforcing a bounded up-window
 * on a pure wall-clock basis: it drives each rare's pool (or single spawn) up for
 * a fixed time, then forces it back onto its normal long respawn timer, entirely
 * from World::Update - no player, no bot, no loaded grid required. Reading and
 * writing pool/respawn bookkeeping never loads a grid, so idle zones stay asleep.
 */

#ifndef RARE_SPAWN_MGR_H
#define RARE_SPAWN_MGR_H

#include "Common.h"
#include "Policies/Singleton.h"

#include <ctime>
#include <vector>

struct ManagedRare
{
    bool pooled = false;
    uint16 poolId = 0;                 // pooled: entry-pool id (pool_creature_template.pool_entry)
    uint32 entry = 0;                  // creature_template entry (logging / member lookup)
    uint32 guid = 0;                   // non-pooled: the single spawn's db guid
    uint32 mapId = 0;
    uint32 respawnMin = 0;             // seconds (spawntimesecsmin)
    uint32 respawnMax = 0;             // seconds (spawntimesecsmax)
    std::vector<uint32> members;       // spawn db guids (combat guard); non-pooled: { guid }

    uint8 state = 0;                   // STATE_UP / STATE_DOWN
    time_t nextFlip = 0;               // wall-clock time of the next state change
};

class RareSpawnMgr
{
    public:
        RareSpawnMgr() = default;
        ~RareSpawnMgr() = default;

        void Initialize();             // read config + load the managed rare list
        void Update(uint32 diff);      // called every World::Update tick

        bool IsEnabled() const { return m_enabled; }

    private:
        enum State : uint8 { STATE_UP = 0, STATE_DOWN = 1 };

        void LoadManagedRares();
        void FlipDown(ManagedRare& rare, time_t now);
        void FlipUp(ManagedRare& rare, time_t now);
        bool AnyMemberInCombat(const ManagedRare& rare) const;
        uint32 RollRespawnDelay(const ManagedRare& rare) const;

        bool m_enabled = false;
        uint32 m_upTimeSecs = 1800;        // how long a rare stays available (default 30 min)
        uint32 m_checkIntervalSecs = 60;   // how often the cycler evaluates
        uint32 m_sinceCheckMs = 0;

        std::vector<ManagedRare> m_rares;
};

#define sRareSpawnMgr MaNGOS::Singleton<RareSpawnMgr>::Instance()

#endif
