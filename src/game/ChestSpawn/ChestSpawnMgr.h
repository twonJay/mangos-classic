/*
 * Open-world chest cycler.
 *
 * Companion to RareSpawnMgr, for the same class of problem. A world treasure
 * chest only starts its respawn timer once it has been looted, and playerbots
 * structurally never loot chests: AddGatheringLootAction bails on
 * "loot.skillId == SKILL_NONE", which is true for every non-gathering lock. So
 * on a botted realm each chest is seeded once and then sits in GO_READY
 * forever - GameObject::Update does nothing for a ready chest whose
 * m_respawnTime is 0 - and the world fills up with permanent, untouched loot.
 *
 * This manager gives each managed chest a bounded up-window on pure wall-clock
 * time, then forces it down onto a long respawn, driven from World::Update with
 * no player, no bot and no loaded grid required.
 *
 * Scope is deliberately narrow. GAMEOBJECT_TYPE_CHEST also covers every herb
 * node and ore vein in the game, and those already cycle correctly because bots
 * DO gather them. The lock is what separates the two: anything whose lock asks
 * for Herbalism or Mining is left alone, which makes the managed set exactly the
 * complement of what bots loot. Quest-objective chests, seasonal event spawns,
 * dungeon spawns and any name on the exclusion list are skipped as well.
 */

#ifndef CHEST_SPAWN_MGR_H
#define CHEST_SPAWN_MGR_H

#include "Common.h"
#include "Policies/Singleton.h"

#include <ctime>
#include <string>
#include <vector>

struct ManagedChest
{
    bool pooled = false;
    uint16 poolId = 0;                 // pooled: pool_gameobject.pool_entry
    uint32 entry = 0;                  // gameobject_template entry (logging)
    uint32 guid = 0;                   // non-pooled: the single spawn's db guid
    uint32 mapId = 0;
    std::vector<uint32> members;       // spawn db guids; non-pooled: { guid }

    uint8 state = 0;                   // STATE_UP / STATE_DOWN
    time_t nextFlip = 0;               // wall-clock time of the next state change
};

class ChestSpawnMgr
{
    public:
        ChestSpawnMgr() = default;
        ~ChestSpawnMgr() = default;

        void Initialize();             // read config + load the managed chest list
        void Update(uint32 diff);      // called every World::Update tick

        bool IsEnabled() const { return m_enabled; }

    private:
        enum State : uint8 { STATE_UP = 0, STATE_DOWN = 1 };

        void LoadManagedChests();
        bool IsGatheringLock(uint32 lockId) const;
        bool IsIncludedName(const std::string& name) const;
        bool IsExcludedName(const std::string& name) const;
        bool IsBusy(const ManagedChest& chest) const;
        void FlipDown(ManagedChest& chest, time_t now);
        void FlipUp(ManagedChest& chest, time_t now);
        uint32 RollRespawnDelay() const;

        bool m_enabled = false;
        uint32 m_upTimeSecs = 1800;        // how long a chest stays available (30 min)
        uint32 m_respawnMinSecs = 7200;    // forced respawn floor (2 h)
        uint32 m_respawnMaxSecs = 21600;   // forced respawn ceiling (6 h)
        uint32 m_checkIntervalSecs = 60;   // how often the cycler evaluates
        float m_playerGuardDist = 50.0f;   // do not despawn near a real player (0 = off)
        uint32 m_sinceCheckMs = 0;

        std::vector<std::string> m_includeNames;   // lowercased substrings; empty = allow all
        std::vector<std::string> m_excludeNames;   // lowercased substrings

        std::vector<ManagedChest> m_chests;
};

#define sChestSpawnMgr MaNGOS::Singleton<ChestSpawnMgr>::Instance()

#endif
